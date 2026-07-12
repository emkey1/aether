#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Minimal loopback-only static HTTP/1.1 file server for the Display
// applet's noVNC bundle (display.html/js/css + deps/novnc). WKWebView
// enforces CORS on ES module loading (even dynamic import()) under
// file://, treating every file as an opaque cross-origin resource --
// confirmed on-device: noVNC's rfb.js module import was rejected with
// "Cross-origin script load denied" even though display.js itself,
// loaded via loadFileURL:, ran fine as a classic script. Serving
// everything from one real http://127.0.0.1:<port>/ origin instead
// sidesteps the restriction entirely.
//
// GET/HEAD only, Connection: close on every response -- no keep-alive or
// pipelining. This only ever serves our own bundled, read-only static
// assets to our own WKWebView, so that's plenty.
@interface DisplayStaticFileServer : NSObject

// Starts serving `rootURL` (and its subdirectories) on an OS-assigned
// loopback port; `completion` receives that port, or an error if the
// listener failed to start. Call once per instance.
- (void)startServingRootURL:(NSURL *)rootURL
                  completion:(void (^)(uint16_t port, NSError *_Nullable error))completion;

- (void)stop;

@end

NS_ASSUME_NONNULL_END
