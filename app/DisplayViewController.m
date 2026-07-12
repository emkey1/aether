#import "DisplayViewController.h"
#import "DisplayNetworkBridge.h"
#import "DisplayStaticFileServer.h"
#import "Terminal.h"
#import "AppDelegate.h"
#import "GuestFileBridge.h"
#import <WebKit/WebKit.h>
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/devices.h"
#include "fs/path.h"

NS_ASSUME_NONNULL_BEGIN

// Written by /AOK/tools/start-wayland.sh once wayvnc is confirmed listening
// (its content is the guest TCP port, as ASCII decimal). Polled rather than
// parsed from the session's own stdout, which is carried over a real pty and
// would otherwise need unwinding from whatever escape sequences the shell
// emits, not a plain byte stream.
static NSString *const DisplayReadyGuestPath = @"/tmp/ish-display.ready";
static const NSTimeInterval DisplayReadyPollInterval = 0.3;
static const NSTimeInterval DisplayReadyTimeout = 45.0;

typedef NS_ENUM(NSInteger, DisplayConnectionState) {
    DisplayConnectionStateIdle,
    DisplayConnectionStateStartingGuestSession,
    DisplayConnectionStateWaitingForReady,
    DisplayConnectionStateStartingBridge,
    DisplayConnectionStateConnected,
    DisplayConnectionStateFailed,
};

@interface DisplayViewController () <WKScriptMessageHandler, WKNavigationDelegate>
@end

@implementation DisplayViewController {
    UIView *_toolbarCard;
    UIView *_displayCard;
    UILabel *_statusLabel;
    UIButton *_ctrlAltDelButton;
    UIButton *_pasteButton;
    UIButton *_reconnectButton;
    WKWebView *_webView;

    // The guest session is owned exactly the way TerminalViewController owns
    // its shell session (a real pty via +[Terminal createPseudoTerminal:]),
    // but _sessionTerminal's own .webView is never touched/shown -- only
    // _webView (the noVNC client) is user-visible. Tearing this Terminal
    // down hangs up the pty (SIGHUP), which start-wayland.sh's trap uses to
    // tear the whole guest Wayland stack down.
    Terminal *_Nullable _sessionTerminal;
    int _sessionPid;

    DisplayNetworkBridge *_Nullable _bridge;
    DisplayConnectionState _state;
    NSDate *_Nullable _readyPollDeadline;
    BOOL _startedOnce;

    // Serves display.html/js/css + deps/novnc over http://127.0.0.1 instead
    // of file:// -- see DisplayStaticFileServer.h for why (WKWebView
    // rejects noVNC's rfb.js ES module import under file:// as
    // cross-origin, even with allowingReadAccessToURL: granted). Started
    // once, independent of the guest session, since it never depends on
    // the guest boot/Wayland stack at all.
    DisplayStaticFileServer *_staticServer;
    uint16_t _staticServerPort;
    NSDate *_Nullable _staticServerStartDeadline;
}

