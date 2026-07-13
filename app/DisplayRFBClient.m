#import "DisplayRFBClient.h"
#import <Network/Network.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(uint8_t, DisplayRFBServerMessageType) {
    DisplayRFBServerMessageFramebufferUpdate = 0,
    DisplayRFBServerMessageSetColourMapEntries = 1,
    DisplayRFBServerMessageBell = 2,
    DisplayRFBServerMessageServerCutText = 3,
};

typedef NS_ENUM(uint8_t, DisplayRFBClientMessageType) {
    DisplayRFBClientMessageSetPixelFormat = 0,
    DisplayRFBClientMessageSetEncodings = 2,
    DisplayRFBClientMessageFramebufferUpdateRequest = 3,
    DisplayRFBClientMessageKeyEvent = 4,
    DisplayRFBClientMessagePointerEvent = 5,
    DisplayRFBClientMessageClientCutText = 6,
};

static const int32_t DisplayRFBEncodingRaw = 0;
static const int32_t DisplayRFBEncodingCopyRect = 1;
static const int32_t DisplayRFBEncodingCursor = -239;

static NSString *rfb_hex_dump(const uint8_t *bytes, size_t length) {
    NSMutableString *hex = [NSMutableString stringWithCapacity:length * 3];
    for (size_t i = 0; i < length; i++)
        [hex appendFormat:i == 0 ? @"%02x" : @" %02x", bytes[i]];
    return hex;
}

// dispatch_data_create_map's returned pointer (network-provided bytes) and
// the stack-allocated uint8_t message buffers we build for sending are both
// only byte-aligned -- reinterpreting `&buf[N]` as a uint16_t*/uint32_t* and
// dereferencing it is undefined behavior in C (works by luck on some
// architectures/optimization levels, silently reads/writes the wrong value
// on others). Route every multi-byte RFB field through these instead.
static inline uint16_t rfb_read_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

static inline uint32_t rfb_read_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return ntohl(v);
}

static inline void rfb_write_u16(uint8_t *p, uint16_t hostValue) {
    uint16_t be = htons(hostValue);
    memcpy(p, &be, sizeof(be));
}

static inline void rfb_write_u32(uint8_t *p, uint32_t hostValue) {
    uint32_t be = htonl(hostValue);
    memcpy(p, &be, sizeof(be));
}

