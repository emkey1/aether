#import "DisplayViewController.h"
#import "DisplayRFBClient.h"
#import "DisplayRFBView.h"
#import "Terminal.h"
#import "AppDelegate.h"
#import "GuestFileBridge.h"
#import "UserPreferences.h"
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
static NSArray<NSString *> *const DisplayPlainRootCommand = @[@"/bin/sh", @"/AOK/tools/start-wayland.sh"];

// "Open Everything as Default User" (UserPreferences.shouldLoginAsDefaultUser):
// TerminalViewController honors this by substituting the username inside an
// already-interactive `/bin/login -f root` and letting login itself drop
// privileges. This session isn't interactive -- it execve's a script
// directly, no login prompt involved -- so `su -c` is the equivalent for
// that case: root can su to any other user without a password, and `-`
// resets HOME/USER/LOGNAME/PATH to match a genuine login as that user
// instead of leaving the hardcoded root envp below in effect.
static NSArray<NSString *> *DisplayGuestSessionCommand(void) {
    if (!UserPreferences.shared.shouldLoginAsDefaultUser)
        return DisplayPlainRootCommand;
    return @[@"/bin/su", @"-", ISHDefaultUserAccountName, @"-c", @"sh /AOK/tools/start-wayland.sh"];
}
static const NSTimeInterval DisplayReadyTimeout = 45.0;

typedef NS_ENUM(NSInteger, DisplayConnectionState) {
    DisplayConnectionStateIdle,
    DisplayConnectionStateStartingGuestSession,
    DisplayConnectionStateWaitingForReady,
    DisplayConnectionStateConnected,
    DisplayConnectionStateFailed,
};

@interface DisplayViewController () <DisplayRFBClientDelegate>
@end

@implementation DisplayViewController {
    UIView *_toolbarCard;
    UIView *_displayCard;
    UILabel *_statusLabel;
    UIButton *_ctrlAltDelButton;
    UIButton *_pasteButton;
    UIButton *_reconnectButton;
    DisplayRFBView *_displayView;

    // The guest session is owned exactly the way TerminalViewController owns
    // its shell session (a real pty via +[Terminal createPseudoTerminal:]),
    // but _sessionTerminal itself is never shown -- only _displayView (the
    // native RFB renderer) is user-visible. Tearing this Terminal down
    // hangs up the pty (SIGHUP), which start-wayland.sh's trap uses to tear
    // the whole guest Wayland stack down.
    Terminal *_Nullable _sessionTerminal;
    int _sessionPid; // the currently active session's pid, 0 if none

    // A prior session we've asked to tear down but haven't yet confirmed is
    // actually gone. -teardownSession only *initiates* teardown (hangs up
    // the pty) -- the guest side (start-wayland.sh's trap killing labwc/
    // foot/wayvnc, releasing the fixed WAYVNC_PORT) happens asynchronously,
    // whenever the guest scheduler gets to it. Starting a new session before
    // that's confirmed races the old labwc/foot/wayvnc for the same port and
    // leaves them un-reaped, piling up zombie generations that drag the
    // whole guest's scheduling down until the applet looks wedged (found via
    // a device backtrace showing 4 generations of labwc and 9 of foot alive
    // at once after repeated Reconnect clicks).
    int _teardownPid;
    BOOL _reconnectPendingAfterTeardown;

    DisplayRFBClient *_Nullable _rfbClient;
    DisplayConnectionState _state;
    NSDate *_Nullable _readyPollDeadline;
    BOOL _startedOnce;
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

    NSArray<NSString *> *command = DisplayGuestSessionCommand();
    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
                        "HOME=/root\0"
                        "TERM=xterm\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0 && ![command isEqualToArray:DisplayPlainRootCommand]) {
        // "su" missing (or otherwise failed) on this root -- fall back to
        // running as root rather than failing the whole session over a
        // preference that's a nice-to-have, not a hard requirement.
        command = DisplayPlainRootCommand;
        [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
        err = do_execve(command[0].UTF8String, command.count, argv, envp);
    }
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
                [strongSelf connectRFBToGuestPort:(uint16_t) port];
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

- (void)connectRFBToGuestPort:(uint16_t)guestPort {
    _state = DisplayConnectionStateConnected; // optimistic; refined by DisplayRFBClientDelegate callbacks
    _statusLabel.text = @"Connecting to compositor…";
    _rfbClient = [DisplayRFBClient new];
    _rfbClient.delegate = self;
    [_rfbClient connectToGuestPort:guestPort];
}

- (void)teardownSession {
    [_rfbClient disconnect];
    _rfbClient = nil;
    _displayView.rfbClient = nil;
    Terminal *terminal = _sessionTerminal;
    _sessionTerminal = nil;
    if (_sessionPid != 0) {
        _teardownPid = _sessionPid;
        _sessionPid = 0;
    }
    [terminal setPendingDestroyReason:@"display-applet-teardown"];
    [terminal destroy];
}

- (void)guestProcessExited:(NSNotification *)notification {
    int pid = [notification.userInfo[@"pid"] intValue];
    if (pid == 0)
        return;
    if (pid == _teardownPid) {
        // Confirms the guest side actually finished (labwc/foot/wayvnc
        // killed, WAYVNC_PORT released) -- see the _teardownPid doc above
        // for why this can't just be assumed once -teardownSession returns.
        _teardownPid = 0;
        if (_reconnectPendingAfterTeardown) {
            _reconnectPendingAfterTeardown = NO;
            [self startGuestSession];
        }
        return;
    }
    if (pid != _sessionPid)
        return;
    _sessionPid = 0;
    _sessionTerminal = nil; // the kernel already tore this down; don't double-destroy it
    [_rfbClient disconnect];
    _rfbClient = nil;
    _displayView.rfbClient = nil;
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
    if (_reconnectPendingAfterTeardown)
        return; // already reconnecting; ignore extra taps while we wait for the old session to actually exit
    BOOL hadActiveSession = _sessionPid != 0;
    [self teardownSession];
    _statusLabel.numberOfLines = 1;
    _reconnectButton.hidden = YES;
    _state = DisplayConnectionStateIdle;

    if (!hadActiveSession) {
        [self startGuestSession];
        return;
    }

    // Deferred until -guestProcessExited: confirms the old session's guest
    // processes actually exited -- see the _teardownPid doc above for why.
    _statusLabel.text = @"Disconnecting…";
    _reconnectPendingAfterTeardown = YES;
    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (5.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        // Fallback in case the exit notification never arrives (e.g. the
        // old session was already gone by the time we asked to tear it
        // down) -- don't leave Reconnect stuck waiting forever.
        if (strongSelf == nil || !strongSelf->_reconnectPendingAfterTeardown)
            return;
        strongSelf->_reconnectPendingAfterTeardown = NO;
        strongSelf->_teardownPid = 0;
        [strongSelf startGuestSession];
    });
}

