//
//  SceneDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import "SceneDelegate.h"
#import "AboutViewController.h"
#import "AppDelegate.h"
#import "Diagnostics.h"
#import "NSObject+SaneKVO.h"
#import "Roots.h"
#import "WorkspaceViewController.h"

TerminalViewController *currentTerminalViewController = NULL;

@interface SceneDelegate ()

@property NSString *terminalUUID;
@property BOOL waitingForInitialRootImport;

@end

static NSString *const ISHSceneActivityTypeLegacy = @"app.ish.scene";
NSString *const ISHSceneActivityTypeTerminal = @"app.ish.scene.terminal";
NSString *const ISHSceneActivityTypeWorkspace = @"app.ish.scene.workspace";
NSString *const ISHSceneTerminalUUIDUserInfoKey = @"TerminalUUID";
NSString *const ISHSceneWorkspaceToolUserInfoKey = @"WorkspaceTool";

static UIViewController *CreateRootSelectionViewController(void) {
    UIViewController *rootsViewController = [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:rootsViewController];
    return navigationController;
}

static TerminalViewController *CreateTerminalViewController(void) {
    UIViewController *viewController = [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    return [viewController isKindOfClass:TerminalViewController.class] ? (TerminalViewController *) viewController : nil;
}

static NSUserActivity *SceneRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) API_AVAILABLE(ios(13.0));
static NSUserActivity *SceneRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) {
    NSUserActivity *activity = connectionOptions.userActivities.anyObject;
    if (activity != nil)
        return activity;
    return session.stateRestorationActivity;
}

static NSUserActivity *SceneEffectiveRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) API_AVAILABLE(ios(13.0));
static NSUserActivity *SceneEffectiveRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) {
    NSUserActivity *explicitActivity = connectionOptions.userActivities.anyObject;
    if (explicitActivity != nil)
        return explicitActivity;

    NSUserActivity *restorationActivity = session.stateRestorationActivity;
    NSString *restorationType = restorationActivity.activityType;
    if (restorationType.length == 0)
        return restorationActivity;

    BOOL prefersWorkspace = ISHShouldLaunchWorkspaceAtStartup();
    BOOL restoresWorkspace = [restorationType isEqualToString:ISHSceneActivityTypeWorkspace];
    if (prefersWorkspace != restoresWorkspace)
        return nil;
    return restorationActivity;
}

static void EnsureSceneWindow(SceneDelegate *delegate, UIScene *scene) API_AVAILABLE(ios(13.0));
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

static void ConfigureTerminalViewController(SceneDelegate *delegate, TerminalViewController *vc, UISceneSession *session, NSUserActivity *activity) API_AVAILABLE(ios(13.0));
static void ConfigureTerminalViewController(SceneDelegate *delegate, TerminalViewController *vc, UISceneSession *session, NSUserActivity *activity) {
    vc.sceneSession = session;
    NSString *terminalUUID = activity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
    if (terminalUUID.length == 0) {
        [vc startNewSession];
    } else {
        delegate.terminalUUID = terminalUUID;
        [vc reconnectSessionFromTerminalUUID:
         [[NSUUID alloc] initWithUUIDString:delegate.terminalUUID]];
    }
}

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.willConnect"
                                  details:@{@"session": session.persistentIdentifier ?: @"",
                                            @"recovery": @([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])}];
    EnsureSceneWindow(self, scene);
    NSUserActivity *requestedActivity = SceneEffectiveRequestedActivity(session, connectionOptions);

    if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
        self.window.rootViewController = ISHCreateAboutNavigationController(NO, YES);
        [self.window makeKeyAndVisible];
        return;
    }
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        self.window.rootViewController = ISHCreateAboutNavigationController(YES, NO);
        [self.window makeKeyAndVisible];
        return;
    }

    if (Roots.instance.needsInitialRootSelection) {
        [NSNotificationCenter.defaultCenter removeObserver:self
                                                      name:RootsDidFinishInitialSelectionNotification
                                                    object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(rootsDidFinishInitialSelection:)
                                                   name:RootsDidFinishInitialSelectionNotification
                                                 object:nil];
        self.window.rootViewController = CreateRootSelectionViewController();
        [self.window makeKeyAndVisible];
        return;
    }

    NSString *activityType = requestedActivity.activityType;
    BOOL wantsWorkspace = [activityType isEqualToString:ISHSceneActivityTypeWorkspace];
    BOOL wantsTerminal = activityType.length == 0
        || [activityType isEqualToString:ISHSceneActivityTypeTerminal]
        || [activityType isEqualToString:ISHSceneActivityTypeLegacy];
    if (wantsWorkspace) {
        NSString *toolIdentifier = requestedActivity.userInfo[ISHSceneWorkspaceToolUserInfoKey];
        self.window.rootViewController = ISHCreateWorkspaceNavigationControllerForTool(toolIdentifier);
        [self.window makeKeyAndVisible];
        return;
    }
    if (activityType.length == 0 && ISHShouldLaunchWorkspaceAtStartup()) {
        self.window.rootViewController = ISHCreateWorkspaceNavigationController();
        [self.window makeKeyAndVisible];
        return;
    }
    if (!wantsTerminal) {
        [ISHDiagnosticsStore recordBreadcrumb:@"scene.unknownActivityType"
                                      details:@{@"activityType": activityType ?: @""}];
    }

    TerminalViewController *vc = [self.window.rootViewController isKindOfClass:TerminalViewController.class]
        ? (TerminalViewController *) self.window.rootViewController
        : CreateTerminalViewController();
    if (vc == nil)
        return;
    if (self.window.rootViewController != vc) {
        self.window.rootViewController = vc;
        [self.window makeKeyAndVisible];
    }
    ConfigureTerminalViewController(self, vc, session, requestedActivity);
}

