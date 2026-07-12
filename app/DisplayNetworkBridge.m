#import "DisplayNetworkBridge.h"
#import <Network/Network.h>
#import <os/lock.h>

NS_ASSUME_NONNULL_BEGIN

static NSString *const DisplayNetworkBridgeErrorDomain = @"DisplayNetworkBridgeErrorDomain";

typedef NS_ENUM(NSInteger, DisplayNetworkBridgeError) {
    DisplayNetworkBridgeErrorListenerFailed = 1,
    DisplayNetworkBridgeErrorRejectedNonLoopbackPeer = 2,
};

@implementation DisplayNetworkBridge {
    dispatch_queue_t _queue;
    nw_listener_t _Nullable _listener;
    uint16_t _targetPort;
    void (^_Nullable _startCompletion)(uint16_t, NSError *_Nullable);

    // Guards exactly-once delivery of the in-flight start's completion.
    // Two independent paths can race to fire it: the normal _queue-driven
    // nw_listener success/failure callback, and a watchdog timer armed on
    // dispatch_get_main_queue() -- deliberately NOT _queue -- before any
    // Network.framework call is made. Found on-device: a run got stuck on
    // "Starting bridge…" forever with the completion never firing either
    // way; a watchdog *chained after* nw_listener_start() on the same
    // serial _queue never ran either, meaning whatever wedged still hadn't
    // returned control to that queue at all. Arming the timer on a
    // separate queue up front means it fires regardless of what's stuck.
    os_unfair_lock _completionLock;
    BOOL _completionFired;

    // Exactly one relayed session at a time (v1 scope: one Display applet
    // session). A fresh WKWebView connection (e.g. the user reloading the
    // page) tears down whatever was active first.
    nw_connection_t _Nullable _activeWSConnection;
    nw_connection_t _Nullable _activeTCPConnection;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _queue = dispatch_queue_create("com.ish-aok.display.bridge", DISPATCH_QUEUE_SERIAL);
        _completionLock = OS_UNFAIR_LOCK_INIT;
    }
    return self;
}

- (void)dealloc {
    [self stop];
}

#pragma mark - Start / stop

- (void)startRelayingToTCPPort:(uint16_t)targetPort
                     completion:(void (^)(uint16_t bridgePort, NSError *_Nullable error))completion {
    // Armed before touching _queue or Network.framework at all -- see the
    // _completionLock/_completionFired comment above.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (8.0 * NSEC_PER_SEC)),
                    dispatch_get_main_queue(), ^{
        [self _fireCompletionOnce:completion
                              port:0
                             error:[NSError errorWithDomain:DisplayNetworkBridgeErrorDomain
                                                        code:DisplayNetworkBridgeErrorListenerFailed
                                                    userInfo:@{NSLocalizedDescriptionKey:
                                                        @"Timed out starting the local relay listener"}]];
    });
    dispatch_async(_queue, ^{
        [self _startRelayingToTCPPort:targetPort completion:completion];
    });
}

- (void)_fireCompletionOnce:(void (^_Nullable)(uint16_t bridgePort, NSError *_Nullable error))completion
                        port:(uint16_t)port
                       error:(NSError *_Nullable)error {
    os_unfair_lock_lock(&_completionLock);
    BOOL alreadyFired = _completionFired;
    _completionFired = YES;
    os_unfair_lock_unlock(&_completionLock);
    if (alreadyFired || completion == nil)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        completion(port, error);
    });
}

