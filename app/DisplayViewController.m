#import "DisplayViewController.h"
#import "AboutViewController.h"
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
// instead of leaving the hardcoded root envp below in effect. No account is
// provisioned for this -- [AppDelegate defaultUserAccountName] looks up
// whatever's actually at UID 1000 on this rootfs; falls back to root if
// there isn't one.
static NSArray<NSString *> *DisplayGuestSessionCommand(void) {
    if (!UserPreferences.shared.shouldLoginAsDefaultUser)
        return DisplayPlainRootCommand;
    NSString *accountName = [AppDelegate defaultUserAccountName];
    if (accountName.length == 0)
        return DisplayPlainRootCommand;
    return @[@"/bin/su", @"-", accountName, @"-c", @"sh /AOK/tools/start-wayland.sh"];
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
    UILabel *_statusLabel;
    UIButton *_ctrlAltDelButton;
    UIButton *_pasteButton;
    UIButton *_reconnectButton;
    UIButton *_Nullable _menuPip; // standalone mode only
    NSLayoutConstraint *_Nullable _menuPipBottomConstraint;
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
    self.title = @"Wayland";

    _toolbarCard = [self workspaceThemeCardView];
    [self.toolContentView addSubview:_toolbarCard];

    _statusLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _statusLabel.text = @"Starting…";
    _statusLabel.numberOfLines = 1;
    _statusLabel.font = [UIFont systemFontOfSize:11.0];
    [_toolbarCard addSubview:_statusLabel];

    _ctrlAltDelButton = [self displayButtonWithTitle:@"Ctrl+Alt+Del" action:@selector(sendCtrlAltDel:)];
    [_toolbarCard addSubview:_ctrlAltDelButton];

    _pasteButton = [self displayButtonWithTitle:@"Paste" action:@selector(pasteToGuest:)];
    [_toolbarCard addSubview:_pasteButton];

    _reconnectButton = [self displayButtonWithTitle:@"Reconnect" action:@selector(reconnect:)];
    _reconnectButton.hidden = YES;
    [_toolbarCard addSubview:_reconnectButton];

    CGFloat inset = 8.0;
    NSMutableArray<NSLayoutConstraint *> *constraints = [NSMutableArray arrayWithArray:@[
        [_toolbarCard.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:inset],
        [_toolbarCard.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:inset],
        [_toolbarCard.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-inset],
        [_toolbarCard.heightAnchor constraintEqualToConstant:22.0],

        [_statusLabel.leadingAnchor constraintEqualToAnchor:_toolbarCard.leadingAnchor constant:10.0],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_reconnectButton.trailingAnchor constraintEqualToAnchor:_toolbarCard.trailingAnchor constant:-6.0],
        [_reconnectButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_ctrlAltDelButton.trailingAnchor constraintEqualToAnchor:_reconnectButton.leadingAnchor constant:-6.0],
        [_ctrlAltDelButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_pasteButton.trailingAnchor constraintEqualToAnchor:_ctrlAltDelButton.leadingAnchor constant:-6.0],
        [_pasteButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],
    ]];
    [constraints addObject:
        [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_pasteButton.leadingAnchor constant:-6.0]];
    [NSLayoutConstraint activateConstraints:constraints];

    // Standalone (startup-mode) only: the same lower-right "Workspace menu"
    // pip the Workspace desktop shows (visual style copied from
    // WorkspaceViewController's makeModernMenuPip), so the corner menu button
    // is reachable from every mode. Added to self.view, not toolContentView,
    // so it floats above the display surface.
    if (self.standaloneMode) {
        UIButton *pip = [UIButton buttonWithType:UIButtonTypeSystem];
        pip.translatesAutoresizingMaskIntoConstraints = NO;
        [pip setImage:[UIImage systemImageNamed:@"line.3.horizontal"] forState:UIControlStateNormal];
        pip.tintColor = UIColor.whiteColor;
        NSDictionary<NSString *, UIColor *> *theme = [self workspaceTheme];
        pip.backgroundColor = theme[@"accent"] ?: [UIColor colorWithRed:0.20 green:0.48 blue:0.96 alpha:1.0];
        pip.layer.cornerRadius = 22.0;
        pip.layer.borderWidth = 1.5;
        pip.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.85].CGColor;
        pip.layer.shadowColor = UIColor.blackColor.CGColor;
        pip.layer.shadowOpacity = 0.35;
        pip.layer.shadowRadius = 6.0;
        pip.layer.shadowOffset = CGSizeMake(0.0, 2.0);
        pip.accessibilityLabel = @"Display menu";
        [pip addTarget:self action:@selector(menuPipTapped:) forControlEvents:UIControlEventTouchUpInside];
        _menuPip = pip;
        [self.view addSubview:pip];
        _menuPipBottomConstraint = [pip.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-16.0];
        [NSLayoutConstraint activateConstraints:@[
            [pip.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-16.0],
            _menuPipBottomConstraint,
            [pip.widthAnchor constraintEqualToConstant:44.0],
            [pip.heightAnchor constraintEqualToConstant:44.0],
        ]];

        // DisplayRFBView is its own first responder and supplies the accessory key strip
        // (see -[DisplayRFBView accessoryBar]) as its inputAccessoryView. With a hardware
        // keyboard attached there's no software keyboard to push content up -- the
        // accessory bar is presented on its own at the bottom of the screen, right where
        // this pip sits, and this view controller otherwise has no idea it's there. Track
        // the same keyboard-frame notifications the terminal uses to keep its accessory
        // bar clear of content, and nudge the pip up above whatever's covering it.
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserver:self
                   selector:@selector(menuPipKeyboardDidSomething:)
                       name:UIKeyboardWillChangeFrameNotification
                     object:nil];
        [center addObserver:self
                   selector:@selector(menuPipKeyboardDidSomething:)
                       name:UIKeyboardDidChangeFrameNotification
                     object:nil];
    }

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

