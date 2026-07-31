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

@interface ExternalDisplaySceneDelegate ()

// The controller whose rendering we borrowed, so the right one gets it back on
// disconnect even if the foreground scene has changed in the meantime. Weak:
// if its scene goes away first, TerminalView's own teardown detaches the
// webView and there is nothing left for us to restore.
@property (weak, nonatomic) TerminalViewController *hostedTerminalViewController;
@property (nonatomic) UIView *terminalContainerView;

@end

@implementation ExternalDisplaySceneDelegate

// The terminal to show over there. Prefer whichever scene is frontmost, but
// fall back to scanning: the external scene can connect while the app is in the
// background, when no scene has reported itself active.
static TerminalViewController *ISHTerminalViewControllerForExternalDisplay(void) {
    if (currentTerminalViewController != nil && currentTerminalViewController.isViewLoaded)
        return currentTerminalViewController;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        for (UIWindow *window in ((UIWindowScene *) scene).windows) {
            if ([window.rootViewController isKindOfClass:TerminalViewController.class])
                return (TerminalViewController *) window.rootViewController;
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
    UIView *container = [UIView new];
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

- (void)applyPaletteBackground {
    UIColor *background = [[UIColor alloc] ish_initWithHexString:UserPreferences.shared.palette.backgroundColor];
    self.window.backgroundColor = background ?: UIColor.blackColor;
    self.terminalContainerView.backgroundColor = background ?: UIColor.blackColor;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

@end
