//
//  SceneDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import "SceneDelegate.h"
#import "AboutViewController.h"
#import "AppDelegate.h"
#import "NSObject+SaneKVO.h"
#import "Roots.h"

TerminalViewController *currentTerminalViewController = NULL;

@interface SceneDelegate ()

@property NSString *terminalUUID;
@property BOOL waitingForInitialRootImport;

@end

static NSString *const TerminalUUID = @"TerminalUUID";

static UIViewController *CreateRootSelectionViewController(void) {
    UIViewController *rootsViewController = [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:rootsViewController];
    return navigationController;
}

static TerminalViewController *CreateTerminalViewController(void) {
    UIViewController *viewController = [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    return [viewController isKindOfClass:TerminalViewController.class] ? (TerminalViewController *) viewController : nil;
}

static void EnsureSceneWindow(SceneDelegate *delegate, UIScene *scene) {
    if (delegate.window == nil) {
        if (![scene isKindOfClass:UIWindowScene.class])
            return;
        delegate.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *) scene];
    }
    if (delegate.window.rootViewController == nil) {
        delegate.window.rootViewController = CreateTerminalViewController();
        [delegate.window makeKeyAndVisible];
    }
}

static void ConfigureTerminalViewController(SceneDelegate *delegate, TerminalViewController *vc, UISceneSession *session) {
    vc.sceneSession = session;
    if (session.stateRestorationActivity == nil) {
        [vc startNewSession];
    } else {
        delegate.terminalUUID = session.stateRestorationActivity.userInfo[TerminalUUID];
        [vc reconnectSessionFromTerminalUUID:
         [[NSUUID alloc] initWithUUIDString:delegate.terminalUUID]];
    }
}

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    EnsureSceneWindow(self, scene);
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        UINavigationController *vc = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
        AboutViewController *avc = (AboutViewController *) vc.topViewController;
        avc.recoveryMode = YES;
        self.window.rootViewController = vc;
        [self.window makeKeyAndVisible];
        return;
    }

    if (Roots.instance.needsInitialRootSelection) {
        self.waitingForInitialRootImport = Roots.instance.initialBundledRootImportInProgress;
        if (self.waitingForInitialRootImport) {
            [Roots.instance observe:@[@"roots", @"initialBundledRootImportInProgress"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
                [self continueAfterInitialRootImportForSession:session];
            }];
        }
        self.window.rootViewController = CreateRootSelectionViewController();
        [self.window makeKeyAndVisible];
        return;
    }

    TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
    ConfigureTerminalViewController(self, vc, session);
}

- (void)continueAfterInitialRootImportForSession:(UISceneSession *)session {
    if (!self.waitingForInitialRootImport)
        return;
    if (Roots.instance.initialBundledRootImportInProgress)
        return;
    if (Roots.instance.needsInitialRootSelection)
        return;

    self.waitingForInitialRootImport = NO;
    TerminalViewController *vc = CreateTerminalViewController();
    if (vc == nil)
        return;

    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    ConfigureTerminalViewController(self, vc, session);
}

- (NSUserActivity *)stateRestorationActivityForScene:(UIScene *)scene {
    NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:@"app.ish.scene"];
    TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
    if ([vc isKindOfClass:TerminalViewController.class]) {
        self.terminalUUID = vc.sessionTerminalUUID.UUIDString;
        if (self.terminalUUID != nil) {
            [activity addUserInfoEntriesFromDictionary:@{TerminalUUID: self.terminalUUID}];
        }
    }
    return activity;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    UIViewController *rootViewController = self.window.rootViewController;
    if ([rootViewController isKindOfClass:TerminalViewController.class]) {
        currentTerminalViewController = (TerminalViewController *) rootViewController;
    } else {
        currentTerminalViewController = NULL;
    }
}

- (void)sceneWillResignActive:(UIScene *)scene {
    UIViewController *rootViewController = self.window.rootViewController;
    TerminalViewController *terminalViewController = [rootViewController isKindOfClass:TerminalViewController.class]
        ? (TerminalViewController *) rootViewController
        : NULL;

    if (currentTerminalViewController == terminalViewController) {
        currentTerminalViewController = NULL;
    }
}

@end