@implementation DisplayRFBClient {
    dispatch_queue_t _queue;
    nw_connection_t _Nullable _connection;
    uint16_t _guestPort;
    BOOL _didFail;
    BOOL _connected;
    // Set right before notifying the delegate of a completed FramebufferUpdate,
    // cleared at the start of -acknowledgeFramebufferRead. Guards against a
    // spurious/premature -acknowledgeFramebufferRead call (e.g. MTKView firing
    // one -drawInMTKView: automatically on first layout, before any real RFB
    // update has actually completed -- the framebuffer buffer already exists
    // by then, non-null/non-zero, so a bytes/size guard alone doesn't catch
    // it) from sending a second FramebufferUpdateRequest and starting a second,
    // concurrent read loop that races the legitimate one on the same stream.
    BOOL _hasPendingFrame;

    uint8_t *_Nullable _framebuffer; // BGRA8888, framebufferWidth*framebufferHeight*4
    NSString *_Nullable _lastUpdateHeaderHex; // diagnostic: raw pad+count bytes of the in-progress FramebufferUpdate
    CGRect _dirtyRect; // bounding box of everything changed since the last acknowledge; CGRectNull = nothing yet
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _queue = dispatch_queue_create("com.ish-aok.display.rfb", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)dealloc {
    // Deliberately NOT routed through -disconnect, which dispatch_asyncs a
    // block that implicitly captures self via `self->_connection` -- naming
    // self inside a block created from -dealloc is undefined behavior
    // (self's retain count is already zero; the block re-inflating it and
    // later releasing it can trigger a second, reentrant -dealloc, or
    // release already-torn-down ivars). Found via a device crash: EXC_BAD_
    // ACCESS inside a block-destroy helper on this exact queue. Copying the
    // connection out by value and never naming self in the block avoids
    // that entirely -- dispatch_async (not sync) so this can't deadlock if
    // dealloc happens to run on _queue's own thread (e.g. it was the block
    // that dropped the last strong reference to self).
    nw_connection_t connectionToCancel = _connection;
    dispatch_queue_t queue = _queue;
    if (connectionToCancel != nil) {
        dispatch_async(queue, ^{
            nw_connection_cancel(connectionToCancel);
        });
    }
    free(_framebuffer);
}

#pragma mark - Connect / disconnect

- (void)connectToGuestPort:(uint16_t)guestPort {
    _guestPort = guestPort;
    dispatch_async(_queue, ^{
        [self _connectAttempt:1];
    });
}

- (void)disconnect {
    dispatch_async(_queue, ^{
        if (self->_connection != nil) {
            nw_connection_cancel(self->_connection);
            self->_connection = nil;
        }
    });
}

- (void)_connectAttempt:(int)attempt {
    char portString[6];
    snprintf(portString, sizeof(portString), "%u", (unsigned) _guestPort);
    nw_endpoint_t endpoint = nw_endpoint_create_host("127.0.0.1", portString);
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,
                                                                   NW_PARAMETERS_DEFAULT_CONFIGURATION);
    // Same reasoning as DisplayNetworkBridge: without this, a listener-side
    // Wi-Fi-off device would wait forever; here on the client side it keeps
    // us from ever trying to route this off the loopback interface.
    nw_parameters_set_required_interface_type(parameters, nw_interface_type_loopback);
    nw_connection_t connection = nw_connection_create(endpoint, parameters);
    if (connection == NULL) {
        [self _failWithMessage:@"nw_connection_create failed"];
        return;
    }
    _connection = connection;
    nw_connection_set_queue(connection, _queue);

    __weak typeof(self) weakSelf = self;
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t error) {
        (void) error;
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_connection != connection)
            return;
        if (state == nw_connection_state_ready) {
            [strongSelf _beginHandshake];
        } else if (state == nw_connection_state_failed || state == nw_connection_state_waiting) {
            // On loopback .waiting is a dead end (see DisplayNetworkBridge's
            // longer comment on this) -- wayvnc may not have bound its port
            // yet, or briefly dropped it while its own Wayland connection
            // settled, so drive the retry ourselves rather than trust
            // Network.framework to ever re-attempt a loopback path.
            [strongSelf _retryConnectAttempt:attempt connection:connection];
        }
    });
    nw_connection_start(connection);
}

- (void)_retryConnectAttempt:(int)attempt connection:(nw_connection_t)connection {
    if (_connection != connection)
        return;
    // 120 * 0.5s = 60s. By the time we get here, DisplayViewController has
    // already confirmed via the ready file that wayvnc reported itself
    // listening, so this is normally just covering the same brief re-bind
    // window DisplayNetworkBridge's original 30-attempt/15s budget covered
    // -- but on a slow/CPU-starved device (heavier JIT overhead, older
    // hardware), the whole guest can simply be slower to actually accept a
    // connection even after reporting ready, so this needs real headroom
    // rather than the tight window tuned for a healthy system.
    if (attempt >= 120) {
        [self _failWithMessage:@"Timed out connecting to wayvnc"];
        return;
    }
    nw_connection_cancel(connection);
    _connection = nil;
    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (0.5 * NSEC_PER_SEC)), _queue, ^{
        [weakSelf _connectAttempt:attempt + 1];
    });
}

- (void)_failWithMessage:(NSString *)message {
    if (_didFail)
        return;
    _didFail = YES;
    if (_connection != nil) {
        nw_connection_cancel(_connection);
        _connection = nil;
    }
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        [strongSelf.delegate rfbClient:strongSelf didFailWithMessage:message];
    });
}