- (void)_startRelayingToTCPPort:(uint16_t)targetPort
                      completion:(void (^)(uint16_t bridgePort, NSError *_Nullable error))completion {
    if (_listener != nil) {
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(0, [NSError errorWithDomain:DisplayNetworkBridgeErrorDomain
                                               code:DisplayNetworkBridgeErrorListenerFailed
                                           userInfo:@{NSLocalizedDescriptionKey: @"bridge already started"}]);
        });
        return;
    }

    _targetPort = targetPort;
    _startCompletion = completion;

    // Plain TCP + WebSocket framing on top, no TLS (ws://, not wss:// --
    // this never leaves loopback, and NW_PARAMETERS_DISABLE_PROTOCOL on a
    // *_secure_tcp parameters object means "no TLS", matching how wayvnc
    // itself is invoked with no encryption on the guest side).
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,
                                                                   NW_PARAMETERS_DEFAULT_CONFIGURATION);
    // Restricts the listener to the loopback interface: without some
    // restriction, nw_listener_start waits for a viable Wi-Fi/cellular path
    // before ever reaching nw_listener_state_ready, sitting in
    // nw_listener_state_waiting forever on a Wi-Fi-off/no-signal device even
    // though this listener only ever serves our own loopback WKWebView
    // (found via a device run that hung indefinitely on "Starting bridge…").
    // nw_parameters_set_local_only is the WRONG API for this despite the
    // name -- it's a *listener* option meaning "local link" (LAN/Bonjour
    // scope: peers on the same network segment), not loopback, and using it
    // here produced a listener that reached .ready almost instantly but
    // then never actually delivered the WKWebView's loopback connection to
    // -handleNewConnection: at all (confirmed via on-device logging: the
    // listener state handler fired nw_listener_state_ready, but zero
    // subsequent nw_listener_new_connection_handler calls ever happened).
    // required_interface_type=loopback is the correct, narrower restriction.
    nw_parameters_set_required_interface_type(parameters, nw_interface_type_loopback);
    nw_protocol_stack_t protocolStack = nw_parameters_copy_default_protocol_stack(parameters);
    nw_protocol_options_t wsOptions = nw_ws_create_options(nw_ws_version_13);
    // Accept every WS handshake unconditionally -- the only gate on who can
    // reach this listener at all is the loopback check in
    // -handleNewConnection: below (Network.framework's listener API has no
    // first-class "bind to 127.0.0.1 only", so this is enforced per-connection
    // instead of at bind time).
    nw_ws_options_set_client_request_handler(wsOptions, _queue, ^nw_ws_response_t(nw_ws_request_t request) {
        (void) request;
        return nw_ws_response_create(nw_ws_response_status_accept, NULL);
    });
    nw_protocol_stack_prepend_application_protocol(protocolStack, wsOptions);

    nw_listener_t listener = nw_listener_create(parameters);
    if (listener == NULL) {
        [self _failStartWithMessage:@"nw_listener_create failed"];
        return;
    }
    _listener = listener;

    nw_listener_set_queue(listener, _queue);

    __weak typeof(self) weakSelf = self;
    nw_listener_set_new_connection_handler(listener, ^(nw_connection_t connection) {
        [weakSelf handleNewConnection:connection];
    });

    nw_listener_set_state_changed_handler(listener, ^(nw_listener_state_t state, nw_error_t error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        switch (state) {
            case nw_listener_state_ready: {
                uint16_t port = nw_listener_get_port(listener);
                [strongSelf _completeStartWithPort:port error:nil];
                break;
            }
            case nw_listener_state_failed: {
                NSError *nsError = error != NULL ? (NSError *) CFBridgingRelease(nw_error_copy_cf_error(error)) : nil;
                [strongSelf _completeStartWithPort:0
                                              error:nsError ?: [NSError errorWithDomain:DisplayNetworkBridgeErrorDomain
                                                                                     code:DisplayNetworkBridgeErrorListenerFailed
                                                                                 userInfo:nil]];
                break;
            }
            default:
                break;
        }
    });

    nw_listener_start(listener);
}

- (void)_failStartWithMessage:(NSString *)message {
    [self _completeStartWithPort:0
                            error:[NSError errorWithDomain:DisplayNetworkBridgeErrorDomain
                                                       code:DisplayNetworkBridgeErrorListenerFailed
                                                   userInfo:@{NSLocalizedDescriptionKey: message}]];
}

- (void)_completeStartWithPort:(uint16_t)port error:(NSError *_Nullable)error {
    void (^completion)(uint16_t, NSError *_Nullable) = _startCompletion;
    _startCompletion = nil;
    [self _fireCompletionOnce:completion port:port error:error];
}

- (void)stop {
    dispatch_async(_queue, ^{
        [self _stop];
    });
}

- (void)_stop {
    if (_listener != nil) {
        nw_listener_cancel(_listener);
        _listener = nil;
    }
    [self _teardownActiveRelay];
    _startCompletion = nil;
}

- (void)_teardownActiveRelay {
    if (_activeWSConnection != nil) {
        nw_connection_cancel(_activeWSConnection);
        _activeWSConnection = nil;
    }
    if (_activeTCPConnection != nil) {
        nw_connection_cancel(_activeTCPConnection);
        _activeTCPConnection = nil;
    }
}

#pragma mark - Accepting a WebSocket connection