// Standalone fullscreen: the desktop extends under the home indicator (see
// -displayView), so let the indicator fade out when idle like a video player.
- (BOOL)prefersHomeIndicatorAutoHidden {
    return self.standaloneMode;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [self teardownSession];
}

- (UIButton *)displayButtonWithTitle:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setTitle:title forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:11.0 weight:UIFontWeightSemibold];
    button.contentEdgeInsets = UIEdgeInsetsZero;
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

    // Stale copies of the ready/error files can be owned by a different uid
    // than this session will run as: a root session leaves them root-owned,
    // and an "Open Everything as Default User" session (su - <user>) can then
    // neither remove nor overwrite them in sticky /tmp. Observed on-device as
    // rm/tee EACCES cascading into the compositor dying before its socket
    // existed. Unlink them here with root credentials so every session starts
    // clean no matter who ran the last one. (ENOENT is the normal case.)
    generic_unlinkat(AT_PWD, "/tmp/ish-display.ready");
    generic_unlinkat(AT_PWD, "/tmp/ish-display.ready.error");

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
        BOOL diedDuringStartup = strongSelf->_state == DisplayConnectionStateStartingGuestSession
            || strongSelf->_state == DisplayConnectionStateWaitingForReady;
        strongSelf->_state = DisplayConnectionStateFailed;
        strongSelf->_statusLabel.text = @"Wayland session ended";
        strongSelf->_reconnectButton.hidden = NO;
        // A session that exits before ever reaching Connected means
        // start-wayland.sh die()d (or never ran) -- and this exit
        // notification always wins the race against pollForReadyFile's
        // .error read, so without this every guest-side startup failure
        // presented as the bare "Wayland session ended" with the actual
        // reason left unread in the guest. Fetch and show it.
        if (diedDuringStartup)
            [strongSelf showStartupExitReason];
    });
}

- (void)showStartupExitReason {
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:[DisplayReadyGuestPath stringByAppendingString:@".error"]
                                                 maxBytes:4096
                                               completion:^(NSData *_Nullable errorData, NSError *_Nullable readError) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_state != DisplayConnectionStateFailed)
            return;
        if (errorData.length > 0) {
            NSString *reason = [[NSString alloc] initWithData:errorData encoding:NSUTF8StringEncoding];
            [strongSelf failWithMessage:[NSString stringWithFormat:@"Wayland session ended:\n\n%@", reason]];
            return;
        }
        // No error file: the script never got far enough to write one (or the
        // failure was outside its die() paths). Point at the places the
        // evidence actually lands. The su note matters because the
        // default-user preference launches the script via `su - <user> -c`,
        // whose runtime failures (e.g. a nologin shell) exit before the
        // script ever runs.
        NSMutableString *message = [@"Wayland session ended before the compositor came up.\n\n"
                                    @"Check /tmp/ish-wayland-debug.log in a terminal, or run "
                                    @"'sh /AOK/tools/start-wayland.sh' there to see the failure directly." mutableCopy];
        if (UserPreferences.shared.shouldLoginAsDefaultUser)
            [message appendString:@"\n\nIf that works, the failure is in the \"Open Everything as "
                                  @"Default User\" su path -- try disabling that setting."];
        [strongSelf failWithMessage:message];
    }];
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