#pragma mark - Reading fixed-size chunks

// RFB messages are not aligned to TCP segment boundaries, but every piece we
// need to read is a known, fixed size (the framing tells us the size of what
// follows before we read it) -- so instead of hand-rolling a byte
// accumulator, we lean on nw_connection_receive's minimum/maximum length
// both being set to the exact size we want: it won't invoke the completion
// handler until that many bytes are available (or the connection errors/
// closes), which is precisely rfb_shot.py's blocking rd(n) helper translated
// into Network.framework's async idiom.
- (void)_readExactly:(uint32_t)length completion:(void (^)(const uint8_t *bytes))completion {
    if (_connection == nil)
        return;
    nw_connection_t connection = _connection;
    nw_connection_receive(connection, length, length,
        ^(dispatch_data_t _Nullable content, nw_content_context_t _Nullable context,
          bool is_complete, nw_error_t _Nullable error) {
        (void) context;
        if (self->_connection != connection)
            return;
        if (error != NULL) {
            [self _failWithMessage:@"Connection error while reading from wayvnc"];
            return;
        }
        if (content == nil || dispatch_data_get_size(content) < length) {
            [self _failWithMessage:is_complete ? @"wayvnc closed the connection" : @"Short read from wayvnc"];
            return;
        }
        const void *bytes = NULL;
        size_t bytesLength = 0;
        dispatch_data_t mapped = dispatch_data_create_map(content, &bytes, &bytesLength);
        (void) mapped; // kept alive for the duration of this call by ARC
        completion((const uint8_t *) bytes);
    });
}

#pragma mark - Handshake

- (void)_beginHandshake {
    [self _readExactly:12 completion:^(const uint8_t *bytes) {
        // Server protocol version banner, e.g. "RFB 003.008\n". We only ever
        // speak 3.8 regardless of what's offered -- neatvnc/wayvnc supports it.
        (void) bytes;
        static const uint8_t clientVersion[12] = "RFB 003.008\n";
        nw_connection_send(self->_connection,
                            dispatch_data_create(clientVersion, sizeof(clientVersion), self->_queue,
                                                  DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                            NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable error) {
            if (error != NULL) {
                [self _failWithMessage:@"Failed to send protocol version"];
                return;
            }
            [self _readSecurityTypes];
        });
    }];
}

- (void)_readSecurityTypes {
    [self _readExactly:1 completion:^(const uint8_t *bytes) {
        uint8_t count = bytes[0];
        if (count == 0) {
            // RFB 3.7/3.8 "connection failed" variant: a 4-byte reason
            // length followed by the reason text, instead of a type list.
            [self _readExactly:4 completion:^(const uint8_t *lenBytes) {
                uint32_t reasonLength = rfb_read_u32(lenBytes);
                [self _readExactly:reasonLength completion:^(const uint8_t *reasonBytes) {
                    NSString *reason = [[NSString alloc] initWithBytes:reasonBytes length:reasonLength encoding:NSUTF8StringEncoding];
                    [self _failWithMessage:[NSString stringWithFormat:@"wayvnc refused the connection: %@", reason ?: @"unknown reason"]];
                }];
            }];
            return;
        }
        [self _readExactly:count completion:^(const uint8_t *types) {
            BOOL offersNone = NO;
            for (uint8_t i = 0; i < count; i++) {
                if (types[i] == 1) {
                    offersNone = YES;
                    break;
                }
            }
            if (!offersNone) {
                [self _failWithMessage:@"wayvnc did not offer security type None (no-auth)"];
                return;
            }
            static const uint8_t chooseNone[1] = {1};
            nw_connection_send(self->_connection,
                                dispatch_data_create(chooseNone, sizeof(chooseNone), self->_queue,
                                                      DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                                NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable error) {
                if (error != NULL) {
                    [self _failWithMessage:@"Failed to select security type"];
                    return;
                }
                [self _readSecurityResult];
            });
        }];
    }];
}

