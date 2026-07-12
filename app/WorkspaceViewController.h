#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const ISHInitialWindowWorkspaceValue;
extern NSString *const ISHInitialWindowChooseFilesystemValue;
extern UINavigationController *ISHCreateWorkspaceNavigationController(void);
extern UINavigationController *ISHCreateWorkspaceNavigationControllerForTool(NSString *_Nullable toolIdentifier);
extern BOOL ISHShouldLaunchWorkspaceAtStartup(void);
extern NSString *_Nullable ISHWorkspaceToolIdentifierForViewController(UIViewController *viewController);

// Adopted by an applet that can be told to open/reveal a specific guest path
// (e.g. MotePad opening a text file, or the File Manager revealing a file's
// containing folder). Routed through
// -[WorkspaceViewController openWorkspaceToolWithIdentifier:fileGuestPath:].
@protocol WorkspaceFileOpenable <NSObject>
- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath;
@end

@class WorkspaceViewController;

// Base class for every Workspace applet's content view controller. Provides a
// themed background, `toolContentView` (the safe-area-inset area a subclass
// builds its UI in), and factory methods for theme-tracked cards/labels/text
// views/progress views that automatically recolor on a theme change. A
// subclass overrides -viewDidLoad (calling super first) and builds its UI
// there; see WorkspaceFileManager.m or MotePadDocumentStore's owner for a
// worked example.
@interface WorkspaceThemedToolViewController : UIViewController

@property (nonatomic, strong, readonly) UIView *toolContentView;
@property (nonatomic, weak) WorkspaceViewController *workspaceHostViewController;

- (UIView *)workspaceThemeCardView;
- (UILabel *)workspaceThemePrimaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced;
- (UILabel *)workspaceThemeSecondaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced;
- (UILabel *)workspaceThemeAccentLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced;
- (UITextView *)workspaceThemeTextView;
- (UIProgressView *)workspaceThemeProgressView;
- (NSDictionary<NSString *, UIColor *> *)workspaceTheme;
- (void)workspaceApplyTheme;

@end

@interface WorkspaceViewController : UIViewController

- (void)presentDesktopSwitchMenuFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect;

// Opens (or reuses, for a singleton tool) the window for `toolIdentifier`,
// brings it to the front, and — if its content view controller conforms to
// WorkspaceFileOpenable — delivers `guestPath` to it. No-op if the tool
// identifier has no factory registration.
- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier fileGuestPath:(NSString *)guestPath;

@end

NS_ASSUME_NONNULL_END