- (void)continueAfterInitialRootImportForSession:(UISceneSession *)session {
    if (Roots.instance.needsInitialRootSelection)
        return;
    if ([self.window.rootViewController isKindOfClass:TerminalViewController.class])
        return;

    self.waitingForInitialRootImport = NO;
    if (ISHShouldLaunchWorkspaceAtStartup()) {
        self.window.rootViewController = ISHCreateWorkspaceNavigationController();
        [self.window makeKeyAndVisible];
        return;
    }
    TerminalViewController *vc = CreateTerminalViewController();
    if (vc == nil)
        return;

    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    ConfigureTerminalViewController(self, vc, session, SceneEffectiveRequestedActivity(session, nil));
}

- (void)rootsDidFinishInitialSelection:(__unused NSNotification *)notification {
    UISceneSession *session = self.window.windowScene.session;
    if (session == nil)
        return;
    [self continueAfterInitialRootImportForSession:session];
}

- (NSUserActivity *)stateRestorationActivityForScene:(UIScene *)scene {
    UIViewController *rootViewController = self.window.rootViewController;
    UIViewController *topViewController = [rootViewController isKindOfClass:UINavigationController.class]
        ? ((UINavigationController *) rootViewController).topViewController
        : rootViewController;
    UIViewController *presentedViewController = topViewController.presentedViewController;
    UIViewController *presentedTopViewController = [presentedViewController isKindOfClass:UINavigationController.class]
        ? ((UINavigationController *) presentedViewController).topViewController
        : presentedViewController;
    NSString *presentedWorkspaceToolIdentifier = ISHWorkspaceToolIdentifierForViewController(presentedTopViewController);
    if (presentedWorkspaceToolIdentifier.length > 0) {
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace];
        [activity addUserInfoEntriesFromDictionary:@{ISHSceneWorkspaceToolUserInfoKey: presentedWorkspaceToolIdentifier}];
        return activity;
    }
    if ([topViewController isKindOfClass:WorkspaceViewController.class]) {
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace];
        NSString *toolIdentifier = ISHWorkspaceToolIdentifierForViewController(topViewController);
        if (toolIdentifier.length > 0) {
            [activity addUserInfoEntriesFromDictionary:@{ISHSceneWorkspaceToolUserInfoKey: toolIdentifier}];
        }
        return activity;
    }
    NSString *workspaceToolIdentifier = ISHWorkspaceToolIdentifierForViewController(topViewController);
    if (workspaceToolIdentifier.length > 0) {
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace];
        [activity addUserInfoEntriesFromDictionary:@{ISHSceneWorkspaceToolUserInfoKey: workspaceToolIdentifier}];
        return activity;
    }

    NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeTerminal];
    TerminalViewController *vc = (TerminalViewController *) rootViewController;
    if ([vc isKindOfClass:TerminalViewController.class]) {
        self.terminalUUID = vc.sessionTerminalUUID.UUIDString;
        if (self.terminalUUID != nil) {
            [activity addUserInfoEntriesFromDictionary:@{ISHSceneTerminalUUIDUserInfoKey: self.terminalUUID}];
        }
    }
    return activity;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.didBecomeActive"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    UIViewController *rootViewController = self.window.rootViewController;
    if ([rootViewController isKindOfClass:TerminalViewController.class]) {
        currentTerminalViewController = (TerminalViewController *) rootViewController;
    } else {
        currentTerminalViewController = NULL;
    }
}

- (void)sceneWillResignActive:(UIScene *)scene {
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.willResignActive"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    UIViewController *rootViewController = self.window.rootViewController;
    TerminalViewController *terminalViewController = [rootViewController isKindOfClass:TerminalViewController.class]
        ? (TerminalViewController *) rootViewController
        : NULL;

    if (currentTerminalViewController == terminalViewController) {
        currentTerminalViewController = NULL;
    }
}

@end