- (void)sendCtrlAltDel:(id)sender {
    [_rfbClient sendCtrlAltDel];
}

- (void)pasteToGuest:(id)sender {
    NSString *text = UIPasteboard.generalPasteboard.string;
    if (text.length == 0)
        return;
    [_rfbClient sendClientCutText:text];
}

#pragma mark - DisplayRFBView

- (DisplayRFBView *)displayView {
    if (_displayView == nil) {
        _displayView = [[DisplayRFBView alloc] initWithFrame:CGRectZero];
        [_displayCard addSubview:_displayView];
        CGFloat inset = 8.0;
        [NSLayoutConstraint activateConstraints:@[
            [_displayView.topAnchor constraintEqualToAnchor:_displayCard.topAnchor constant:inset],
            [_displayView.leadingAnchor constraintEqualToAnchor:_displayCard.leadingAnchor constant:inset],
            [_displayView.trailingAnchor constraintEqualToAnchor:_displayCard.trailingAnchor constant:-inset],
            [_displayView.bottomAnchor constraintEqualToAnchor:_displayCard.bottomAnchor constant:-inset],
        ]];
    }
    return _displayView;
}

#pragma mark - DisplayRFBClientDelegate

- (void)rfbClientDidConnect:(DisplayRFBClient *)client {
    if (client != _rfbClient)
        return;
    self.displayView.rfbClient = client;
    if (client.desktopName.length > 0)
        self.title = client.desktopName;
    _state = DisplayConnectionStateConnected;
    _statusLabel.text = @"Connected";
    _statusLabel.numberOfLines = 1;
    _reconnectButton.hidden = YES;
}

- (void)rfbClientDidUpdateFramebuffer:(DisplayRFBClient *)client {
    if (client != _rfbClient)
        return;
    [_displayView setNeedsDisplay];
}

- (void)rfbClient:(DisplayRFBClient *)client didUpdateCursorWithWidth:(uint16_t)width height:(uint16_t)height
         hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra {
    if (client != _rfbClient)
        return;
    [_displayView updateCursorWithWidth:width height:height hotspotX:hotspotX hotspotY:hotspotY bgra:bgra];
}

- (void)rfbClient:(DisplayRFBClient *)client didReceiveServerCutText:(NSString *)text {
    if (client != _rfbClient)
        return;
    UIPasteboard.generalPasteboard.string = text;
}

- (void)rfbClient:(DisplayRFBClient *)client didFailWithMessage:(NSString *)message {
    if (client != _rfbClient)
        return;
    [self failWithMessage:message];
}

@end

NS_ASSUME_NONNULL_END
