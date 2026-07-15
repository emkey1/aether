#import "DisplayRFBView.h"

NS_ASSUME_NONNULL_BEGIN

@interface DisplayRFBView () <MTKViewDelegate, UIKeyInput>
@end

@implementation DisplayRFBView {
    id<MTLCommandQueue> _commandQueue;
    id<MTLRenderPipelineState> _pipelineState;
    id<MTLTexture> _Nullable _texture;
    NSMutableArray<UIKeyCommand *> *_Nullable _keyCommands;

    UIImageView *_Nullable _cursorView;
    CGSize _cursorImageSize;
    CGPoint _cursorHotspot;
    CGPoint _lastPointerViewPoint; // last touch location, in this view's own coordinate space
}

- (instancetype)initWithFrame:(CGRect)frameRect {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:frameRect device:device];
    if (self != nil) {
        self.translatesAutoresizingMaskIntoConstraints = NO;
        self.delegate = self;
        // Fully on-demand rendering: draw only when a frame actually
        // arrived, not on a continuous display-link tick. setNeedsDisplay
        // coalesces any calls that land within the same vsync window into a
        // single -drawInMTKView:, which is the mechanism by which this
        // should beat noVNC's per-rect canvas putImageData in a WebView.
        self.enableSetNeedsDisplay = YES;
        self.paused = YES;
        self.framebufferOnly = YES;
        self.backgroundColor = UIColor.blackColor;
        self.layer.cornerRadius = 10.0;
        self.layer.masksToBounds = YES;
        self.multipleTouchEnabled = NO;
        // Trackpad/mouse movement without a button held never generates
        // touch events at all (only click-and-drag does, via iPadOS's
        // touch-compatibility synthesis) -- this is the only way to track
        // the remote cursor to plain hover movement.
        [self addGestureRecognizer:[[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(handleHover:)]];

        _commandQueue = [device newCommandQueue];
        id<MTLLibrary> library = [device newDefaultLibrary];
        id<MTLFunction> vertexFn = [library newFunctionWithName:@"display_rfb_vertex"];
        id<MTLFunction> fragmentFn = [library newFunctionWithName:@"display_rfb_fragment"];
        // A nil function here (e.g. DisplayRFBShaders.metal not actually
        // compiled into this bundle's default.metallib) produces an invalid
        // MTLRenderPipelineDescriptor -- Metal API Validation treats that as
        // a hard abort, not a graceful nil+error return, so this must be
        // caught before ever calling newRenderPipelineStateWithDescriptor:.
        if (vertexFn != nil && fragmentFn != nil) {
            MTLRenderPipelineDescriptor *pipelineDescriptor = [MTLRenderPipelineDescriptor new];
            pipelineDescriptor.vertexFunction = vertexFn;
            pipelineDescriptor.fragmentFunction = fragmentFn;
            pipelineDescriptor.colorAttachments[0].pixelFormat = self.colorPixelFormat;
            NSError *error = nil;
            _pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        }
    }
    return self;
}

#pragma mark - MTKViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void) view;
    (void) size;
}

- (void)drawInMTKView:(MTKView *)view {
    DisplayRFBClient *client = _rfbClient;
    uint16_t width = client.framebufferWidth;
    uint16_t height = client.framebufferHeight;
    const uint8_t *bytes = client.framebufferBytes;
    if (client == nil || width == 0 || height == 0 || bytes == NULL)
        return;

    [self ensureTextureWithWidth:width height:height];

    // Upload only what actually changed -- client.dirtyRect is the bounding
    // box of every rect in the completed update, which for typical "mostly
    // static desktop, small localized change" traffic (cursor blink, a text
    // cursor, a small window redraw) is far smaller than the full
    // framebuffer. bytesPerRow stays the FULL row stride even though we're
    // uploading a sub-region -- replaceRegion reads regionWidth*4 bytes per
    // row and advances by bytesPerRow between rows, so this reads a strided
    // sub-rectangle straight out of the full buffer with no extra copy.
    CGRect dirty = CGRectIntegral(CGRectIntersection(client.dirtyRect, CGRectMake(0, 0, width, height)));
    if (CGRectIsNull(dirty) || CGRectIsEmpty(dirty)) {
        // Nothing in the texture actually changed (e.g. an update that was
        // purely a cursor rect, handled separately as an overlay) -- no GPU
        // work needed, just keep the RFB pull loop going.
        [client acknowledgeFramebufferRead];
        return;
    }
    size_t stride = (size_t) width * 4;
    NSUInteger originX = (NSUInteger) dirty.origin.x;
    NSUInteger originY = (NSUInteger) dirty.origin.y;
    const uint8_t *regionStart = bytes + (size_t) originY * stride + (size_t) originX * 4;
    MTLRegion region = MTLRegionMake2D(originX, originY, (NSUInteger) dirty.size.width, (NSUInteger) dirty.size.height);
    [_texture replaceRegion:region mipmapLevel:0 withBytes:regionStart bytesPerRow:stride];
    // Safe the instant replaceRegion returns: Metal has its own copy of the
    // pixel data by then, so the client can start overwriting its buffer
    // with the next update immediately -- no need to wait for the encode/
    // present below to actually finish on the GPU.
    [client acknowledgeFramebufferRead];
    if (_pipelineState == nil)
        return; // shader pipeline failed to build; keep the RFB session alive without rendering

    id<CAMetalDrawable> drawable = self.currentDrawable;
    MTLRenderPassDescriptor *pass = self.currentRenderPassDescriptor;
    if (drawable == nil || pass == nil)
        return;
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:_pipelineState];
    [encoder setFragmentTexture:_texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
}