// Keeps the pip clear of DisplayRFBView's inputAccessoryView (the accessory key
// strip, see -[DisplayRFBView accessoryBar]) whether it's riding above the
// software keyboard or, with a hardware keyboard attached, sitting alone at the
// bottom of the screen. Mirrors the frame math TerminalViewController uses for
// the same reason (-keyboardDidSomething:).
- (void)menuPipKeyboardDidSomething:(NSNotification *)notification {
    if (_menuPipBottomConstraint == nil)
        return;
    CGRect screenKeyboardFrame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    UIWindow *window = self.view.window;
    if (window == nil)
        return;
    CGRect keyboardFrame = [self.view convertRect:screenKeyboardFrame fromView:window];
    if (CGRectIsNull(keyboardFrame))
        return;
    CGRect overlap = CGRectIntersection(keyboardFrame, self.view.bounds);
    CGFloat overlapHeight = CGRectIsNull(overlap) ? 0.0 : overlap.size.height;
    // The pip sits off the safe-area guide already, so only push it up by however much
    // the covering view eats into the safe area, not by its full height.
    CGFloat extra = MAX(0.0, overlapHeight - self.view.safeAreaInsets.bottom);
    _menuPipBottomConstraint.constant = -(16.0 + extra);

    NSNumber *interval = notification.userInfo[UIKeyboardAnimationDurationUserInfoKey];
    [UIView animateWithDuration:interval != nil ? interval.doubleValue : 0.25
                     animations:^{
        [self.view layoutIfNeeded];
    }];
}

// Standalone-only lower-right pip: mirrors the Workspace desktop's corner
// menu button so it's reachable from every mode. Workspace-desktop actions
// (windows, desktops, launcher) don't exist here; this carries the ones that
// do.
- (void)menuPipTapped:(UIButton *)sender {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Wayland Display"
                                            message:nil
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    [sheet addAction:[UIAlertAction actionWithTitle:@"Open Workspace…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [weakSelf switchToWorkspace:sender];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Settings"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        // Form sheet: dismissable by swipe-down, so Settings can't strand the
        // session (the About screen has no Done button of its own when it
        // isn't hosted in a Workspace window).
        UINavigationController *settings = ISHCreateAboutNavigationController(NO, NO);
        settings.modalPresentationStyle = UIModalPresentationFormSheet;
        [weakSelf presentViewController:settings animated:YES completion:nil];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Reconnect"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [weakSelf reconnect:sender];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sender;
        popover.sourceRect = sender.bounds;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

// Standalone (startup-mode) escape hatch: swap the scene's root over to the
// Workspace. Releasing this controller tears the guest Wayland session down
// (dealloc -> teardownSession -> pty SIGHUP), exactly like closing the
// windowed applet -- which also means the Workspace's own Display applet can
// then start a fresh session without racing the old one for WAYVNC_PORT.
- (void)switchToWorkspace:(id)sender {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Open Workspace?"
                                            message:@"This ends the current Wayland session. You can reopen it from the Workspace's Display applet."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Open Workspace"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        UIWindow *window = weakSelf.view.window;
        if (window == nil)
            return;
        window.rootViewController = ISHCreateWorkspaceNavigationController();
    }]];
    [self presentViewController:alert animated:YES completion:nil];
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

// No card wrapper: unlike the toolbar, the display fills all remaining
// space edge-to-edge (no rounded corners/border/shadow) so the remote
// framebuffer gets as much room as possible.
- (DisplayRFBView *)displayView {
    if (_displayView == nil) {
        _displayView = [[DisplayRFBView alloc] initWithFrame:CGRectZero];
        [self.toolContentView addSubview:_displayView];
        // Standalone fullscreen: extend to the view's real bottom edge, not
        // the safe-area-inset toolContentView -- otherwise the home-indicator
        // inset leaves a dead black band under the desktop. (toolContentView
        // doesn't clip, so a subview may extend past its bottom.) The rounded
        // corners only make sense inside a Workspace window; at the physical
        // screen edge they'd just notch the desktop.
        NSLayoutYAxisAnchor *bottomAnchor = self.standaloneMode
            ? self.view.bottomAnchor
            : self.toolContentView.bottomAnchor;
        if (self.standaloneMode)
            _displayView.layer.cornerRadius = 0.0;
        [NSLayoutConstraint activateConstraints:@[
            [_displayView.topAnchor constraintEqualToAnchor:_toolbarCard.bottomAnchor constant:8.0],
            [_displayView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
            [_displayView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
            [_displayView.bottomAnchor constraintEqualToAnchor:bottomAnchor],
        ]];
        if (_menuPip != nil)
            [self.view bringSubviewToFront:_menuPip];
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
