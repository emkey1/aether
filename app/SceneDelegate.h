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

API_AVAILABLE(ios(13))
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>

@property (nonatomic) UIWindow *window;

@end

NS_ASSUME_NONNULL_END
