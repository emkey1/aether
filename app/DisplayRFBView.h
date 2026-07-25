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

// When YES, -inputAccessoryView returns nil and the owner is expected to
// place -accessoryBarView in its own view hierarchy instead. Standalone
// Display mode does this: UIKit's keyboard-host window (which hosts a real
// inputAccessoryView) repositions the bar to clear the home indicator
// whenever the indicator becomes visible -- and never lowers it again once
// the indicator auto-hides -- so the only way to keep the strip genuinely
// flush at the physical bottom of a fullscreen surface is to own its
// placement outright.
@property (nonatomic) BOOL accessoryBarExternallyHosted;

// The accessory key strip (esc/tab/ctrl/alt/super/arrows), lazily built.
// Same view -inputAccessoryView vends when not externally hosted.
- (UIView *)accessoryBarView;

- (instancetype)initWithFrame:(CGRect)frameRect;

// Forward DisplayRFBClientDelegate's cursor callback here. Renders the
// cursor as a small overlay positioned at the last coordinates sent via
// touch input (RFB doesn't push cursor position, only shape/hotspot).
- (void)updateCursorWithWidth:(uint16_t)width height:(uint16_t)height
                      hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra;

@end

NS_ASSUME_NONNULL_END