- (void)ensureTextureWithWidth:(uint16_t)width height:(uint16_t)height {
    if (_texture != nil && _texture.width == width && _texture.height == height)
        return;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    _texture = [self.device newTextureWithDescriptor:descriptor];
}

#pragma mark - Pointer input

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self becomeFirstResponder];
    [self sendPointerEventFromTouches:touches down:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self sendPointerEventFromTouches:touches down:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self sendPointerEventFromTouches:touches down:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self sendPointerEventFromTouches:touches down:NO];
}

- (void)sendPointerEventFromTouches:(NSSet<UITouch *> *)touches down:(BOOL)down {
    UITouch *touch = touches.anyObject;
    if (touch == nil)
        return;
    [self sendPointerEventAtViewPoint:[touch locationInView:self] buttonMask:down ? 0x01 : 0x00];
}

// Shared by both actual touch (finger down/drag) and hover (trackpad/mouse
// moved without a button held -- see -handleHover: below). Without the
// hover path, a connected trackpad/mouse only ever updates the remote
// pointer on click-and-drag (iPadOS synthesizes touch events for that), so
// just moving the pointer around does nothing at all -- the remote cursor
// sits wherever the last click left it instead of tracking live movement.
- (void)sendPointerEventAtViewPoint:(CGPoint)point buttonMask:(uint8_t)buttonMask {
    uint16_t fbWidth = _rfbClient.framebufferWidth;
    uint16_t fbHeight = _rfbClient.framebufferHeight;
    if (_rfbClient == nil || fbWidth == 0 || fbHeight == 0)
        return;
    CGSize viewSize = self.bounds.size;
    if (viewSize.width <= 0 || viewSize.height <= 0)
        return;
    CGFloat scaleX = (CGFloat) fbWidth / viewSize.width;
    CGFloat scaleY = (CGFloat) fbHeight / viewSize.height;
    uint16_t x = (uint16_t) MAX(0.0, MIN((CGFloat) (fbWidth - 1), point.x * scaleX));
    uint16_t y = (uint16_t) MAX(0.0, MIN((CGFloat) (fbHeight - 1), point.y * scaleY));
    [_rfbClient sendPointerEventAtX:x y:y buttonMask:buttonMask];
    _lastPointerViewPoint = point;
    [self repositionCursor];
}

- (void)handleHover:(UIHoverGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateChanged && recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self sendPointerEventAtViewPoint:[recognizer locationInView:self] buttonMask:0x00];
}

#pragma mark - Cursor overlay

// RFB doesn't push cursor position, only shape/hotspot (via the Cursor
// pseudo-encoding) -- position is tracked here from the same touch-driven
// pointer events we send to the server, so the overlay follows wherever we
// last told the server the pointer is.

- (UIImageView *)cursorView {
    if (_cursorView == nil) {
        _cursorView = [[UIImageView alloc] initWithFrame:CGRectZero];
        _cursorView.hidden = YES;
        _cursorView.userInteractionEnabled = NO; // never steals touches meant for the desktop
        [self addSubview:_cursorView];
    }
    return _cursorView;
}

- (void)updateCursorWithWidth:(uint16_t)width height:(uint16_t)height
                      hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra {
    if (width == 0 || height == 0) {
        _cursorView.hidden = YES;
        return;
    }
    if (bgra.length != (NSUInteger) width * height * 4)
        return;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef) bgra);
    CGImageRef image = CGImageCreate(width, height, 8, 32, (size_t) width * 4, colorSpace,
                                      kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst,
                                      provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    if (image == NULL)
        return;
    self.cursorView.image = [UIImage imageWithCGImage:image];
    CGImageRelease(image);
    _cursorImageSize = CGSizeMake(width, height);
    _cursorHotspot = CGPointMake(hotspotX, hotspotY);
    self.cursorView.hidden = NO;
    [self repositionCursor];
}

