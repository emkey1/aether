//
//  ExternalDisplaySceneDelegate.m
//  iSH
//

#import "ExternalDisplaySceneDelegate.h"
#import "Diagnostics.h"
#import "NSObject+SaneKVO.h"
#import "SceneDelegate.h"
#import "TerminalViewController.h"
#import "Theme.h"
#import "UserPreferences.h"
#import "WorkspaceViewController.h"

// Hosts the relocated webView and reports when that content comes or goes. The
// webView can leave without anyone telling us -- closing a Workspace terminal
// window deallocates its TerminalView, whose teardown detaches the webView
// straight out of this container -- and the idle message has to come back when
// it does.
@interface ISHExternalDisplayContainerView : UIView
@property (nonatomic, copy) dispatch_block_t contentDidChange;
@end

@implementation ISHExternalDisplayContainerView

- (void)notifyContentDidChange {
    if (self.contentDidChange == nil)
        return;
    // Deferred: willRemoveSubview: runs before the subview is actually gone, so
    // counting content from inside it would still see the departing view.
    dispatch_async(dispatch_get_main_queue(), self.contentDidChange);
}

- (void)didAddSubview:(UIView *)subview {
    [super didAddSubview:subview];
    [self notifyContentDidChange];
}

- (void)willRemoveSubview:(UIView *)subview {
    [super willRemoveSubview:subview];
    [self notifyContentDidChange];
}

@end

@interface ExternalDisplaySceneDelegate ()

// The controller whose rendering we borrowed, so the right one gets it back on
// disconnect even if the foreground scene has changed in the meantime. Weak:
// if its scene goes away first, TerminalView's own teardown detaches the
// webView and there is nothing left for us to restore.
@property (weak, nonatomic) TerminalViewController *hostedTerminalViewController;
@property (nonatomic) ISHExternalDisplayContainerView *terminalContainerView;
@property (nonatomic) UILabel *idleLabel;

@end

@implementation ExternalDisplaySceneDelegate

// The terminal a window is showing, whether it is the window's own root or a
// terminal hosted in the frontmost Workspace desktop window. Workspace never
// sets currentTerminalViewController (its root is never a TerminalViewController),
// so without this branch a Workspace user gets an idle external display.
static TerminalViewController *ISHTerminalViewControllerForWindow(UIWindow *window) {
    if ([window.rootViewController isKindOfClass:TerminalViewController.class])
        return (TerminalViewController *) window.rootViewController;
    WorkspaceViewController *workspace = ISHWorkspaceViewControllerForViewController(window.rootViewController);
    return workspace.frontmostHostedTerminalViewController;
}

// The terminal to show over there: whatever the user is actually looking at,
// then progressively weaker guesses. Asking what's frontmost first matters
// because the Workspace is often *presented over* a terminal scene -- consulting
// currentTerminalViewController first would hand the display a terminal that is
// covered up, and leave the Workspace terminal the user just opened rendering in
// a postage-stamp window on the phone.
static TerminalViewController *ISHTerminalViewControllerForExternalDisplay(void) {

    UIViewController *frontmost = ISHActivePresentationViewController();
    TerminalViewController *workspaceTerminal =
        ISHWorkspaceViewControllerForViewController(frontmost).frontmostHostedTerminalViewController;
    if (workspaceTerminal != nil)
        return workspaceTerminal;
    if ([frontmost isKindOfClass:TerminalViewController.class])
        return (TerminalViewController *) frontmost;

    // Nothing frontmost to go on -- the external scene can connect while the app
    // is in the background, when no scene has reported itself active.
    if (currentTerminalViewController != nil && currentTerminalViewController.isViewLoaded)
        return currentTerminalViewController;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        for (UIWindow *window in ((UIWindowScene *) scene).windows) {
            TerminalViewController *terminalViewController = ISHTerminalViewControllerForWindow(window);
            if (terminalViewController != nil)
                return terminalViewController;
        }
    }
    return nil;
}

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if (![scene isKindOfClass:UIWindowScene.class])
        return;
    UIWindowScene *windowScene = (UIWindowScene *) scene;
    [ISHDiagnosticsStore recordBreadcrumb:@"externalDisplay.willConnect"
                                  details:@{@"session": session.persistentIdentifier ?: @"",
                                            @"size": NSStringFromCGSize(windowScene.screen.bounds.size)}];

    UIWindow *window = [[UIWindow alloc] initWithWindowScene:windowScene];
    // The scene's coordinate space -- not the screen -- is the authoritative
    // geometry for the window, and didUpdateCoordinateSpace below keeps us in
    // sync with it when the display renegotiates its mode after connecting.
    window.frame = windowScene.coordinateSpace.bounds;
    UIViewController *rootViewController = [UIViewController new];
    ISHExternalDisplayContainerView *container = [ISHExternalDisplayContainerView new];
    __weak typeof(self) weakSelf = self;
    container.contentDidChange = ^{
        [weakSelf externalContentDidChange];
    };
    // Constrained rather than autoresized: the root view's bounds at this point
    // are a placeholder UIKit picks before the window adopts it, and an
    // autoresizing container inherits that wrong size (observed as the terminal
    // rendering into a 720x480 corner of a 1080p display). Constraints make the
    // container track the window whatever order the sizing happens in.
    container.translatesAutoresizingMaskIntoConstraints = NO;
    [rootViewController.view addSubview:container];
    [NSLayoutConstraint activateConstraints:@[
        [container.leadingAnchor constraintEqualToAnchor:rootViewController.view.leadingAnchor],
        [container.trailingAnchor constraintEqualToAnchor:rootViewController.view.trailingAnchor],
        [container.topAnchor constraintEqualToAnchor:rootViewController.view.topAnchor],
        [container.bottomAnchor constraintEqualToAnchor:rootViewController.view.bottomAnchor],
    ]];
    self.terminalContainerView = container;
    [self installIdleLabelInContainer:container];
    window.rootViewController = rootViewController;
    self.window = window;
    // The webView is installed with the container's bounds, so the container has
    // to have its real size before anything is relocated into it.
    [window layoutIfNeeded];
    [self applyPaletteBackground];
    // hterm paints its own background as transparent, so the terminal's colour
    // has to come from whatever hosts the webView -- here, this container.
    [UserPreferences.shared observe:@[@"theme", @"colorScheme"] options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self applyPaletteBackground];
        });
    }];
    // Not makeKeyAndVisible: this scene can never become key -- it is the
    // non-interactive external-display role -- so the window is only shown.
    window.hidden = NO;

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(terminalDidAttach:)
                                               name:ISHTerminalViewControllerDidAttachTerminalNotification
                                             object:nil];
    [self attachTerminalIfPossible];
}

