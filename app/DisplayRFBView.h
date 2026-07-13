#import <MetalKit/MetalKit.h>
#import "DisplayRFBClient.h"

NS_ASSUME_NONNULL_BEGIN

// Renders a DisplayRFBClient's framebuffer via Metal (not CALayer.contents +
// CGImage -- that composites on the GPU too, but the per-frame CGImage/
// CGDataProvider upload path is opaque about when/whether it actually lands
// in an IOSurface; going through Metal explicitly guarantees the upload-to-
// texture and the compositing/scaling are both genuinely GPU-executed) and
// forwards touch/keyboard input to it as RFB Pointer/Key events. Replaces
// the WKWebView + vendored noVNC in DisplayViewController.
//
// Fully on-demand: no continuous render loop. -setNeedsDisplay (called by
// DisplayViewController from -rfbClientDidUpdateFramebuffer:) schedules
// exactly one -drawInMTKView: at the next vsync, coalescing any additional
// calls that land before then.
@interface DisplayRFBView : MTKView

// Set once, after the client has connected (framebufferWidth/Height must
// already be known). Reading pixels/sending input both go through this.
@property (nonatomic, nullable) DisplayRFBClient *rfbClient;

- (instancetype)initWithFrame:(CGRect)frameRect;

// Forward DisplayRFBClientDelegate's cursor callback here. Renders the
// cursor as a small overlay positioned at the last coordinates sent via
// touch input (RFB doesn't push cursor position, only shape/hotspot).
- (void)updateCursorWithWidth:(uint16_t)width height:(uint16_t)height
                      hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra;

@end

NS_ASSUME_NONNULL_END