- (void)_readSecurityResult {
    [self _readExactly:4 completion:^(const uint8_t *bytes) {
        uint32_t result = rfb_read_u32(bytes);
        if (result != 0) {
            [self _readExactly:4 completion:^(const uint8_t *lenBytes) {
                uint32_t reasonLength = rfb_read_u32(lenBytes);
                [self _readExactly:reasonLength completion:^(const uint8_t *reasonBytes) {
                    NSString *reason = [[NSString alloc] initWithBytes:reasonBytes length:reasonLength encoding:NSUTF8StringEncoding];
                    [self _failWithMessage:[NSString stringWithFormat:@"wayvnc security handshake failed: %@", reason ?: @"unknown reason"]];
                }];
            }];
            return;
        }
        [self _sendClientInit];
    }];
}

- (void)_sendClientInit {
    static const uint8_t sharedFlag[1] = {1};
    nw_connection_send(_connection,
                        dispatch_data_create(sharedFlag, sizeof(sharedFlag), _queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                        NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable error) {
        if (error != NULL) {
            [self _failWithMessage:@"Failed to send ClientInit"];
            return;
        }
        [self _readServerInit];
    });
}

- (void)_readServerInit {
    [self _readExactly:4 completion:^(const uint8_t *bytes) {
        uint16_t width = rfb_read_u16(&bytes[0]);
        uint16_t height = rfb_read_u16(&bytes[2]);
        // 16-byte server-native pixel format follows, then a 4-byte name
        // length + name. We don't care about the server's proposed pixel
        // format -- SetPixelFormat below overrides it -- so just skip it.
        [self _readExactly:16 completion:^(const uint8_t *pixfmtBytes) {
            (void) pixfmtBytes;
            [self _readExactly:4 completion:^(const uint8_t *nameLenBytes) {
                uint32_t nameLength = rfb_read_u32(nameLenBytes);
                [self _readExactly:nameLength completion:^(const uint8_t *nameBytes) {
                    NSString *name = [[NSString alloc] initWithBytes:nameBytes length:nameLength encoding:NSUTF8StringEncoding];
                    [self _finishConnectWithWidth:width height:height name:name];
                }];
            }];
        }];
    }];
}

- (void)_finishConnectWithWidth:(uint16_t)width height:(uint16_t)height name:(NSString *_Nullable)name {
    if (width == 0 || height == 0) {
        [self _failWithMessage:@"wayvnc reported an empty desktop size"];
        return;
    }
    free(_framebuffer);
    _framebuffer = calloc((size_t) width * height, 4);
    if (_framebuffer == NULL) {
        [self _failWithMessage:@"Failed to allocate framebuffer"];
        return;
    }
    _framebufferWidth = width;
    _framebufferHeight = height;
    _desktopName = name;
    _dirtyRect = CGRectMake(0, 0, width, height); // first frame always renders in full

    [self _sendSetPixelFormat];
    [self _sendSetEncodings];
    [self _sendFramebufferUpdateRequestIncremental:NO];
    _connected = YES;

    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        [strongSelf.delegate rfbClientDidConnect:strongSelf];
    });

    [self _readNextServerMessage];
}