- (void)windowScene:(UIWindowScene *)windowScene
didUpdateCoordinateSpace:(id<UICoordinateSpace>)previousCoordinateSpace
interfaceOrientation:(UIInterfaceOrientation)previousInterfaceOrientation
   traitCollection:(UITraitCollection *)previousTraitCollection {
    self.window.frame = windowScene.coordinateSpace.bounds;
    // The container (and the webView autoresized inside it) follows the window,
    // so hterm re-lays out and the new rows/cols reach the tty on their own.
}

- (void)sceneDidDisconnect:(UIScene *)scene {
    [ISHDiagnosticsStore recordBreadcrumb:@"externalDisplay.didDisconnect"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @"",
                                            @"hadTerminal": self.hostedTerminalViewController != nil ? @"yes" : @"no"}];
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:ISHTerminalViewControllerDidAttachTerminalNotification
                                                object:nil];
    [self.hostedTerminalViewController restoreTerminalContentFromExternalView:self.terminalContainerView];
    self.hostedTerminalViewController = nil;
    self.terminalContainerView = nil;
    self.window = nil;
}

- (void)terminalDidAttach:(__unused NSNotification *)notification {
    // A session appeared after we connected (the usual order at cold launch, and
    // what happens when the user starts a new session while plugged in).
    [self attachTerminalIfPossible];
}

- (void)attachTerminalIfPossible {
    if (self.terminalContainerView == nil)
        return;
    TerminalViewController *vc = ISHTerminalViewControllerForExternalDisplay();
    if (vc == nil)
        return;
    // Already showing this controller: nothing to do. A *different* controller
    // becoming frontmost does take the display over, which is what makes
    // switching sessions or windows behave the way the user expects.
    if (vc == self.hostedTerminalViewController && vc.rendersOnExternalDisplay)
        return;
    if (self.hostedTerminalViewController != nil && self.hostedTerminalViewController != vc)
        [self.hostedTerminalViewController restoreTerminalContentFromExternalView:self.terminalContainerView];
    if (![vc relocateTerminalContentToExternalView:self.terminalContainerView])
        return;
    self.hostedTerminalViewController = vc;
    [ISHDiagnosticsStore recordBreadcrumb:@"externalDisplay.attachedTerminal"
                                  details:@{@"size": NSStringFromCGSize(self.terminalContainerView.bounds.size)}];
}

- (void)installIdleLabelInContainer:(UIView *)container {
    UILabel *label = [UILabel new];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle3];
    // Declaring the external-display scene means iOS no longer mirrors the
    // device, so an idle display would otherwise just be a blank field with no
    // hint that anything is working. Naming the fix (open a terminal) covers
    // both a Workspace desktop with no terminal window and the standalone
    // Wayland Display, neither of which can hand anything over yet.
    label.text = @"iSH-AOK\n\nOpen a terminal to use this display.";
    [container addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:container.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:container.centerYAnchor],
        [label.leadingAnchor constraintGreaterThanOrEqualToAnchor:container.leadingAnchor constant:20],
        [label.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor constant:-20],
    ]];
    self.idleLabel = label;
}

- (void)externalContentDidChange {
    [self updateIdleMessage];
    // Losing the content usually means the terminal that was up here went away
    // (its Workspace window was closed, say). If another one is open, take it
    // over rather than sitting idle.
    if (!self.idleLabel.isHidden)
        [self attachTerminalIfPossible];
}

- (void)updateIdleMessage {
    BOOL hasContent = NO;
    for (UIView *subview in self.terminalContainerView.subviews) {
        if (subview != self.idleLabel) {
            hasContent = YES;
            break;
        }
    }
    self.idleLabel.hidden = hasContent;
}

- (void)applyPaletteBackground {
    Palette *palette = UserPreferences.shared.palette;
    UIColor *background = [[UIColor alloc] ish_initWithHexString:palette.backgroundColor];
    self.window.backgroundColor = background ?: UIColor.blackColor;
    self.terminalContainerView.backgroundColor = background ?: UIColor.blackColor;
    // Not a system label colour: this window has no relation to the system's
    // light/dark trait, and the terminal palette can be dark under a light
    // trait (or the reverse), which left the message barely legible against
    // its own background. The palette's own foreground always contrasts with it.
    UIColor *foreground = [[UIColor alloc] ish_initWithHexString:palette.foregroundColor];
    self.idleLabel.textColor = [foreground ?: UIColor.whiteColor colorWithAlphaComponent:0.65];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

@end
