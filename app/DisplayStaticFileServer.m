#import "DisplayStaticFileServer.h"
#import <Network/Network.h>

NS_ASSUME_NONNULL_BEGIN

static NSString *const DisplayStaticFileServerErrorDomain = @"DisplayStaticFileServerErrorDomain";

@implementation DisplayStaticFileServer {
    dispatch_queue_t _queue;
    nw_listener_t _Nullable _listener;
    NSURL *_rootURL;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _queue = dispatch_queue_create("com.ish-aok.display.staticserver", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)dealloc {
    [self stop];
}

#pragma mark - Start / stop

- (void)startServingRootURL:(NSURL *)rootURL
                  completion:(void (^)(uint16_t port, NSError *_Nullable error))completion {
    _rootURL = rootURL.URLByStandardizingPath ?: rootURL;
    dispatch_async(_queue, ^{
        [self _startWithCompletion:completion];
    });
}

- (void)_startWithCompletion:(void (^)(uint16_t port, NSError *_Nullable error))completion {
    if (_listener != nil) {
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(0, [NSError errorWithDomain:DisplayStaticFileServerErrorDomain
                                               code:1
                                           userInfo:@{NSLocalizedDescriptionKey: @"server already started"}]);
        });
        return;
    }

    nw_parameters_t parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,
                                                                    NW_PARAMETERS_DEFAULT_CONFIGURATION);
    // Loopback only -- see DisplayNetworkBridge.m for why this (not
    // nw_parameters_set_local_only, which means LAN/link-local scope, not
    // loopback) is the correct restriction, and why it also avoids waiting
    // on a Wi-Fi/cellular path this listener never needs.
    nw_parameters_set_required_interface_type(parameters, nw_interface_type_loopback);

    nw_listener_t listener = nw_listener_create(parameters);
    if (listener == NULL) {
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(0, [NSError errorWithDomain:DisplayStaticFileServerErrorDomain
                                               code:2
                                           userInfo:@{NSLocalizedDescriptionKey: @"nw_listener_create failed"}]);
        });
        return;
    }
    _listener = listener;
    nw_listener_set_queue(listener, _queue);

    __weak typeof(self) weakSelf = self;
    nw_listener_set_new_connection_handler(listener, ^(nw_connection_t connection) {
        [weakSelf handleConnection:connection];
    });

    __block BOOL completed = NO;
    nw_listener_set_state_changed_handler(listener, ^(nw_listener_state_t state, nw_error_t error) {
        if (completed)
            return;
        if (state == nw_listener_state_ready) {
            completed = YES;
            uint16_t port = nw_listener_get_port(listener);
            dispatch_async(dispatch_get_main_queue(), ^{
                completion(port, nil);
            });
        } else if (state == nw_listener_state_failed) {
            completed = YES;
            NSError *nsError = error != NULL ? (NSError *) CFBridgingRelease(nw_error_copy_cf_error(error)) : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                completion(0, nsError ?: [NSError errorWithDomain:DisplayStaticFileServerErrorDomain code:3 userInfo:nil]);
            });
        }
    });

    nw_listener_start(listener);
}

- (void)stop {
    dispatch_async(_queue, ^{
        if (self->_listener != nil) {
            nw_listener_cancel(self->_listener);
            self->_listener = nil;
        }
    });
}

#pragma mark - Accepting a connection

- (void)handleConnection:(nw_connection_t)connection {
    // Defense in depth, matching DisplayNetworkBridge.m: verify the peer is
    // really loopback before doing anything with it.
    nw_endpoint_t remoteEndpoint = nw_connection_copy_endpoint(connection);
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
        nw_connection_cancel(connection);
        return;
    }

    nw_connection_set_queue(connection, _queue);
    NSMutableData *buffer = [NSMutableData new];
    __weak typeof(self) weakSelf = self;
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t error) {
        (void) error;
        if (state == nw_connection_state_ready) {
            [weakSelf readRequestFromConnection:connection buffer:buffer];
        }
    });
    nw_connection_start(connection);
}

#pragma mark - Request parsing

- (void)readRequestFromConnection:(nw_connection_t)connection buffer:(NSMutableData *)buffer {
    __weak typeof(self) weakSelf = self;
    nw_connection_receive(connection, 1, 65536, ^(dispatch_data_t _Nullable content,
                                                     nw_content_context_t _Nullable context,
                                                     bool is_complete,
                                                     nw_error_t _Nullable error) {
        (void) context;
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        if (error != NULL) {
            nw_connection_cancel(connection);
            return;
        }
        if (content != nil) {
            dispatch_data_apply(content, ^bool(dispatch_data_t region, size_t offset, const void *bytes, size_t size) {
                (void) region;
                (void) offset;
                [buffer appendBytes:bytes length:size];
                return true;
            });
        }
        // A full HTTP request head ends at the blank line; our own
        // WKWebView never sends a body on GET/HEAD, so that's always the
        // full request.
        NSRange headerEnd = [buffer rangeOfData:[@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding]
                                          options:0
                                            range:NSMakeRange(0, buffer.length)];
        if (headerEnd.location != NSNotFound) {
            [strongSelf handleRequestData:buffer onConnection:connection];
            return;
        }
        if (is_complete || buffer.length > 32768) {
            nw_connection_cancel(connection);
            return;
        }
        [strongSelf readRequestFromConnection:connection buffer:buffer];
    });
}