- (void)_sendSetPixelFormat {
    // 32bpp/depth24/true-colour/little-endian with red/green/blue shifts
    // 16/8/0 -- byte order in memory is [B,G,R,pad], i.e. exactly
    // MTLPixelFormatBGRA8Unorm, so decoded Raw rects can be fed straight
    // into a Metal texture with zero per-pixel conversion. Confirmed
    // against neatvnc's source that an explicit true-colour SetPixelFormat
    // is honored and the raw encoder converts to it (see the header doc).
    uint8_t msg[4 + 16] = {0};
    msg[0] = DisplayRFBClientMessageSetPixelFormat;
    uint8_t *pf = &msg[4];
    pf[0] = 32;  // bits-per-pixel
    pf[1] = 24;  // depth
    pf[2] = 0;   // big-endian-flag
    pf[3] = 1;   // true-colour-flag
    rfb_write_u16(&pf[4], 255);  // red-max
    rfb_write_u16(&pf[6], 255);  // green-max
    rfb_write_u16(&pf[8], 255);  // blue-max
    pf[10] = 16; // red-shift
    pf[11] = 8;  // green-shift
    pf[12] = 0;  // blue-shift
    nw_connection_send(_connection, dispatch_data_create(msg, sizeof(msg), _queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                        NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable sendError) {});
}

- (void)_sendSetEncodings {
    // Raw (0): the only real pixel encoding we decode -- see the class doc
    // for why that's enough for our own wayvnc. CopyRect (1): scroll/move
    // regions arrive as a 4-byte source offset instead of full pixel data.
    // Cursor (-239): the server composites the pointer image client-side
    // instead of redrawing it into the framebuffer on every pointer move,
    // cutting both wire bytes and server-side render cost for the common
    // case of just moving the mouse around.
    static const int32_t encodings[] = {DisplayRFBEncodingRaw, DisplayRFBEncodingCopyRect, DisplayRFBEncodingCursor};
    const size_t count = sizeof(encodings) / sizeof(encodings[0]);
    uint8_t msg[4 + 4 * 3];
    msg[0] = DisplayRFBClientMessageSetEncodings;
    msg[1] = 0;
    rfb_write_u16(&msg[2], (uint16_t) count);
    for (size_t i = 0; i < count; i++)
        rfb_write_u32(&msg[4 + i * 4], (uint32_t) encodings[i]);
    nw_connection_send(_connection, dispatch_data_create(msg, sizeof(msg), _queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                        NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable sendError) {});
}

- (void)_sendFramebufferUpdateRequestIncremental:(BOOL)incremental {
    uint8_t msg[10];
    msg[0] = DisplayRFBClientMessageFramebufferUpdateRequest;
    msg[1] = incremental ? 1 : 0;
    rfb_write_u16(&msg[2], 0);
    rfb_write_u16(&msg[4], 0);
    rfb_write_u16(&msg[6], _framebufferWidth);
    rfb_write_u16(&msg[8], _framebufferHeight);
    nw_connection_send(_connection, dispatch_data_create(msg, sizeof(msg), _queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                        NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable sendError) {});
}

#pragma mark - Server message loop

- (void)_readNextServerMessage {
    [self _readExactly:1 completion:^(const uint8_t *bytes) {
        DisplayRFBServerMessageType type = bytes[0];
        switch (type) {
            case DisplayRFBServerMessageFramebufferUpdate:
                [self _readFramebufferUpdateHeader];
                return;
            case DisplayRFBServerMessageBell:
                [self _readNextServerMessage];
                return;
            case DisplayRFBServerMessageServerCutText:
                [self _readServerCutTextHeader];
                return;
            case DisplayRFBServerMessageSetColourMapEntries:
            default:
                // We always request true-colour, so a real server should
                // never send a colour map; treat anything here (including
                // an unrecognized type) as a protocol violation rather than
                // guess at how to skip an unknown/unbounded payload.
                [self _failWithMessage:[NSString stringWithFormat:@"Unexpected server message type %u", (unsigned) type]];
                return;
        }
    }];
}

- (void)_readFramebufferUpdateHeader {
    [self _readExactly:3 completion:^(const uint8_t *bytes) {
        self->_lastUpdateHeaderHex = rfb_hex_dump(bytes, 3);
        uint16_t rectCount = rfb_read_u16(&bytes[1]);
        [self _readRectAtIndex:0 of:rectCount];
    }];
}