- (void)handleNewConnection:(nw_connection_t)wsConnection {
    // Defense in depth against Network.framework's listener API having no
    // first-class "loopback only" bind option: verify the peer really is
    // 127.0.0.1/::1 before doing anything else with it. This bridge only
    // ever needs to serve our own WKWebView content on the same device.
    nw_endpoint_t remoteEndpoint = nw_connection_copy_endpoint(wsConnection);
    NSString *remoteAddress = nil;
    if (remoteEndpoint != NULL) {
        char *addressString = nw_endpoint_copy_address_string(remoteEndpoint);
        if (addressString != NULL) {
            remoteAddress = [NSString stringWithUTF8String:addressString];
            free(addressString);
        }
    }
    BOOL isLoopback = [remoteAddress hasPrefix:@"127."] || [remoteAddress isEqualToString:@"::1"];
    if (!isLoopback) {
        nw_connection_cancel(wsConnection);
        return;
    }

    // A fresh connection replaces whatever was active (e.g. WKWebView reload).
    [self _teardownActiveRelay];
    _activeWSConnection = wsConnection;

    __weak typeof(self) weakSelf = self;
    nw_connection_set_queue(wsConnection, _queue);
    nw_connection_set_state_changed_handler(wsConnection, ^(nw_connection_state_t state, nw_error_t error) {
        (void) error;
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        if (state == nw_connection_state_ready) {
            [strongSelf connectToTargetForWSConnection:wsConnection];
        } else if (state == nw_connection_state_failed) {
            if (strongSelf->_activeWSConnection == wsConnection)
                [strongSelf _teardownActiveRelay];
        } else if (state == nw_connection_state_cancelled) {
            if (strongSelf->_activeWSConnection == wsConnection)
                [strongSelf _teardownActiveRelay];
        }
    });
    nw_connection_start(wsConnection);
}

- (void)connectToTargetForWSConnection:(nw_connection_t)wsConnection {
    [self connectToTargetForWSConnection:wsConnection attempt:1];
}

- (void)connectToTargetForWSConnection:(nw_connection_t)wsConnection attempt:(int)attempt {
    if (_activeWSConnection != wsConnection)
        return; // superseded by a newer connection while we were setting up

    char portString[6];
    snprintf(portString, sizeof(portString), "%u", (unsigned) _targetPort);
    nw_endpoint_t targetEndpoint = nw_endpoint_create_host("127.0.0.1", portString);
    nw_parameters_t tcpParameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,
                                                                      NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_parameters_set_required_interface_type(tcpParameters, nw_interface_type_loopback); // see the listener-side comment above
    nw_connection_t tcpConnection = nw_connection_create(targetEndpoint, tcpParameters);
    if (tcpConnection == NULL) {
        nw_connection_cancel(wsConnection);
        _activeWSConnection = nil;
        return;
    }
    _activeTCPConnection = tcpConnection;

    __weak typeof(self) weakSelf = self;
    nw_connection_set_queue(tcpConnection, _queue);
    nw_connection_set_state_changed_handler(tcpConnection, ^(nw_connection_state_t state, nw_error_t error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        if (state == nw_connection_state_ready) {
            [strongSelf beginRelayingWS:wsConnection tcp:tcpConnection];
        } else if (state == nw_connection_state_failed) {
            [strongSelf retryTargetConnection:tcpConnection ws:wsConnection attempt:attempt];
        } else if (state == nw_connection_state_cancelled) {
            // Deliberately no teardown: the retry path cancels a stale
            // connection before creating its replacement, so reacting to
            // .cancelled here would kill the WS side mid-retry.
        } else if (state == nw_connection_state_waiting) {
            // On loopback, .waiting is a dead end, not a transient state:
            // Network.framework re-attempts a waiting connection only on a
            // network-path change event, and the loopback path never
            // changes -- so a single ECONNREFUSED (wayvnc bound its port a
            // beat late, or briefly dropped it while its own Wayland
            // connection settled) would otherwise park here forever
            // (observed on-device: one instant refusal, then nothing for
            // the rest of the session). Treat it as a failure and drive
            // the retry ourselves.
            [strongSelf retryTargetConnection:tcpConnection ws:wsConnection attempt:attempt];
        }
    });
    nw_connection_start(tcpConnection);
}