#pragma mark - Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Display";

    _toolbarCard = [self workspaceThemeCardView];
    _displayCard = [self workspaceThemeCardView];
    [self.toolContentView addSubview:_toolbarCard];
    [self.toolContentView addSubview:_displayCard];

    _statusLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _statusLabel.text = @"Starting…";
    _statusLabel.numberOfLines = 1;
    [_toolbarCard addSubview:_statusLabel];

    _ctrlAltDelButton = [self displayButtonWithTitle:@"Ctrl+Alt+Del" action:@selector(sendCtrlAltDel:)];
    [_toolbarCard addSubview:_ctrlAltDelButton];

    _pasteButton = [self displayButtonWithTitle:@"Paste" action:@selector(pasteToGuest:)];
    [_toolbarCard addSubview:_pasteButton];

    _reconnectButton = [self displayButtonWithTitle:@"Reconnect" action:@selector(reconnect:)];
    _reconnectButton.hidden = YES;
    [_toolbarCard addSubview:_reconnectButton];

    CGFloat inset = 8.0;
    [NSLayoutConstraint activateConstraints:@[
        [_toolbarCard.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:inset],
        [_toolbarCard.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:inset],
        [_toolbarCard.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-inset],
        [_toolbarCard.heightAnchor constraintEqualToConstant:44.0],

        [_displayCard.topAnchor constraintEqualToAnchor:_toolbarCard.bottomAnchor constant:inset],
        [_displayCard.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:inset],
        [_displayCard.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-inset],
        [_displayCard.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor constant:-inset],

        [_statusLabel.leadingAnchor constraintEqualToAnchor:_toolbarCard.leadingAnchor constant:12.0],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_reconnectButton.trailingAnchor constraintEqualToAnchor:_toolbarCard.trailingAnchor constant:-8.0],
        [_reconnectButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_ctrlAltDelButton.trailingAnchor constraintEqualToAnchor:_reconnectButton.leadingAnchor constant:-8.0],
        [_ctrlAltDelButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_pasteButton.trailingAnchor constraintEqualToAnchor:_ctrlAltDelButton.leadingAnchor constant:-8.0],
        [_pasteButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_pasteButton.leadingAnchor constant:-8.0],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                            selector:@selector(guestProcessExited:)
                                                name:ProcessExitedNotification
                                              object:nil];

    // Started immediately, independent of the guest session -- purely
    // static asset serving, nothing here depends on the guest booting.
    // By the time -loadWebViewWithBridgePort: actually needs
    // _staticServerPort (after guest boot + Wayland stack readiness, many
    // seconds away), this has always long since finished.
    NSURL *displayHTMLFile = [NSBundle.mainBundle URLForResource:@"display" withExtension:@"html"];
    NSURL *bundleRootURL = displayHTMLFile.URLByDeletingLastPathComponent ?: NSBundle.mainBundle.resourceURL;
    if (bundleRootURL != nil) {
        _staticServer = [DisplayStaticFileServer new];
        __weak typeof(self) weakSelf = self;
        [_staticServer startServingRootURL:bundleRootURL completion:^(uint16_t port, NSError *_Nullable error) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            if (error != nil) {
                [strongSelf failWithMessage:[NSString stringWithFormat:@"Static asset server failed to start: %@", error.localizedDescription]];
                return;
            }
            strongSelf->_staticServerPort = port;
        }];
    }
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!_startedOnce) {
        _startedOnce = YES;
        [self startGuestSession];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [self teardownSession];
    // Not stopped in -teardownSession: that also runs on every Reconnect,
    // and the static server is independent of the guest session -- no
    // need to restart it (and re-poll for it in -loadWebViewWithBridgePort:)
    // on every reconnect attempt.
    [_staticServer stop];
}

- (UIButton *)displayButtonWithTitle:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setTitle:title forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:13.0 weight:UIFontWeightSemibold];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

#pragma mark - Guest session

- (void)startGuestSession {
    if (_state != DisplayConnectionStateIdle && _state != DisplayConnectionStateFailed)
        return;
    _reconnectButton.hidden = YES;
    _state = DisplayConnectionStateStartingGuestSession;
    _statusLabel.text = @"Starting Wayland session…";
    _readyPollDeadline = nil;

    intptr_t err = [AppDelegate ensureBooted];
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:@"Boot failed: %@", [AppDelegate descriptionForISHErrno:err]]];
        return;
    }

    err = become_new_init_child();
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:@"Could not create guest session: %@",
                                [AppDelegate descriptionForISHErrno:err]]];
        return;
    }

    struct tty *tty;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        [self failWithMessage:@"Could not allocate a pseudo-terminal for the Wayland session"];
        return;
    }
    _sessionTerminal = terminal;
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:@"Could not attach session I/O: %@",
                                [AppDelegate descriptionForISHErrno:err]]];
        return;
    }
    tty_release(tty);

    NSArray<NSString *> *command = @[@"/bin/sh", @"/AOK/tools/start-wayland.sh"];
    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
                        "HOME=/root\0"
                        "TERM=xterm\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:
            @"Could not start /AOK/tools/start-wayland.sh: %@\n\nIf labwc/foot/wayvnc aren't "
            @"installed yet, run 'sudo sh /AOK/tools/setup-wayland.sh' in a terminal first.",
            [AppDelegate descriptionForISHErrno:err]]];
        return;
    }
    _sessionPid = current->pid;
    if (task_start(current) < 0) {
        struct task *failed = current;
        current = NULL;
        _sessionPid = 0;
        task_never_ran_destroy(failed);
        [self failWithMessage:@"Could not start the Wayland session task"];
        return;
    }

    _state = DisplayConnectionStateWaitingForReady;
    _statusLabel.text = @"Waiting for compositor…";
    [self pollForReadyFile];
}