- (void)repositionCursor {
    if (_cursorView == nil || _cursorView.hidden)
        return;
    uint16_t fbWidth = _rfbClient.framebufferWidth;
    uint16_t fbHeight = _rfbClient.framebufferHeight;
    CGSize viewSize = self.bounds.size;
    if (fbWidth == 0 || fbHeight == 0 || viewSize.width <= 0 || viewSize.height <= 0)
        return;
    CGFloat scaleX = viewSize.width / (CGFloat) fbWidth;
    CGFloat scaleY = viewSize.height / (CGFloat) fbHeight;
    CGSize viewImageSize = CGSizeMake(_cursorImageSize.width * scaleX, _cursorImageSize.height * scaleY);
    CGPoint origin = CGPointMake(_lastPointerViewPoint.x - _cursorHotspot.x * scaleX,
                                  _lastPointerViewPoint.y - _cursorHotspot.y * scaleY);
    _cursorView.frame = CGRectMake(origin.x, origin.y, viewImageSize.width, viewImageSize.height);
}

#pragma mark - Keyboard input

- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (BOOL)hasText {
    return YES;
}

- (void)insertText:(NSString *)text {
    if (_rfbClient == nil)
        return;
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar ch = [text characterAtIndex:i];
        // X11 keysyms equal the Unicode code point for the printable
        // Latin-1 range, which covers normal typing; anything outside that
        // range is out of scope for v1 (matches typing through a US/Latin-1
        // layout, same practical coverage noVNC's default input path had).
        uint32_t keysym = ch == '\n' ? 0xFF0D /* Return */ : (uint32_t) ch;
        [_rfbClient sendKeyEvent:keysym down:YES];
        [_rfbClient sendKeyEvent:keysym down:NO];
    }
}

- (void)deleteBackward {
    if (_rfbClient == nil)
        return;
    [_rfbClient sendKeyEvent:0xFF08 down:YES]; // BackSpace
    [_rfbClient sendKeyEvent:0xFF08 down:NO];
}

- (nullable NSArray<UIKeyCommand *> *)keyCommands {
    if (_keyCommands != nil)
        return _keyCommands;
    _keyCommands = [NSMutableArray new];
    [self addSpecialKeyWithInput:UIKeyInputUpArrow keysym:0xFF52];
    [self addSpecialKeyWithInput:UIKeyInputDownArrow keysym:0xFF54];
    [self addSpecialKeyWithInput:UIKeyInputLeftArrow keysym:0xFF51];
    [self addSpecialKeyWithInput:UIKeyInputRightArrow keysym:0xFF53];
    [self addSpecialKeyWithInput:UIKeyInputEscape keysym:0xFF1B];
    [self addSpecialKeyWithInput:@"\t" keysym:0xFF09];
    // Ctrl+<key> (Ctrl+C, Ctrl+D, Ctrl+Z, ...): not covered by UIKeyInput
    // at all -- iOS only routes plain character insertion through
    // -insertText:, not modified combinations, so without an explicit
    // UIKeyCommand per key these are silently swallowed before ever
    // reaching the RFB session. Besides the letters, cover digits and the
    // characters terminal font zooming uses: foot binds font-increase to
    // Control+plus/Control+equal, font-decrease to Control+minus, and
    // font-reset to Control+0 by default ("+" is registered as its own
    // input alongside "=" because a hardware keyboard reports the shifted
    // character itself for Ctrl+Shift+=; every keysym here equals its
    // ASCII value, so handleControlKeyCommand needs no special cases).
    static const char *controlLetters = "abcdefghijklmnopqrstuvwxyz0123456789=+-_";
    for (size_t i = 0; controlLetters[i] != '\0'; i++) {
        NSString *letter = [NSString stringWithFormat:@"%c", controlLetters[i]];
        UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:letter
                                                    modifierFlags:UIKeyModifierControl
                                                           action:@selector(handleControlKeyCommand:)];
        if (@available(iOS 15, *))
            command.wantsPriorityOverSystemBehavior = YES;
        [_keyCommands addObject:command];
    }
    // Alt+<letter> and Alt+Shift+<letter>: sway's $mod is Alt (Mod1), not
    // Control -- Control has to stay free for the terminal/app-level Ctrl
    // combos above (Ctrl+C etc.), so a window-manager modifier would collide
    // with those if it also used Control. Same UIKeyInput gap as Control:
    // iOS never routes modified combinations through -insertText:.
    for (size_t i = 0; controlLetters[i] != '\0'; i++) {
        NSString *letter = [NSString stringWithFormat:@"%c", controlLetters[i]];
        UIKeyCommand *altCommand = [UIKeyCommand keyCommandWithInput:letter
                                                       modifierFlags:UIKeyModifierAlternate
                                                              action:@selector(handleAltKeyCommand:)];
        UIKeyCommand *altShiftCommand = [UIKeyCommand keyCommandWithInput:letter
                                                            modifierFlags:UIKeyModifierAlternate | UIKeyModifierShift
                                                                   action:@selector(handleAltShiftKeyCommand:)];
        if (@available(iOS 15, *)) {
            altCommand.wantsPriorityOverSystemBehavior = YES;
            altShiftCommand.wantsPriorityOverSystemBehavior = YES;
        }
        [_keyCommands addObject:altCommand];
        [_keyCommands addObject:altShiftCommand];
    }
    // Alt+Return: sway's new-terminal binding.
    UIKeyCommand *altReturn = [UIKeyCommand keyCommandWithInput:@"\r"
                                                  modifierFlags:UIKeyModifierAlternate
                                                         action:@selector(handleAltKeyCommand:)];
    if (@available(iOS 15, *))
        altReturn.wantsPriorityOverSystemBehavior = YES;
    [_keyCommands addObject:altReturn];
    return _keyCommands;
}