- (void)_readRectAtIndex:(uint16_t)index of:(uint16_t)total {
    if (index >= total) {
        // Whole FramebufferUpdate processed: notify, then go quiet on the
        // wire until the delegate calls -acknowledgeFramebufferRead. See
        // the header doc -- this is the backpressure mechanism, not just an
        // optimization: we deliberately don't ask for (or read) more pixel
        // data until whatever consumed this frame says it's safe to.
        self->_hasPendingFrame = YES;
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            [strongSelf.delegate rfbClientDidUpdateFramebuffer:strongSelf];
        });
        return;
    }
    [self _readExactly:12 completion:^(const uint8_t *bytes) {
        uint16_t x = rfb_read_u16(&bytes[0]);
        uint16_t y = rfb_read_u16(&bytes[2]);
        uint16_t w = rfb_read_u16(&bytes[4]);
        uint16_t h = rfb_read_u16(&bytes[6]);
        int32_t encoding = (int32_t) rfb_read_u32(&bytes[8]);

        // Cursor's x/y are a hotspot offset within the cursor image, not a
        // framebuffer position -- must branch before the framebuffer-bounds
        // check below, which doesn't apply to it at all.
        if (encoding == DisplayRFBEncodingCursor) {
            [self _readCursorRectWithWidth:w height:h hotspotX:x hotspotY:y index:index total:total];
            return;
        }

        // A zero-area rect has no pixel payload under ANY encoding -- per
        // spec there's nothing to read regardless of what the encoding field
        // says, so check this before validating encoding/bounds at all.
        // Observed in the wild: this server's very first rect of the first
        // update is a 0x0 sentinel at the bottom-right corner carrying a
        // meaningless encoding value (not a real RFB/QEMU/VMware encoding --
        // checked against every known constant) -- skip it rather than treat
        // an irrelevant field on a no-op rect as a protocol violation.
        if (w == 0 || h == 0) {
            [self _readRectAtIndex:index + 1 of:total];
            return;
        }
        if ((uint32_t) x + w > self->_framebufferWidth || (uint32_t) y + h > self->_framebufferHeight) {
            [self _failWithMessage:[NSString stringWithFormat:
                @"Rect out of framebuffer bounds: rect=%u,%u %ux%u fb=%ux%u",
                x, y, w, h, (unsigned) self->_framebufferWidth, (unsigned) self->_framebufferHeight]];
            return;
        }
        if (encoding == DisplayRFBEncodingCopyRect) {
            [self _readCopyRectSourceForDestX:x y:y w:w h:h index:index total:total];
            return;
        }
        if (encoding != DisplayRFBEncodingRaw) {
            [self _failWithMessage:[NSString stringWithFormat:
                @"Unexpected encoding %d at rect %u/%u, rect=%u,%u %ux%u -- header=[%@] rectbytes=[%@]",
                encoding, (unsigned) index, (unsigned) total, x, y, w, h,
                self->_lastUpdateHeaderHex, rfb_hex_dump(bytes, 12)]];
            return;
        }
        uint32_t byteCount = (uint32_t) w * h * 4;
        [self _readExactly:byteCount completion:^(const uint8_t *pixels) {
            size_t srcStride = (size_t) w * 4;
            size_t dstStride = (size_t) self->_framebufferWidth * 4;
            for (uint16_t row = 0; row < h; row++) {
                uint8_t *dst = self->_framebuffer + (size_t) (y + row) * dstStride + (size_t) x * 4;
                memcpy(dst, pixels + (size_t) row * srcStride, srcStride);
            }
            self->_dirtyRect = CGRectUnion(self->_dirtyRect, CGRectMake(x, y, w, h));
            [self _readRectAtIndex:index + 1 of:total];
        }];
    }];
}

