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
#import "RootsTableViewController.h"
#import "UserPreferences.h"
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
NSString *const ISHSceneTerminalDisplayModeUserInfoKey = @"TerminalDisplayMode";
NSString *const ISHSceneWorkspaceToolUserInfoKey = @"WorkspaceTool";
static NSString *const ISHSceneTerminalDisplayModeSessionShellValue = @"session-shell";
static NSString *const ISHSceneTerminalDisplayModeSystemConsoleValue = @"system-console";

static BOOL ISHShouldChooseFilesystemAtStartup(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return [initialWindow isEqualToString:ISHInitialWindowChooseFilesystemValue];
}

static UIViewController *CreateRootSelectionViewController(BOOL choosesRootOnSelection, void (^ _Nullable rootSelectionHandler)(NSString *rootName)) {
    UIViewController *rootsViewController = [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:rootsViewController];
    if ([rootsViewController isKindOfClass:RootsTableViewController.class]) {
        RootsTableViewController *rootsTableViewController = (RootsTableViewController *) rootsViewController;
        rootsTableViewController.choosesRootOnSelection = choosesRootOnSelection;
        rootsTableViewController.rootSelectionHandler = rootSelectionHandler;
        if (choosesRootOnSelection)
            rootsTableViewController.title = @"Choose Filesystem";
    }
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

    if (ISHShouldChooseFilesystemAtStartup())
        return nil;

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

static void ISHScheduleDeferredFileProviderDomainSyncRelease(void) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            [Roots.instance resumeDeferredFileProviderDomainSync];
        });
    });
}

static void EnsureSceneWindow(SceneDelegate *delegate, UIScene *scene) API_AVAILABLE(ios(13.0));
static void EnsureSceneWindow(SceneDelegate *delegate, UIScene *scene) {
    if (delegate.window == nil) {
        if (![scene isKindOfClass:UIWindowScene.class])
            return;
        delegate.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *) scene];
    }
}

static void ConfigureTerminalViewController(SceneDelegate *delegate, TerminalViewController *vc, UISceneSession *session, NSUserActivity *activity) API_AVAILABLE(ios(13.0));
static void ConfigureTerminalViewController(SceneDelegate *delegate, TerminalViewController *vc, UISceneSession *session, NSUserActivity *activity) {
    vc.sceneSession = session;
    NSString *terminalDisplayMode = activity.userInfo[ISHSceneTerminalDisplayModeUserInfoKey];
    if ([terminalDisplayMode isEqualToString:ISHSceneTerminalDisplayModeSessionShellValue]) {
        vc.freshSessionTerminalDisplayMode = ISHFreshSessionTerminalDisplayModeSessionShell;
    } else if ([terminalDisplayMode isEqualToString:ISHSceneTerminalDisplayModeSystemConsoleValue]) {
        vc.freshSessionTerminalDisplayMode = ISHFreshSessionTerminalDisplayModeSystemConsole;
    } else {
        vc.freshSessionTerminalDisplayMode = ISHFreshSessionTerminalDisplayModeAuto;
    }
    NSString *terminalUUID = activity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
    if (terminalUUID.length == 0) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.terminal.startNewSession"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        [vc startNewSession];
    } else {
        delegate.terminalUUID = terminalUUID;
        [ISHDiagnosticsStore recordLaunchStage:@"scene.terminal.reconnectSession"
                                       details:@{@"session": session.persistentIdentifier ?: @"",
                                                 @"terminalUUID": terminalUUID}];
        [vc reconnectSessionFromTerminalUUID:
         [[NSUUID alloc] initWithUUIDString:delegate.terminalUUID]];
    }
}

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    [ISHDiagnosticsStore recordLaunchStage:@"scene.willConnect"
                                   details:@{@"session": session.persistentIdentifier ?: @"",
                                             @"recovery": @([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])}];
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.willConnect"
                                  details:@{@"session": session.persistentIdentifier ?: @"",
                                            @"recovery": @([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])}];
    EnsureSceneWindow(self, scene);
    NSUserActivity *requestedActivity = SceneEffectiveRequestedActivity(session, connectionOptions);

    if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.diagnostics"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateAboutNavigationController(NO, YES);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"diagnostics",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.recovery"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateAboutNavigationController(YES, NO);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"recovery",
                                             @"session": session.persistentIdentifier ?: @""});
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
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.initialRootSelection"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = CreateRootSelectionViewController(NO, nil);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"initial-root-selection",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }

    if (requestedActivity.activityType.length == 0 && ISHShouldChooseFilesystemAtStartup()) {
        __weak typeof(self) weakSelf = self;
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.chooseFilesystem"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = CreateRootSelectionViewController(YES, ^(__unused NSString *rootName) {
            __strong typeof(weakSelf) strongSelf = weakSelf;
            UISceneSession *activeSession = strongSelf.window.windowScene.session ?: session;
            [strongSelf continueAfterInitialRootImportForSession:activeSession];
        });
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"choose-filesystem",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }

    NSString *activityType = requestedActivity.activityType;
    BOOL wantsWorkspace = [activityType isEqualToString:ISHSceneActivityTypeWorkspace];
    BOOL wantsTerminal = activityType.length == 0
        || [activityType isEqualToString:ISHSceneActivityTypeTerminal]
        || [activityType isEqualToString:ISHSceneActivityTypeLegacy];
    if (wantsWorkspace) {
        NSString *toolIdentifier = requestedActivity.userInfo[ISHSceneWorkspaceToolUserInfoKey];
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.workspace"
                                       details:@{@"session": session.persistentIdentifier ?: @"",
                                                 @"tool": toolIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateWorkspaceNavigationControllerForTool(toolIdentifier);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace",
                                             @"session": session.persistentIdentifier ?: @"",
                                             @"tool": toolIdentifier ?: @""});
        return;
    }
    if (activityType.length == 0 && ISHShouldLaunchWorkspaceAtStartup()) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.workspace.default"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateWorkspaceNavigationControllerForTool(nil);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace",
                                             @"session": session.persistentIdentifier ?: @""});
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
    [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.terminal"
                                   details:@{@"session": session.persistentIdentifier ?: @"",
                                             @"activityType": activityType ?: @""}];
    ConfigureTerminalViewController(self, vc, session, requestedActivity);
}

- (void)continueAfterInitialRootImportForSession:(UISceneSession *)session {
    if (Roots.instance.needsInitialRootSelection)
        return;
    if ([self.window.rootViewController isKindOfClass:TerminalViewController.class])
        return;

    self.waitingForInitialRootImport = NO;
    if (ISHShouldLaunchWorkspaceAtStartup()) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.workspace.afterInitialImport"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateWorkspaceNavigationControllerForTool(nil);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }
    TerminalViewController *vc = CreateTerminalViewController();
    if (vc == nil)
        return;

    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.terminal.afterInitialImport"
                                   details:@{@"session": session.persistentIdentifier ?: @""}];
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
    [ISHDiagnosticsStore recordLaunchStage:@"scene.didBecomeActive"
                                   details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.didBecomeActive"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    UIViewController *rootViewController = self.window.rootViewController;
    if ([rootViewController isKindOfClass:TerminalViewController.class]) {
        currentTerminalViewController = (TerminalViewController *) rootViewController;
    } else {
        currentTerminalViewController = NULL;
    }
    ISHScheduleDeferredFileProviderDomainSyncRelease();
    ISHScheduleLaunchJournalCompletion(@{@"rootController": NSStringFromClass(rootViewController.class) ?: @"unknown",
                                         @"session": scene.session.persistentIdentifier ?: @""});
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