- (void)retryTargetConnection:(nw_connection_t)tcpConnection
                            ws:(nw_connection_t)wsConnection
                       attempt:(int)attempt {
    if (_activeTCPConnection != tcpConnection || _activeWSConnection != wsConnection)
        return; // superseded; nothing to do
    // ~15s total (30 * 0.5s), comfortably covering wayvnc re-binding after
    // an internal compositor-reconnect cycle.
    if (attempt >= 30) {
        [self _teardownActiveRelay];
        return;
    }
    nw_connection_cancel(tcpConnection);
    _activeTCPConnection = nil;
    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (0.5 * NSEC_PER_SEC)), _queue, ^{
        [weakSelf connectToTargetForWSConnection:wsConnection attempt:attempt + 1];
    });
}

#pragma mark - Relaying

// Each pump function re-arms itself from inside its own completion handler,
// so the two directions run independently and concurrently (both scheduled
// on the same serial _queue, which is fine -- neither blocks the other since
// Network.framework's receive/send calls are themselves async).

- (void)beginRelayingWS:(nw_connection_t)wsConnection tcp:(nw_connection_t)tcpConnection {
    [self pumpWSToTCP:wsConnection tcp:tcpConnection];
    [self pumpTCPToWS:tcpConnection ws:wsConnection];
}

- (void)pumpWSToTCP:(nw_connection_t)wsConnection tcp:(nw_connection_t)tcpConnection {
    if (_activeWSConnection != wsConnection || _activeTCPConnection != tcpConnection)
        return;
    __weak typeof(self) weakSelf = self;
    nw_connection_receive_message(wsConnection, ^(dispatch_data_t _Nullable content,
                                                     nw_content_context_t _Nullable context,
                                                     bool is_complete,
                                                     nw_error_t _Nullable error) {
        (void) context;
        (void) is_complete;
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_activeWSConnection != wsConnection)
            return;
        if (error != NULL) {
            [strongSelf _teardownActiveRelay];
            return;
        }
        if (content == nil) {
            // Empty/keepalive message (e.g. a ping the OS didn't auto-reply
            // to some other way) -- nothing to forward, keep pumping.
            [strongSelf pumpWSToTCP:wsConnection tcp:tcpConnection];
            return;
        }
        nw_connection_send(tcpConnection, content, NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false,
                            ^(nw_error_t _Nullable sendError) {
            typeof(self) innerSelf = weakSelf;
            if (innerSelf == nil || innerSelf->_activeWSConnection != wsConnection)
                return;
            if (sendError != NULL) {
                [innerSelf _teardownActiveRelay];
                return;
            }
            [innerSelf pumpWSToTCP:wsConnection tcp:tcpConnection];
        });
    });
}

- (void)pumpTCPToWS:(nw_connection_t)tcpConnection ws:(nw_connection_t)wsConnection {
    if (_activeTCPConnection != tcpConnection || _activeWSConnection != wsConnection)
        return;
    __weak typeof(self) weakSelf = self;
    // 1..64KiB: forward whatever arrives promptly (RFB is latency-sensitive
    // -- don't hold frames waiting to fill a bigger buffer) but cap a single
    // relayed WS message at a sane size.
    nw_connection_receive(tcpConnection, 1, 65536, ^(dispatch_data_t _Nullable content,
                                                        nw_content_context_t _Nullable context,
                                                        bool is_complete,
                                                        nw_error_t _Nullable error) {
        (void) context;
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_activeTCPConnection != tcpConnection)
            return;
        if (error != NULL) {
            [strongSelf _teardownActiveRelay];
            return;
        }
        void (^continueOrStop)(void) = ^{
            if (is_complete) {
                // TCP "read close" (wayvnc closed its end) -- tear the whole
                // relay down rather than leaving a half-open WS connection.
                [strongSelf _teardownActiveRelay];
            } else {
                [strongSelf pumpTCPToWS:tcpConnection ws:wsConnection];
            }
        };
        if (content == nil) {
            continueOrStop();
            return;
        }
        nw_protocol_metadata_t wsMetadata = nw_ws_create_metadata(nw_ws_opcode_binary);
        nw_content_context_t sendContext = nw_content_context_create("display-bridge-frame");
        nw_content_context_set_metadata_for_protocol(sendContext, wsMetadata);
        nw_connection_send(wsConnection, content, sendContext, true, ^(nw_error_t _Nullable sendError) {
            typeof(self) innerSelf = weakSelf;
            if (innerSelf == nil || innerSelf->_activeTCPConnection != tcpConnection)
                return;
            if (sendError != NULL) {
                [innerSelf _teardownActiveRelay];
                return;
            }
            continueOrStop();
        });
    });
}

@end

NS_ASSUME_NONNULL_END