- (void)_readCopyRectSourceForDestX:(uint16_t)x y:(uint16_t)y w:(uint16_t)w h:(uint16_t)h
                               index:(uint16_t)index total:(uint16_t)total {
    [self _readExactly:4 completion:^(const uint8_t *bytes) {
        uint16_t srcX = rfb_read_u16(&bytes[0]);
        uint16_t srcY = rfb_read_u16(&bytes[2]);
        if ((uint32_t) srcX + w > self->_framebufferWidth || (uint32_t) srcY + h > self->_framebufferHeight) {
            [self _failWithMessage:@"CopyRect source out of framebuffer bounds"];
            return;
        }
        size_t stride = (size_t) self->_framebufferWidth * 4;
        size_t rowBytes = (size_t) w * 4;
        // Via a temp buffer, not a direct row-by-row copy -- src and dst can
        // legitimately overlap (e.g. scrolling content down a few rows
        // within the same window is exactly a CopyRect whose source and
        // destination rects overlap), and memcpy between overlapping
        // regions is undefined behavior.
        uint8_t *tmp = malloc(rowBytes * h);
        if (tmp != NULL) {
            for (uint16_t row = 0; row < h; row++)
                memcpy(tmp + (size_t) row * rowBytes,
                       self->_framebuffer + (size_t) (srcY + row) * stride + (size_t) srcX * 4, rowBytes);
            for (uint16_t row = 0; row < h; row++)
                memcpy(self->_framebuffer + (size_t) (y + row) * stride + (size_t) x * 4,
                       tmp + (size_t) row * rowBytes, rowBytes);
            free(tmp);
            self->_dirtyRect = CGRectUnion(self->_dirtyRect, CGRectMake(x, y, w, h));
        }
        [self _readRectAtIndex:index + 1 of:total];
    }];
}

- (void)_readCursorRectWithWidth:(uint16_t)width height:(uint16_t)height
                         hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY
                            index:(uint16_t)index total:(uint16_t)total {
    if (width == 0 || height == 0) {
        // Empty cursor (server hiding the pointer) -- no payload at all, but
        // still tell the delegate so it can hide whatever it's rendering.
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            [strongSelf.delegate rfbClient:strongSelf didUpdateCursorWithWidth:0 height:0
                                   hotspotX:0 hotspotY:0 bgra:NSData.data];
        });
        [self _readRectAtIndex:index + 1 of:total];
        return;
    }
    uint32_t pixelBytes = (uint32_t) width * height * 4;
    uint32_t maskStride = (uint32_t) ((width + 7) / 8);
    uint32_t maskBytes = maskStride * height;
    [self _readExactly:pixelBytes completion:^(const uint8_t *pixels) {
        // Copy out now -- `pixels` is only valid inside this completion block.
        NSMutableData *bgra = [NSMutableData dataWithBytes:pixels length:pixelBytes];
        [self _readExactly:maskBytes completion:^(const uint8_t *mask) {
            uint8_t *bgraBytes = bgra.mutableBytes;
            // Byte 3 of each pixel is the meaningless pad from our
            // SetPixelFormat request (BGRA8888, no real alpha channel) --
            // overwrite it with the cursor's real bitmask alpha (opaque
            // where the mask bit is 1, fully transparent where 0) so this
            // composites correctly wherever the delegate renders it.
            for (uint16_t row = 0; row < height; row++) {
                const uint8_t *maskRow = mask + (size_t) row * maskStride;
                for (uint16_t col = 0; col < width; col++) {
                    BOOL visible = (maskRow[col / 8] & (0x80 >> (col % 8))) != 0;
                    bgraBytes[((size_t) row * width + col) * 4 + 3] = visible ? 0xff : 0x00;
                }
            }
            __weak typeof(self) weakSelf = self;
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) strongSelf = weakSelf;
                if (strongSelf == nil)
                    return;
                [strongSelf.delegate rfbClient:strongSelf didUpdateCursorWithWidth:width height:height
                                       hotspotX:hotspotX hotspotY:hotspotY bgra:bgra];
            });
            [self _readRectAtIndex:index + 1 of:total];
        }];
    }];
}