- (void)pollForReadyFile {
    if (_state != DisplayConnectionStateWaitingForReady)
        return;
    if (_readyPollDeadline == nil)
        _readyPollDeadline = [NSDate dateWithTimeIntervalSinceNow:DisplayReadyTimeout];
    if (_readyPollDeadline.timeIntervalSinceNow < 0) {
        [self failWithMessage:@"Timed out waiting for the Wayland compositor to start.\n\n"
                                "If labwc/foot/wayvnc aren't installed, run "
                                "'sudo sh /AOK/tools/setup-wayland.sh' in a terminal, then Reconnect."];
        return;
    }
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:DisplayReadyGuestPath
                                                 maxBytes:64
                                               completion:^(NSData *_Nullable data, NSError *_Nullable error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_state != DisplayConnectionStateWaitingForReady)
            return;
        if (data != nil) {
            NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            NSInteger port = text.integerValue;
            if (port > 0 && port <= UINT16_MAX) {
                [strongSelf startBridgeToGuestPort:(uint16_t) port];
                return;
            }
        }
        // start-wayland.sh writes this alongside (never instead of) the
        // ready file whenever it die()s -- labwc/foot/wayvnc missing,
        // crashing, or never reaching a listening state -- so a real
        // failure surfaces immediately instead of only ever producing this
        // poll's generic timeout message after the full deadline.
        [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:[DisplayReadyGuestPath stringByAppendingString:@".error"]
                                                     maxBytes:4096
                                                   completion:^(NSData *_Nullable errorData, NSError *_Nullable readError) {
            typeof(self) innerSelf = weakSelf;
            if (innerSelf == nil || innerSelf->_state != DisplayConnectionStateWaitingForReady)
                return;
            if (errorData.length > 0) {
                NSString *reason = [[NSString alloc] initWithData:errorData encoding:NSUTF8StringEncoding];
                [innerSelf failWithMessage:[NSString stringWithFormat:@"start-wayland.sh failed:\n\n%@", reason]];
                return;
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (DisplayReadyPollInterval * NSEC_PER_SEC)),
                            dispatch_get_main_queue(), ^{
                [innerSelf pollForReadyFile];
            });
        }];
    }];
}

- (void)startBridgeToGuestPort:(uint16_t)guestPort {
    _state = DisplayConnectionStateStartingBridge;
    _statusLabel.text = @"Starting bridge…";
    _bridge = [DisplayNetworkBridge new];
    __weak typeof(self) weakSelf = self;
    [_bridge startRelayingToTCPPort:guestPort completion:^(uint16_t bridgePort, NSError *_Nullable error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        if (error != nil) {
            [strongSelf failWithMessage:[NSString stringWithFormat:@"Bridge failed to start: %@", error.localizedDescription]];
            return;
        }
        [strongSelf loadWebViewWithBridgePort:bridgePort];
    }];
}

- (void)teardownSession {
    [_bridge stop];
    _bridge = nil;
    Terminal *terminal = _sessionTerminal;
    _sessionTerminal = nil;
    _sessionPid = 0;
    [terminal setPendingDestroyReason:@"display-applet-teardown"];
    [terminal destroy];
}

- (void)guestProcessExited:(NSNotification *)notification {
    int pid = [notification.userInfo[@"pid"] intValue];
    if (pid == 0 || pid != _sessionPid)
        return;
    _sessionPid = 0;
    _sessionTerminal = nil; // the kernel already tore this down; don't double-destroy it
    [_bridge stop];
    _bridge = nil;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        strongSelf->_state = DisplayConnectionStateFailed;
        strongSelf->_statusLabel.text = @"Wayland session ended";
        strongSelf->_reconnectButton.hidden = NO;
    });
}

- (void)failWithMessage:(NSString *)message {
    _state = DisplayConnectionStateFailed;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        strongSelf->_statusLabel.text = message;
        strongSelf->_statusLabel.numberOfLines = 0;
        strongSelf->_reconnectButton.hidden = NO;
    });
}

- (void)reconnect:(id)sender {
    [self teardownSession];
    _statusLabel.numberOfLines = 1;
    _state = DisplayConnectionStateIdle;
    [self startGuestSession];
}

- (void)sendCtrlAltDel:(id)sender {
    [self.webView evaluateJavaScript:@"window.ishDisplaySendCtrlAltDel && window.ishDisplaySendCtrlAltDel();"
                    completionHandler:nil];
}

- (void)pasteToGuest:(id)sender {
    NSString *text = UIPasteboard.generalPasteboard.string;
    if (text.length == 0)
        return;
    // NSJSONSerialization for the string literal, not manual quote-escaping
    // -- this is the only safe way to embed arbitrary clipboard text
    // (quotes, backslashes, newlines, unicode) into a JS expression string.
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:@[text] options:0 error:nil];
    if (jsonData == nil)
        return;
    NSString *jsonArray = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
    NSString *jsStringLiteral = [jsonArray substringWithRange:NSMakeRange(1, jsonArray.length - 2)]; // strip [ ]
    NSString *js = [NSString stringWithFormat:@"window.ishDisplayPasteText && window.ishDisplayPasteText(%@);", jsStringLiteral];
    [self.webView evaluateJavaScript:js completionHandler:nil];
}

#pragma mark - WKWebView

