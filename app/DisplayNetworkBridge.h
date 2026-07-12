#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// WebSocket-to-TCP relay: the WKWebView's vendored noVNC client speaks
// WebSocket (browsers can't open raw TCP sockets), but wayvnc speaks plain
// RFB over TCP. This bridges the two on loopback, both ends host-local:
// accepts a WebSocket connection on an OS-assigned ephemeral port, relays
// each WS message's payload bytes verbatim to/from a plain TCP connection to
// 127.0.0.1:<targetPort> (wayvnc). No guest-network translation is involved
// -- iSH has no network namespace, so a guest socket bound to 127.0.0.1 is
// the same host loopback interface this native code runs on.
//
// One bridge instance serves one guest session; DisplayViewController creates
// a fresh one per applet launch and stops it on teardown.
@interface DisplayNetworkBridge : NSObject

// Starts listening on an OS-assigned loopback port. completion fires once on
// the main queue: either (port, nil) once the listener is ready to accept
// connections, or (0, error) if it failed to start. targetPort is the guest
// wayvnc TCP port to relay accepted WebSocket connections to (not resolved
// until a client actually connects, so this call doesn't itself require
// wayvnc to be listening yet).
- (void)startRelayingToTCPPort:(uint16_t)targetPort
                     completion:(void (^)(uint16_t bridgePort, NSError * _Nullable error))completion;

// Stops listening and closes any active relayed connection(s). Safe to call
// even if -startRelayingToTCPPort:completion: never completed or failed;
// idempotent.
- (void)stop;

@end

NS_ASSUME_NONNULL_END