- (void)handleRequestData:(NSData *)requestData onConnection:(nw_connection_t)connection {
    NSString *requestString = [[NSString alloc] initWithData:requestData encoding:NSUTF8StringEncoding];
    NSString *requestLine = [requestString componentsSeparatedByString:@"\r\n"].firstObject ?: @"";
    NSArray<NSString *> *parts = [requestLine componentsSeparatedByString:@" "];
    NSString *method = parts.count > 0 ? parts[0] : @"";
    NSString *rawPath = parts.count > 1 ? parts[1] : @"/";
    NSString *path = [rawPath componentsSeparatedByString:@"?"].firstObject ?: @"/";
    path = [path stringByRemovingPercentEncoding] ?: path;

    if (![method isEqualToString:@"GET"] && ![method isEqualToString:@"HEAD"]) {
        [self sendResponseStatus:405 statusText:@"Method Not Allowed" body:nil contentType:nil
                   declaredLength:0 onConnection:connection];
        return;
    }
    if (path.length == 0 || [path isEqualToString:@"/"]) {
        path = @"/display.html";
    }

    NSURL *fileURL = [self resolveFileURLForPath:path];
    NSData *fileData = fileURL != nil ? [NSData dataWithContentsOfURL:fileURL] : nil;
    if (fileURL == nil) {
        [self sendResponseStatus:403 statusText:@"Forbidden" body:nil contentType:nil
                   declaredLength:0 onConnection:connection];
        return;
    }
    if (fileData == nil) {
        [self sendResponseStatus:404 statusText:@"Not Found" body:nil contentType:nil
                   declaredLength:0 onConnection:connection];
        return;
    }
    BOOL isHead = [method isEqualToString:@"HEAD"];
    [self sendResponseStatus:200 statusText:@"OK"
                          body:(isHead ? nil : fileData)
                   contentType:[self contentTypeForPath:path]
                declaredLength:fileData.length
                  onConnection:connection];
}

// `path` always starts with "/" (parsed from an HTTP request line).
// Resolves it against the served root and verifies the result didn't
// escape the root via "../" traversal -- this only ever serves our own
// bundled read-only assets, but fail closed regardless.
- (nullable NSURL *)resolveFileURLForPath:(NSString *)path {
    NSString *relative = [path hasPrefix:@"/"] ? [path substringFromIndex:1] : path;
    NSURL *candidate = [_rootURL URLByAppendingPathComponent:relative].URLByStandardizingPath;
    NSString *rootPath = _rootURL.path;
    NSString *candidatePath = candidate.path;
    if (candidatePath == nil || rootPath == nil)
        return nil;
    if (![candidatePath isEqualToString:rootPath] && ![candidatePath hasPrefix:[rootPath stringByAppendingString:@"/"]])
        return nil;
    return candidate;
}

- (NSString *)contentTypeForPath:(NSString *)path {
    static NSDictionary<NSString *, NSString *> *map;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        map = @{
            @"html": @"text/html; charset=utf-8",
            @"js":   @"text/javascript; charset=utf-8",
            @"mjs":  @"text/javascript; charset=utf-8",
            @"css":  @"text/css; charset=utf-8",
            @"json": @"application/json; charset=utf-8",
            @"map":  @"application/json; charset=utf-8",
            @"wasm": @"application/wasm",
            @"png":  @"image/png",
            @"svg":  @"image/svg+xml",
        };
    });
    NSString *ext = path.pathExtension.lowercaseString;
    return map[ext] ?: @"application/octet-stream";
}

#pragma mark - Response

- (void)sendResponseStatus:(NSInteger)status
                  statusText:(NSString *)statusText
                        body:(nullable NSData *)body
                 contentType:(nullable NSString *)contentType
              declaredLength:(NSUInteger)declaredLength
                onConnection:(nw_connection_t)connection {
    NSMutableString *headers = [NSMutableString stringWithFormat:@"HTTP/1.1 %ld %@\r\n", (long) status, statusText];
    [headers appendString:@"Connection: close\r\n"];
    if (contentType != nil)
        [headers appendFormat:@"Content-Type: %@\r\n", contentType];
    [headers appendFormat:@"Content-Length: %lu\r\n", (unsigned long) declaredLength];
    [headers appendString:@"\r\n"];

    NSMutableData *response = [NSMutableData dataWithData:[headers dataUsingEncoding:NSUTF8StringEncoding]];
    if (body != nil)
        [response appendData:body];

    dispatch_data_t responseData = dispatch_data_create(response.bytes, response.length, _queue,
                                                          ^{ (void) response; /* keep alive until sent */ });
    nw_connection_send(connection, responseData, NW_CONNECTION_FINAL_MESSAGE_CONTEXT, true, ^(nw_error_t _Nullable error) {
        (void) error;
        nw_connection_cancel(connection);
    });
}

@end

NS_ASSUME_NONNULL_END