- (WKWebView *)webView {
    if (_webView == nil) {
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        [config.userContentController addScriptMessageHandler:self name:@"ishDisplay"];
        _webView = [[WKWebView alloc] initWithFrame:CGRectZero configuration:config];
        _webView.navigationDelegate = self;
        _webView.translatesAutoresizingMaskIntoConstraints = NO;
        _webView.scrollView.scrollEnabled = NO; // noVNC handles its own scaling/panning
        _webView.opaque = NO;
        _webView.backgroundColor = UIColor.blackColor;
        _webView.layer.cornerRadius = 10.0;
        _webView.layer.masksToBounds = YES;
        [_displayCard addSubview:_webView];
        CGFloat inset = 8.0;
        [NSLayoutConstraint activateConstraints:@[
            [_webView.topAnchor constraintEqualToAnchor:_displayCard.topAnchor constant:inset],
            [_webView.leadingAnchor constraintEqualToAnchor:_displayCard.leadingAnchor constant:inset],
            [_webView.trailingAnchor constraintEqualToAnchor:_displayCard.trailingAnchor constant:-inset],
            [_webView.bottomAnchor constraintEqualToAnchor:_displayCard.bottomAnchor constant:-inset],
        ]];
    }
    return _webView;
}

- (void)loadWebViewWithBridgePort:(uint16_t)bridgePort {
    _state = DisplayConnectionStateConnected; // optimistic; refined by JS "status" messages

    // _staticServer is started independently in -viewDidLoad and is
    // essentially always ready long before the guest boot + Wayland stack
    // reaches this point, but poll briefly rather than assume -- cheaper
    // and more honest than a race.
    if (_staticServerPort == 0) {
        if (_staticServerStartDeadline == nil)
            _staticServerStartDeadline = [NSDate dateWithTimeIntervalSinceNow:10.0];
        if (_staticServerStartDeadline.timeIntervalSinceNow < 0) {
            [self failWithMessage:@"Static asset server never started"];
            return;
        }
        __weak typeof(self) weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (0.05 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [weakSelf loadWebViewWithBridgePort:bridgePort];
        });
        return;
    }

    NSURLComponents *components = [NSURLComponents new];
    components.scheme = @"http";
    components.host = @"127.0.0.1";
    components.port = @(_staticServerPort);
    components.path = @"/display.html";
    components.queryItems = @[[NSURLQueryItem queryItemWithName:@"port"
                                                            value:[NSString stringWithFormat:@"%u", (unsigned) bridgePort]]];
    NSURL *url = components.URL;
    if (url == nil) {
        [self failWithMessage:@"Failed to build Display UI URL"];
        return;
    }
    [self.webView loadRequest:[NSURLRequest requestWithURL:url]];
}

- (void)userContentController:(WKUserContentController *)userContentController
       didReceiveScriptMessage:(WKScriptMessage *)message {
    if (![message.name isEqualToString:@"ishDisplay"])
        return;
    NSDictionary *body = message.body;
    if (![body isKindOfClass:NSDictionary.class])
        return;
    if ([body[@"type"] isEqualToString:@"clipboard"]) {
        NSString *text = body[@"text"];
        if (text.length > 0)
            UIPasteboard.generalPasteboard.string = text;
        return;
    }
    if (![body[@"type"] isEqualToString:@"status"])
        return;
    NSString *state = body[@"state"];
    if ([state isEqualToString:@"connected"]) {
        _statusLabel.text = @"Connected";
        _statusLabel.numberOfLines = 1;
        _reconnectButton.hidden = YES;
    } else if ([state isEqualToString:@"connecting"]) {
        _statusLabel.text = @"Connecting to compositor…";
    } else if ([state isEqualToString:@"disconnected"]) {
        _statusLabel.text = @"Disconnected";
        _reconnectButton.hidden = NO;
    } else if ([state isEqualToString:@"error"]) {
        _statusLabel.text = [NSString stringWithFormat:@"Error: %@", body[@"detail"] ?: @"unknown"];
        _statusLabel.numberOfLines = 0;
        _reconnectButton.hidden = NO;
    } else if ([state isEqualToString:@"desktopname"]) {
        NSString *name = body[@"detail"];
        if (name.length > 0)
            self.title = name;
    }
}

// navigation is _Null_unspecified in WKNavigationDelegate's own declaration
// (not _Nullable) -- matched explicitly here since these implementations
// otherwise fall inside this file's NS_ASSUME_NONNULL_BEGIN region, which
// would default the bare pointer to _Nonnull and conflict with the protocol.
- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *_Null_unspecified)navigation withError:(NSError *)error {
    [self failWithMessage:[NSString stringWithFormat:@"Display UI failed to load: %@", error.localizedDescription]];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *_Null_unspecified)navigation withError:(NSError *)error {
    [self failWithMessage:[NSString stringWithFormat:@"Display UI failed to load: %@", error.localizedDescription]];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    [self failWithMessage:@"Display UI's web content process terminated"];
}

@end

NS_ASSUME_NONNULL_END