- (void)handleControlKeyCommand:(UIKeyCommand *)command {
    NSString *input = command.input;
    if (input.length == 0 || _rfbClient == nil)
        return;
    static const uint32_t keysymControlL = 0xFFE3;
    uint32_t keysym = (uint32_t) [input characterAtIndex:0];
    [_rfbClient sendKeyEvent:keysymControlL down:YES];
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [_rfbClient sendKeyEvent:keysymControlL down:NO];
}

// -input's "\r" (Alt+Return) needs the Return keysym, not the literal
// carriage-return character value; everything else here is a plain letter,
// where the X11 keysym equals its ASCII value.
static uint32_t DisplayRFBKeysymForKeyCommandInput(NSString *input) {
    if ([input isEqualToString:@"\r"])
        return 0xFF0D; // Return
    return (uint32_t) [input characterAtIndex:0];
}

- (void)handleAltKeyCommand:(UIKeyCommand *)command {
    NSString *input = command.input;
    if (input.length == 0 || _rfbClient == nil)
        return;
    static const uint32_t keysymAltL = 0xFFE9;
    uint32_t keysym = DisplayRFBKeysymForKeyCommandInput(input);
    [_rfbClient sendKeyEvent:keysymAltL down:YES];
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [_rfbClient sendKeyEvent:keysymAltL down:NO];
}

- (void)handleAltShiftKeyCommand:(UIKeyCommand *)command {
    NSString *input = command.input;
    if (input.length == 0 || _rfbClient == nil)
        return;
    static const uint32_t keysymAltL = 0xFFE9;
    static const uint32_t keysymShiftL = 0xFFE1;
    uint32_t keysym = DisplayRFBKeysymForKeyCommandInput(input);
    [_rfbClient sendKeyEvent:keysymAltL down:YES];
    [_rfbClient sendKeyEvent:keysymShiftL down:YES];
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [_rfbClient sendKeyEvent:keysymShiftL down:NO];
    [_rfbClient sendKeyEvent:keysymAltL down:NO];
}

// Mirrors TerminalView's addFunctionKey: pattern (stashing the payload via
// propertyList: rather than an associated object) but targets an RFB keysym
// instead of a terminal escape sequence.
- (void)addSpecialKeyWithInput:(NSString *)input keysym:(uint32_t)keysym {
    UIKeyCommand *command = [UIKeyCommand commandWithTitle:@""
                                                      image:nil
                                                     action:@selector(handleSpecialKeyCommand:)
                                                      input:input
                                              modifierFlags:0
                                               propertyList:@(keysym)];
    if (@available(iOS 15, *))
        command.wantsPriorityOverSystemBehavior = YES;
    [_keyCommands addObject:command];
}

- (void)handleSpecialKeyCommand:(UIKeyCommand *)command {
    NSNumber *keysymNumber = command.propertyList;
    if (![keysymNumber isKindOfClass:NSNumber.class] || _rfbClient == nil)
        return;
    uint32_t keysym = keysymNumber.unsignedIntValue;
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
}

@end

NS_ASSUME_NONNULL_END