- (void)_readServerCutTextHeader {
    [self _readExactly:7 completion:^(const uint8_t *bytes) {
        uint32_t length = rfb_read_u32(&bytes[3]);
        [self _readExactly:length completion:^(const uint8_t *textBytes) {
            NSString *text = [[NSString alloc] initWithBytes:textBytes length:length encoding:NSISOLatin1StringEncoding];
            __weak typeof(self) weakSelf = self;
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) strongSelf = weakSelf;
                if (strongSelf == nil || text.length == 0)
                    return;
                [strongSelf.delegate rfbClient:strongSelf didReceiveServerCutText:text];
            });
            [self _readNextServerMessage];
        }];
    }];
}

#pragma mark - Framebuffer access

- (nullable const uint8_t *)framebufferBytes {
    return _framebuffer;
}

- (void)acknowledgeFramebufferRead {
    dispatch_async(_queue, ^{
        if (!self->_connected || self->_connection == nil || !self->_hasPendingFrame)
            return;
        self->_hasPendingFrame = NO;
        self->_dirtyRect = CGRectNull;
        [self _sendFramebufferUpdateRequestIncremental:YES];
        [self _readNextServerMessage];
    });
}

#pragma mark - Input

- (void)sendPointerEventAtX:(uint16_t)x y:(uint16_t)y buttonMask:(uint8_t)buttonMask {
    dispatch_async(_queue, ^{
        if (self->_connection == nil)
            return;
        uint8_t msg[6];
        msg[0] = DisplayRFBClientMessagePointerEvent;
        msg[1] = buttonMask;
        rfb_write_u16(&msg[2], x);
        rfb_write_u16(&msg[4], y);
        nw_connection_send(self->_connection, dispatch_data_create(msg, sizeof(msg), self->_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                            NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable sendError) {});
    });
}

- (void)sendKeyEvent:(uint32_t)keysym down:(BOOL)down {
    dispatch_async(_queue, ^{
        if (self->_connection == nil)
            return;
        uint8_t msg[8];
        msg[0] = DisplayRFBClientMessageKeyEvent;
        msg[1] = down ? 1 : 0;
        rfb_write_u16(&msg[2], 0);
        rfb_write_u32(&msg[4], keysym);
        nw_connection_send(self->_connection, dispatch_data_create(msg, sizeof(msg), self->_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                            NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable sendError) {});
    });
}

- (void)sendCtrlAltDel {
    static const uint32_t keysymControlL = 0xFFE3;
    static const uint32_t keysymAltL = 0xFFE9;
    static const uint32_t keysymDelete = 0xFFFF;
    [self sendKeyEvent:keysymControlL down:YES];
    [self sendKeyEvent:keysymAltL down:YES];
    [self sendKeyEvent:keysymDelete down:YES];
    [self sendKeyEvent:keysymDelete down:NO];
    [self sendKeyEvent:keysymAltL down:NO];
    [self sendKeyEvent:keysymControlL down:NO];
}

- (void)sendClientCutText:(NSString *)text {
    NSData *latin1 = [text dataUsingEncoding:NSISOLatin1StringEncoding allowLossyConversion:YES];
    if (latin1 == nil)
        return;
    dispatch_async(_queue, ^{
        if (self->_connection == nil)
            return;
        uint32_t length = (uint32_t) latin1.length;
        NSMutableData *msg = [NSMutableData dataWithLength:8];
        uint8_t *header = msg.mutableBytes;
        header[0] = DisplayRFBClientMessageClientCutText;
        rfb_write_u32(&header[4], length);
        [msg appendData:latin1];
        nw_connection_send(self->_connection, dispatch_data_create(msg.bytes, msg.length, self->_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT),
                            NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t _Nullable sendError) {});
    });
}

@end

NS_ASSUME_NONNULL_END
