//
//  SceneDelegate.h
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import <UIKit/UIKit.h>
#import "TerminalViewController.h"

NS_ASSUME_NONNULL_BEGIN

extern TerminalViewController *currentTerminalViewController;
extern NSString *const ISHSceneActivityTypeTerminal;
extern NSString *const ISHSceneActivityTypeWorkspace;
extern NSString *const ISHSceneTerminalUUIDUserInfoKey;
extern NSString *const ISHSceneTerminalDisplayModeUserInfoKey;
extern NSString *const ISHSceneWorkspaceToolUserInfoKey;

// A view controller to present modal UI (e.g. the iOS-folder-mount document
// picker) from, regardless of which scene or mode -- Terminal or Workspace --
// is currently on screen, and regardless of which thread the request came
// from (a guest task driven entirely over ssh has no foreground terminal at
// all). Falls back to any connected window scene when nothing is foreground-
// active, so a request made while the app is backgrounded still queues UI
// that appears as soon as the app is reopened. Returns nil only when the
// process has no window scene at all. Must be called on the main thread.
UIViewController * _Nullable ISHActivePresentationViewController(void);

API_AVAILABLE(ios(13))
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>

@property (nonatomic) UIWindow *window;

@end

NS_ASSUME_NONNULL_END
