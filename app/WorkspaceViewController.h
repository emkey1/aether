#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const ISHInitialWindowWorkspaceValue;
extern UINavigationController *ISHCreateWorkspaceNavigationController(void);
extern UINavigationController *ISHCreateWorkspaceNavigationControllerForTool(NSString *_Nullable toolIdentifier);
extern BOOL ISHShouldLaunchWorkspaceAtStartup(void);
extern NSString *_Nullable ISHWorkspaceToolIdentifierForViewController(UIViewController *viewController);

@interface WorkspaceViewController : UIViewController

@end

NS_ASSUME_NONNULL_END
