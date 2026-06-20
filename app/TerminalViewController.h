//
//  ViewController.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

typedef NS_ENUM(NSInteger, ISHFreshSessionTerminalDisplayMode) {
    ISHFreshSessionTerminalDisplayModeAuto = 0,
    ISHFreshSessionTerminalDisplayModeSessionShell,
    ISHFreshSessionTerminalDisplayModeSystemConsole,
};

@interface TerminalViewController : UIViewController

@property (nonatomic) Terminal *terminal;
@property (nonatomic) ISHFreshSessionTerminalDisplayMode freshSessionTerminalDisplayMode;

- (void)startNewSession;
- (void)showSystemConsoleForCurrentSession;
- (void)showSessionShellForCurrentSession;
- (void)reconnectSessionFromTerminalUUID:(NSUUID *)uuid;
- (void)focusTerminal;
@property (readonly) NSUUID *sessionTerminalUUID; // 0 means invalid
@property UISceneSession *sceneSession API_AVAILABLE(ios(13.0));
@property (nonatomic) BOOL showsWorkspaceDashboardButton;
@property (nonatomic) BOOL embeddedInWorkspaceWindow;

@end

extern struct tty_driver ios_tty_driver;
