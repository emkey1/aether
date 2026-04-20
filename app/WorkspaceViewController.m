#import "WorkspaceViewController.h"

#import "AboutViewController.h"
#import "Diagnostics.h"
#import "Roots.h"
#import "SceneDelegate.h"
#include "fs/devices.h"
#include "kernel/init.h"
#import "TerminalViewController.h"
#import "UserPreferences.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <net/if.h>
#include <sys/sysctl.h>

@class ISHWorkspaceContainedWindowView;

@interface WorkspaceViewController ()

@property (nonatomic, copy) NSString *initialToolIdentifier;
@property (nonatomic) BOOL didOpenInitialTool;
@property (nonatomic) BOOL didEnsureDefaultWorkspaceUtilities;
@property (nonatomic, strong) UIView *desktopSurfaceView;
@property (nonatomic, strong) UIImageView *desktopWallpaperView;
@property (nonatomic, copy) NSString *appliedWallpaperThemeIdentifier;
@property (nonatomic) CGSize appliedWallpaperImageSize;
@property (nonatomic, strong) NSMutableArray<UIView *> *desktopWindows;
@property (nonatomic) NSInteger desktopWindowCascadeIndex;
@property (nonatomic, weak) ISHWorkspaceContainedWindowView *dashboardWindow;
@property (nonatomic, weak) ISHWorkspaceContainedWindowView *dockWindow;
@property (nonatomic) CGSize dashboardExpandedSize;
@property (nonatomic) BOOL dashboardIsCompact;
@property (nonatomic, strong) UILabel *windowSummaryLabel;
@property (nonatomic, strong) UIButton *dockUtilsButton;
@property (nonatomic, strong) UIButton *dockTerminalButton;
@property (nonatomic, strong) UIStackView *sceneWindowsStack;
@property (nonatomic, strong) UIStackView *bodyStack;
@property (nonatomic, strong) UIView *windowCard;

- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier;
- (void)openOrFocusWorkspaceToolIdentifier:(NSString *)toolIdentifier;
- (void)ensureDefaultWorkspaceUtilitiesOpen;
- (void)openDashboardWindow:(id)sender;
- (void)openNewWorkspaceWindow:(id)sender;
- (void)openTerminalHerePreferringConsole:(BOOL)preferConsole;
- (void)openExistingTerminalHereWithUUID:(NSUUID *)terminalUUID;
- (void)focusSceneWithPersistentIdentifier:(NSString *)identifier;
- (ISHWorkspaceContainedWindowView *)desktopWindowForToolIdentifier:(NSString *)toolIdentifier;
- (ISHWorkspaceContainedWindowView *)desktopWindowHostingTerminalUUID:(NSUUID *)terminalUUID;
- (void)focusDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView;

- (UISceneSession *)sceneSessionHostingTerminalUUID:(NSUUID *)terminalUUID API_AVAILABLE(ios(13.0));
- (BOOL)focusSceneSession:(UISceneSession *)sceneSession title:(NSString *)title API_AVAILABLE(ios(13.0));

@end

NSString *const ISHInitialWindowWorkspaceValue = @"workspace";
NSString *const ISHInitialWindowChooseFilesystemValue = @"choose-filesystem";
static NSString *const ISHWorkspaceToolClockIdentifier = @"clock";
static NSString *const ISHWorkspaceToolInfoIdentifier = @"info";
static NSString *const ISHWorkspaceToolMonitorIdentifier = @"monitor";
static NSString *const ISHWorkspaceToolNetworksIdentifier = @"networks";
static NSString *const ISHWorkspaceToolStatusIdentifier = @"status";
static NSString *const ISHWorkspaceToolWorkspacesIdentifier = @"workspaces";
static NSString *const ISHWorkspaceToolProcessesIdentifier = @"processes";
static NSString *const ISHWorkspaceToolSessionsIdentifier = @"sessions";
static NSString *const ISHWorkspaceToolStorageIdentifier = @"storage";
static NSString *const ISHWorkspaceToolShortcutsIdentifier = @"shortcuts";
static NSString *const ISHWorkspaceToolThemesIdentifier = @"themes";
static NSString *const ISHWorkspaceToolFilesystemsIdentifier = @"filesystems";
static NSString *const ISHWorkspaceToolSettingsIdentifier = @"settings";
static NSString *const ISHWorkspaceToolDiagnosticsIdentifier = @"diagnostics";
static NSString *const ISHWorkspaceSavedLayoutDefaultsKey = @"ISHWorkspaceSavedLayout";
static NSString *const ISHWorkspaceSavedLayoutKindDashboard = @"dashboard";
static NSString *const ISHWorkspaceSavedLayoutKindDock = @"dock";
static NSString *const ISHWorkspaceSavedLayoutKindTool = @"tool";
static NSString *const ISHWorkspaceSavedLayoutKindTerminal = @"terminal";
static NSString *const ISHWorkspaceTerminalRoleSessionShell = @"session-shell";
static NSString *const ISHWorkspaceTerminalRoleSystemConsole = @"system-console";
static NSString *const ISHWorkspaceTerminalRoleGeneric = @"terminal";
static const CGFloat ISHWorkspaceWindowCornerRadius = 22.0;
static const CGFloat ISHWorkspaceWindowTitleBarHeight = 24.0;
static const CGFloat ISHWorkspaceWindowButtonSize = 18.0;
static const CGFloat ISHWorkspaceWindowButtonInset = 8.0;
static const CGFloat ISHWorkspaceWindowTitleSideInset = 34.0;

static BOOL ISHWorkspaceUsesPhoneLayout(void) {
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
}

static CGRect ISHWorkspaceRectWithRoundedOriginPreservingSize(CGRect frame) {
    frame.origin.x = round(frame.origin.x);
    frame.origin.y = round(frame.origin.y);
    return frame;
}

@interface ISHWorkspaceContainedWindowView : UIView

@property (nonatomic) CGSize preferredSize;
@property (nonatomic) BOOL didApplyInitialFrame;
@property (nonatomic) BOOL draggable;
@property (nonatomic) BOOL resizable;
@property (nonatomic, strong) UIView *titleBarView;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UIButton *closeButton;
@property (nonatomic, strong) UIButton *utilityButton;
@property (nonatomic, strong) UIView *contentContainerView;
@property (nonatomic, strong) UIView *panelView;
@property (nonatomic, strong) UIView *resizeHandleView;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleTopConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleBottomConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleTrailingConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleLeadingConstraint;
@property (nonatomic, copy, nullable) dispatch_block_t closeHandler;
@property (nonatomic, copy, nullable) dispatch_block_t utilityHandler;
@property (nonatomic, copy, nullable) dispatch_block_t didBecomeFrontmostHandler;
@property (nonatomic, weak) TerminalViewController *hostedTerminalViewController;
@property (nonatomic, copy) NSString *workspaceToolIdentifier;
@property (nonatomic, copy) NSString *workspaceTerminalRole;
@property (nonatomic) BOOL pinnedToBottomCenter;
@property (nonatomic) BOOL resizeHandleAtTopRight;
@property (nonatomic) CGSize minimumSize;
@property (nonatomic) CGSize maximumSize;

- (instancetype)initWithTitle:(NSString *)title showsCloseButton:(BOOL)showsCloseButton;
- (void)setUtilityButtonTitle:(nullable NSString *)title handler:(nullable dispatch_block_t)handler;
- (void)applyWorkspaceChromeTheme:(NSDictionary<NSString *, UIColor *> *)theme active:(BOOL)active;

@end

@implementation ISHWorkspaceContainedWindowView

- (instancetype)initWithTitle:(NSString *)title showsCloseButton:(BOOL)showsCloseButton {
    self = [super initWithFrame:CGRectZero];
    if (self == nil)
        return nil;

    self.autoresizingMask = UIViewAutoresizingNone;
    self.backgroundColor = UIColor.clearColor;
    self.layer.cornerRadius = ISHWorkspaceWindowCornerRadius;
    self.layer.masksToBounds = NO;
    self.layer.shadowColor = UIColor.blackColor.CGColor;
    self.layer.shadowOpacity = 0.18;
    self.layer.shadowRadius = 28;
    self.layer.shadowOffset = CGSizeMake(0, 16);
    self.draggable = YES;
    self.resizable = NO;
    self.pinnedToBottomCenter = NO;
    self.resizeHandleAtTopRight = NO;
    self.minimumSize = CGSizeMake(280, 180);
    self.maximumSize = CGSizeZero;

    self.panelView = [UIView new];
    self.panelView.translatesAutoresizingMaskIntoConstraints = NO;
    self.panelView.layer.cornerRadius = ISHWorkspaceWindowCornerRadius;
    self.panelView.layer.masksToBounds = YES;
    self.panelView.layer.borderWidth = 1;
    if (@available(iOS 13.0, *)) {
        self.panelView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        self.panelView.backgroundColor = UIColor.whiteColor;
    }
    [self addSubview:self.panelView];

    self.titleBarView = [UIView new];
    self.titleBarView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.titleBarView.backgroundColor = [UIColor.secondarySystemBackgroundColor colorWithAlphaComponent:0.96];
    } else {
        self.titleBarView.backgroundColor = [UIColor colorWithWhite:0.94 alpha:1.0];
    }
    [self.panelView addSubview:self.titleBarView];

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    self.titleLabel.textAlignment = NSTextAlignmentCenter;
    self.titleLabel.text = title;
    if (@available(iOS 13.0, *)) {
        self.titleLabel.textColor = UIColor.labelColor;
    } else {
        self.titleLabel.textColor = UIColor.blackColor;
    }
    [self.titleBarView addSubview:self.titleLabel];

    self.closeButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.closeButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.closeButton setTitle:@"×" forState:UIControlStateNormal];
    self.closeButton.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    self.closeButton.hidden = !showsCloseButton;
    self.closeButton.alpha = showsCloseButton ? 1.0 : 0.0;
    self.closeButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.82];
    self.closeButton.layer.cornerRadius = ISHWorkspaceWindowButtonSize * 0.5;
    [self.closeButton addTarget:self action:@selector(closePressed:) forControlEvents:UIControlEventTouchUpInside];
    [self.titleBarView addSubview:self.closeButton];

    self.utilityButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.utilityButton.translatesAutoresizingMaskIntoConstraints = NO;
    self.utilityButton.hidden = YES;
    self.utilityButton.alpha = 0.0;
    self.utilityButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.82];
    self.utilityButton.layer.cornerRadius = ISHWorkspaceWindowButtonSize * 0.5;
    self.utilityButton.titleLabel.font = [UIFont systemFontOfSize:10 weight:UIFontWeightSemibold];
    [self.utilityButton addTarget:self action:@selector(utilityPressed:) forControlEvents:UIControlEventTouchUpInside];
    [self.titleBarView addSubview:self.utilityButton];

    self.contentContainerView = [UIView new];
    self.contentContainerView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.contentContainerView.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.contentContainerView.backgroundColor = UIColor.whiteColor;
    }
    [self.panelView addSubview:self.contentContainerView];

    self.resizeHandleView = [UIView new];
    self.resizeHandleView.translatesAutoresizingMaskIntoConstraints = NO;
    self.resizeHandleView.hidden = YES;
    self.resizeHandleView.alpha = 0.0;
    self.resizeHandleView.layer.cornerRadius = 10;
    if (@available(iOS 13.0, *)) {
        self.resizeHandleView.backgroundColor = [UIColor.tertiaryLabelColor colorWithAlphaComponent:0.9];
    } else {
        self.resizeHandleView.backgroundColor = [UIColor colorWithWhite:0.65 alpha:0.9];
    }
    [self.panelView addSubview:self.resizeHandleView];

    self.resizeHandleLeadingConstraint =
        [self.resizeHandleView.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.panelView.leadingAnchor constant:12];
    self.resizeHandleTrailingConstraint =
        [self.resizeHandleView.trailingAnchor constraintEqualToAnchor:self.panelView.trailingAnchor constant:-12];
    self.resizeHandleTopConstraint =
        [self.resizeHandleView.topAnchor constraintEqualToAnchor:self.panelView.topAnchor constant:12];
    self.resizeHandleBottomConstraint =
        [self.resizeHandleView.bottomAnchor constraintEqualToAnchor:self.panelView.bottomAnchor constant:-12];

    [NSLayoutConstraint activateConstraints:@[
        [self.panelView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [self.panelView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.panelView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.panelView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],

        [self.titleBarView.topAnchor constraintEqualToAnchor:self.panelView.topAnchor],
        [self.titleBarView.leadingAnchor constraintEqualToAnchor:self.panelView.leadingAnchor],
        [self.titleBarView.trailingAnchor constraintEqualToAnchor:self.panelView.trailingAnchor],
        [self.titleBarView.heightAnchor constraintEqualToConstant:ISHWorkspaceWindowTitleBarHeight],

        [self.closeButton.leadingAnchor constraintEqualToAnchor:self.titleBarView.leadingAnchor constant:ISHWorkspaceWindowButtonInset],
        [self.closeButton.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],
        [self.closeButton.widthAnchor constraintEqualToConstant:ISHWorkspaceWindowButtonSize],
        [self.closeButton.heightAnchor constraintEqualToConstant:ISHWorkspaceWindowButtonSize],

        [self.utilityButton.trailingAnchor constraintEqualToAnchor:self.titleBarView.trailingAnchor constant:-ISHWorkspaceWindowButtonInset],
        [self.utilityButton.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],
        [self.utilityButton.widthAnchor constraintEqualToConstant:34],
        [self.utilityButton.heightAnchor constraintEqualToConstant:ISHWorkspaceWindowButtonSize],

        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.titleBarView.leadingAnchor constant:ISHWorkspaceWindowTitleSideInset],
        [self.titleLabel.trailingAnchor constraintEqualToAnchor:self.titleBarView.trailingAnchor constant:-ISHWorkspaceWindowTitleSideInset],
        [self.titleLabel.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],

        [self.contentContainerView.topAnchor constraintEqualToAnchor:self.titleBarView.bottomAnchor],
        [self.contentContainerView.leadingAnchor constraintEqualToAnchor:self.panelView.leadingAnchor],
        [self.contentContainerView.trailingAnchor constraintEqualToAnchor:self.panelView.trailingAnchor],
        [self.contentContainerView.bottomAnchor constraintEqualToAnchor:self.panelView.bottomAnchor],

        [self.resizeHandleView.widthAnchor constraintEqualToConstant:20],
        [self.resizeHandleView.heightAnchor constraintEqualToConstant:20],
        self.resizeHandleLeadingConstraint,
        self.resizeHandleTrailingConstraint,
        self.resizeHandleBottomConstraint,
    ]];

    UIPanGestureRecognizer *panGestureRecognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePan:)];
    [self.titleBarView addGestureRecognizer:panGestureRecognizer];

    UIPanGestureRecognizer *resizeGestureRecognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handleResizePan:)];
    [self.resizeHandleView addGestureRecognizer:resizeGestureRecognizer];

    UITapGestureRecognizer *tapGestureRecognizer = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(bringWindowToFront)];
    tapGestureRecognizer.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tapGestureRecognizer];

    return self;
}

- (void)applyWorkspaceChromeTheme:(NSDictionary<NSString *, UIColor *> *)theme active:(BOOL)active {
    if (![theme isKindOfClass:NSDictionary.class])
        return;

    UIColor *strokeColor = active
        ? [theme[@"accent"] colorWithAlphaComponent:0.92]
        : [theme[@"stroke"] colorWithAlphaComponent:0.98];
    self.panelView.layer.borderWidth = active ? 1.6 : 1.0;
    self.panelView.layer.borderColor = strokeColor.CGColor;
    self.panelView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.98];
    self.titleBarView.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.98];
    self.contentContainerView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.98];
    self.titleLabel.textColor = theme[@"primary"];

    UIColor *buttonBackground = [theme[@"backgroundTop"] colorWithAlphaComponent:0.14];
    UIColor *buttonBorder = [theme[@"accentAlt"] colorWithAlphaComponent:0.34];
    self.closeButton.backgroundColor = buttonBackground;
    self.closeButton.layer.borderWidth = 1;
    self.closeButton.layer.borderColor = buttonBorder.CGColor;
    [self.closeButton setTitleColor:theme[@"accent"] forState:UIControlStateNormal];

    self.utilityButton.backgroundColor = buttonBackground;
    self.utilityButton.layer.borderWidth = 1;
    self.utilityButton.layer.borderColor = buttonBorder.CGColor;
    [self.utilityButton setTitleColor:theme[@"accent"] forState:UIControlStateNormal];

    self.resizeHandleView.backgroundColor = active
        ? [theme[@"accent"] colorWithAlphaComponent:0.92]
        : [theme[@"accentAlt"] colorWithAlphaComponent:0.76];
    self.layer.shadowColor = [theme[@"backgroundTop"] colorWithAlphaComponent:0.55].CGColor;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    UIBezierPath *shadowPath = [UIBezierPath bezierPathWithRoundedRect:self.bounds cornerRadius:ISHWorkspaceWindowCornerRadius];
    self.layer.shadowPath = shadowPath.CGPath;
}

- (void)bringWindowToFront {
    [self.superview bringSubviewToFront:self];
    if (self.didBecomeFrontmostHandler != nil)
        self.didBecomeFrontmostHandler();
}

- (void)closePressed:(id)sender {
    if (self.closeHandler != nil)
        self.closeHandler();
}

- (void)utilityPressed:(id)sender {
    if (self.utilityHandler != nil)
        self.utilityHandler();
}

- (void)setUtilityButtonTitle:(NSString *)title handler:(dispatch_block_t)handler {
    self.utilityHandler = handler;
    BOOL visible = title.length > 0 && handler != nil;
    [self.utilityButton setTitle:title forState:UIControlStateNormal];
    self.utilityButton.hidden = !visible;
    self.utilityButton.alpha = visible ? 1.0 : 0.0;
}

- (void)setResizable:(BOOL)resizable {
    _resizable = resizable;
    self.resizeHandleView.hidden = !resizable;
    self.resizeHandleView.alpha = resizable ? 1.0 : 0.0;
}

- (void)setResizeHandleAtTopRight:(BOOL)resizeHandleAtTopRight {
    if (_resizeHandleAtTopRight == resizeHandleAtTopRight)
        return;
    _resizeHandleAtTopRight = resizeHandleAtTopRight;
    self.resizeHandleTopConstraint.active = resizeHandleAtTopRight;
    self.resizeHandleBottomConstraint.active = !resizeHandleAtTopRight;
}

- (void)handlePan:(UIPanGestureRecognizer *)recognizer {
    if (!self.draggable || self.superview == nil)
        return;

    if (recognizer.state == UIGestureRecognizerStateBegan) {
        self.pinnedToBottomCenter = NO;
        [self bringWindowToFront];
    }

    CGPoint translation = [recognizer translationInView:self.superview];
    CGRect frame = self.frame;
    frame.origin.x += translation.x;
    frame.origin.y += translation.y;
    CGFloat visibleWidth = MIN(CGRectGetWidth(frame), 140.0);
    CGFloat minX = -(CGRectGetWidth(frame) - visibleWidth);
    CGFloat maxX = CGRectGetWidth(self.superview.bounds) - visibleWidth;
    CGFloat visibleHeight = MIN(CGRectGetHeight(frame), ISHWorkspaceWindowTitleBarHeight);
    CGFloat minY = 0;
    CGFloat maxY = CGRectGetHeight(self.superview.bounds) - visibleHeight;
    if (maxX < minX)
        maxX = minX;
    if (maxY < minY)
        maxY = minY;
    frame.origin.x = MIN(MAX(frame.origin.x, minX), maxX);
    frame.origin.y = MIN(MAX(frame.origin.y, minY), maxY);
    self.frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
    [recognizer setTranslation:CGPointZero inView:self.superview];
}

- (void)handleResizePan:(UIPanGestureRecognizer *)recognizer {
    if (!self.resizable || self.superview == nil)
        return;

    if (recognizer.state == UIGestureRecognizerStateBegan) {
        [self bringWindowToFront];
    }

    CGPoint translation = [recognizer translationInView:self.superview];
    CGRect frame = self.frame;
    CGFloat maxWidth = self.pinnedToBottomCenter ? CGRectGetWidth(self.superview.bounds)
                                                 : CGRectGetWidth(self.superview.bounds) - CGRectGetMinX(frame);
    CGFloat maxHeight = self.pinnedToBottomCenter ? CGRectGetMaxY(frame)
                                                  : CGRectGetHeight(self.superview.bounds) - CGRectGetMinY(frame);
    CGSize minimumSize = self.minimumSize;
    CGFloat targetWidth = MAX(minimumSize.width, CGRectGetWidth(frame) + translation.x);
    CGFloat targetHeight = MAX(minimumSize.height,
                               CGRectGetHeight(frame) + (self.resizeHandleAtTopRight ? -translation.y : translation.y));
    if (self.maximumSize.width > 0) {
        targetWidth = MIN(targetWidth, self.maximumSize.width);
    }
    if (self.maximumSize.height > 0) {
        targetHeight = MIN(targetHeight, self.maximumSize.height);
    }
    frame.size.width = MIN(targetWidth, maxWidth);
    frame.size.height = MIN(targetHeight, maxHeight);
    if (self.pinnedToBottomCenter) {
        frame.origin.x = (CGRectGetWidth(self.superview.bounds) - CGRectGetWidth(frame)) * 0.5;
        frame.origin.y = CGRectGetHeight(self.superview.bounds) - CGRectGetHeight(frame);
    }
    self.frame = CGRectIntegral(frame);
    self.preferredSize = frame.size;
    [recognizer setTranslation:CGPointZero inView:self.superview];
}

@end

UINavigationController *ISHCreateWorkspaceNavigationController(void) {
    return ISHCreateWorkspaceNavigationControllerForTool(nil);
}

UINavigationController *ISHCreateWorkspaceNavigationControllerForTool(NSString *toolIdentifier) {
    WorkspaceViewController *workspaceViewController = [WorkspaceViewController new];
    workspaceViewController.initialToolIdentifier = toolIdentifier;
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:workspaceViewController];
    navigationController.navigationBarHidden = YES;
    return navigationController;
}

static UINavigationController *ISHCreateRootsNavigationController(void) {
    UIViewController *rootsViewController = [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
    return [[UINavigationController alloc] initWithRootViewController:rootsViewController];
}

static UIViewController *ISHCreateRootsViewController(void) {
    return [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
}

static CGSize ISHWorkspacePreferredToolContentSize(NSString *toolIdentifier) {
    if (ISHWorkspaceUsesPhoneLayout()) {
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
            return CGSizeMake(144, 74);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
            return CGSizeMake(280, 154);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
            return CGSizeMake(328, 170);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
            return CGSizeMake(328, 176);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
            return CGSizeMake(340, 248);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
            return CGSizeMake(250, 170);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolProcessesIdentifier])
            return CGSizeMake(332, 224);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
            return CGSizeMake(332, 238);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
            return CGSizeMake(336, 256);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
            return CGSizeMake(312, 184);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
            return CGSizeMake(360, 620);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
            return CGSizeMake(352, 620);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
            return CGSizeMake(352, 620);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
            return CGSizeMake(352, 620);
        return CGSizeMake(344, 580);
    }
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return CGSizeMake(156, 76);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return CGSizeMake(318, 168);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return CGSizeMake(360, 182);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return CGSizeMake(360, 188);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return CGSizeMake(460, 300);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return CGSizeMake(300, 190);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolProcessesIdentifier])
        return CGSizeMake(440, 268);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return CGSizeMake(460, 286);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return CGSizeMake(500, 320);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return CGSizeMake(400, 220);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return CGSizeMake(820, 760);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return CGSizeMake(760, 700);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return CGSizeMake(760, 720);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
        return CGSizeMake(760, 760);
    return CGSizeMake(720, 640);
}

static CGSize ISHWorkspacePreferredTerminalContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(372, 288);
    return CGSizeMake(900, 620);
}

static CGSize ISHWorkspaceCompactDashboardSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(332, 236);
    return CGSizeMake(440, 320);
}

static CGSize ISHWorkspacePreferredDockContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(188, 56);
    return CGSizeMake(220, 64);
}

static CGSize ISHWorkspacePreferredDashboardContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(360, 360);
    return CGSizeMake(720, 560);
}

static CGSize ISHWorkspaceMinimumDashboardContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(300, 220);
    return CGSizeMake(420, 220);
}

static CGSize ISHWorkspaceMinimumDockContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(156, 48);
    return CGSizeMake(180, 56);
}

static CGSize ISHWorkspaceMaximumDockContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(240, 88);
    return CGSizeMake(320, 104);
}

static CGSize ISHWorkspaceMinimumTerminalContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(300, 220);
    return CGSizeMake(520, 340);
}

static CGSize ISHWorkspaceMinimumToolContentSize(NSString *toolIdentifier) {
    if (ISHWorkspaceUsesPhoneLayout()) {
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
            return CGSizeMake(132, 68);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
            return CGSizeMake(232, 132);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
            return CGSizeMake(280, 144);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
            return CGSizeMake(280, 150);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
            return CGSizeMake(300, 220);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
            return CGSizeMake(220, 140);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolProcessesIdentifier])
            return CGSizeMake(272, 170);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
            return CGSizeMake(280, 176);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
            return CGSizeMake(288, 188);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
            return CGSizeMake(260, 148);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
            return CGSizeMake(320, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
            return CGSizeMake(320, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
            return CGSizeMake(320, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
            return CGSizeMake(320, 420);
        return CGSizeMake(300, 220);
    }

    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return CGSizeMake(150, 72);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return CGSizeMake(260, 144);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return CGSizeMake(300, 150);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return CGSizeMake(300, 156);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return CGSizeMake(250, 154);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolProcessesIdentifier])
        return CGSizeMake(340, 200);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return CGSizeMake(340, 208);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return CGSizeMake(360, 220);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return CGSizeMake(300, 164);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return CGSizeMake(520, 560);
    return CGSizeZero;
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceSizeDescriptor(CGSize size) {
    return @{
        @"width": @(MAX(0, size.width)),
        @"height": @(MAX(0, size.height)),
    };
}

static CGSize ISHWorkspaceSizeFromDescriptor(NSDictionary<NSString *, id> *descriptor) {
    if (![descriptor isKindOfClass:NSDictionary.class])
        return CGSizeZero;
    return CGSizeMake([descriptor[@"width"] doubleValue], [descriptor[@"height"] doubleValue]);
}

static NSString *ISHWorkspaceToolTitle(NSString *toolIdentifier) {
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return @"Clock";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return @"Info";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return @"Monitor";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return @"Networks";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return @"System Status";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return @"Workspaces";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolProcessesIdentifier])
        return @"Processes";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return @"Sessions";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return @"Storage";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return @"Shortcuts";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return @"Themes";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return @"Diagnostics";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return @"Filesystems";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
        return @"Settings";
    return @"Window";
}

BOOL ISHShouldLaunchWorkspaceAtStartup(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return [initialWindow isEqualToString:ISHInitialWindowWorkspaceValue];
}

static NSString *ISHInitialWindowTitle(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([initialWindow isEqualToString:ISHInitialWindowWorkspaceValue])
        return @"Workspace";
    if ([initialWindow isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return @"Choose Filesystem";
    if ([initialWindow isEqualToString:@"session-shell"])
        return @"Session Shell (pts/1)";
    return @"Plain Terminal";
}

static NSString *ISHWorkspaceTerminalDisplayName(Terminal *terminal) {
    if (terminal == nil)
        return @"Unknown Terminal";
    int consoleMajor = TTY_CONSOLE_MAJOR;
    int consoleMinor = 1;
    get_console_device(&consoleMajor, &consoleMinor);
    if (terminal.type == consoleMajor && terminal.number == consoleMinor) {
        if (consoleMajor == TTY_CONSOLE_MAJOR)
            return [NSString stringWithFormat:@"System Console (tty%d)", consoleMinor];
        if (consoleMajor == TTY_PSEUDO_SLAVE_MAJOR)
            return [NSString stringWithFormat:@"System Console (pts/%d)", consoleMinor];
        return [NSString stringWithFormat:@"System Console (%d:%d)", consoleMajor, consoleMinor];
    }
    if (terminal.type == TTY_CONSOLE_MAJOR)
        return [NSString stringWithFormat:@"Terminal (tty%d)", terminal.number];
    if (terminal.type == TTY_PSEUDO_SLAVE_MAJOR)
        return [NSString stringWithFormat:@"Pseudo Terminal (pts/%d)", terminal.number];
    return [NSString stringWithFormat:@"Terminal (%d:%d)", terminal.type, terminal.number];
}

static NSString *ISHWorkspaceTerminalRoleForTerminal(Terminal *terminal) {
    if (terminal == nil)
        return ISHWorkspaceTerminalRoleGeneric;
    int consoleMajor = TTY_CONSOLE_MAJOR;
    int consoleMinor = 1;
    get_console_device(&consoleMajor, &consoleMinor);
    if ((terminal.type == consoleMajor && terminal.number == consoleMinor) ||
        terminal.type == TTY_CONSOLE_MAJOR) {
        return ISHWorkspaceTerminalRoleSystemConsole;
    }
    if (terminal.type == TTY_PSEUDO_SLAVE_MAJOR)
        return ISHWorkspaceTerminalRoleSessionShell;
    return ISHWorkspaceTerminalRoleGeneric;
}

static NSString *ISHWorkspaceDiagnosticsString(id value) {
    if (value == nil || value == (id)kCFNull)
        return nil;
    if ([value isKindOfClass:NSString.class])
        return (NSString *)value;
    if ([value respondsToSelector:@selector(stringValue)])
        return [value stringValue];
    return [value description];
}

static NSString *ISHWorkspaceTitleForTerminalRole(NSString *terminalRole, Terminal *terminal) {
    if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSystemConsole])
        return @"System Console";
    if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSessionShell])
        return @"Session Shell";
    if (terminal != nil)
        return ISHWorkspaceTerminalDisplayName(terminal);
    return @"Terminal";
}

static NSString *ISHWorkspaceSceneRoleDescription(UISceneSession *session) API_AVAILABLE(ios(13.0));
static NSString *ISHWorkspaceSceneRoleDescription(UISceneSession *session) {
    NSString *activityType = session.stateRestorationActivity.activityType;
    if ([activityType isEqualToString:ISHSceneActivityTypeWorkspace])
        return @"Workspace";
    if ([activityType isEqualToString:ISHSceneActivityTypeTerminal] ||
        [activityType isEqualToString:@"app.ish.scene"]) {
        return @"Terminal";
    }
    return @"Unknown";
}

static NSString *ISHWorkspaceSceneActivationDescription(UIScene *scene) API_AVAILABLE(ios(13.0));
static NSString *ISHWorkspaceSceneActivationDescription(UIScene *scene) {
    switch (scene.activationState) {
        case UISceneActivationStateForegroundActive:
            return @"Foreground active";
        case UISceneActivationStateForegroundInactive:
            return @"Foreground inactive";
        case UISceneActivationStateBackground:
            return @"Background";
        case UISceneActivationStateUnattached:
            return @"Unattached";
    }
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceSceneDescriptors(UIWindowScene *currentWindowScene) API_AVAILABLE(ios(13.0));
static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceSceneDescriptors(UIWindowScene *currentWindowScene) {
    NSArray<UIScene *> *connectedScenes =
        [UIApplication.sharedApplication.connectedScenes.allObjects sortedArrayUsingComparator:^NSComparisonResult(UIScene *left, UIScene *right) {
        if (left == currentWindowScene)
            return NSOrderedAscending;
        if (right == currentWindowScene)
            return NSOrderedDescending;
        return [left.session.persistentIdentifier compare:right.session.persistentIdentifier];
    }];

    NSMutableArray<NSDictionary<NSString *, id> *> *descriptors = [NSMutableArray array];
    for (UIScene *scene in connectedScenes) {
        NSString *role = ISHWorkspaceSceneRoleDescription(scene.session);
        NSString *state = ISHWorkspaceSceneActivationDescription(scene);
        NSString *identifier = scene.session.persistentIdentifier ?: @"";
        NSString *shortIdentifier = identifier;
        if (shortIdentifier.length > 8)
            shortIdentifier = [shortIdentifier substringFromIndex:shortIdentifier.length - 8];
        NSString *terminalUUID = scene.session.stateRestorationActivity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
        Terminal *terminal = terminalUUID.length > 0
            ? [Terminal terminalWithUUID:[[NSUUID alloc] initWithUUIDString:terminalUUID]]
            : nil;
        NSString *title = scene == currentWindowScene
            ? @"This Window"
            : [NSString stringWithFormat:@"%@ %@", role, shortIdentifier];
        NSString *detail = terminal != nil ? ISHWorkspaceTerminalDisplayName(terminal) : state;
        [descriptors addObject:@{
            @"identifier": identifier,
            @"title": title,
            @"detail": detail ?: @"Unknown",
            @"role": role ?: @"Unknown",
            @"state": state ?: @"Unknown",
            @"isCurrent": @(scene == currentWindowScene),
        }];
    }
    return descriptors;
}

static NSString *ISHWorkspaceNetworkSummaryText(void) {
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0 || interfaces == NULL)
        return @"Network: unavailable";

    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSMutableArray<NSString *> *interfaceOrder = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, NSString *> *> *interfaceAddresses = [NSMutableDictionary dictionary];
    NSString *loopbackLine = nil;
    char addressBuffer[INET6_ADDRSTRLEN] = {0};

    for (struct ifaddrs *cursor = interfaces; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_addr == NULL || cursor->ifa_name == NULL)
            continue;
        sa_family_t family = cursor->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6)
            continue;
        if ((cursor->ifa_flags & IFF_UP) == 0)
            continue;

        const void *source = family == AF_INET
            ? (const void *) &((const struct sockaddr_in *) cursor->ifa_addr)->sin_addr
            : (const void *) &((const struct sockaddr_in6 *) cursor->ifa_addr)->sin6_addr;
        if (inet_ntop(family, source, addressBuffer, sizeof(addressBuffer)) == NULL)
            continue;

        NSString *interfaceName = [NSString stringWithUTF8String:cursor->ifa_name];
        NSString *address = [NSString stringWithUTF8String:addressBuffer];
        BOOL isLoopback = (cursor->ifa_flags & IFF_LOOPBACK) != 0;
        if (isLoopback) {
            if (loopbackLine == nil)
                loopbackLine = [NSString stringWithFormat:@"Loopback: %@ (%@)", interfaceName, address];
            continue;
        }

        NSMutableDictionary<NSString *, NSString *> *addresses = interfaceAddresses[interfaceName];
        if (addresses == nil) {
            addresses = [NSMutableDictionary dictionary];
            interfaceAddresses[interfaceName] = addresses;
            [interfaceOrder addObject:interfaceName];
        }
        NSString *familyKey = family == AF_INET ? @"ipv4" : @"ipv6";
        if (addresses[familyKey] == nil)
            addresses[familyKey] = address;
    }

    freeifaddrs(interfaces);

    NSUInteger activeInterfaces = 0;
    for (NSString *interfaceName in interfaceOrder) {
        NSDictionary<NSString *, NSString *> *addresses = interfaceAddresses[interfaceName];
        NSString *ipv4 = addresses[@"ipv4"];
        NSString *ipv6 = addresses[@"ipv6"];
        NSString *address = ipv4 ?: ipv6;
        if (address.length == 0)
            continue;
        NSString *familyName = ipv4.length > 0 ? @"IPv4" : @"IPv6";
        [lines addObject:[NSString stringWithFormat:@"%@: %@  %@", interfaceName, familyName, address]];
        activeInterfaces += 1;
        if (activeInterfaces >= 3)
            break;
    }

    if (loopbackLine != nil)
        [lines addObject:loopbackLine];

    if (activeInterfaces == 0 && loopbackLine == nil)
        return @"Network: no active interfaces";

    return [NSString stringWithFormat:@"Active interfaces: %lu\n%@",
                                      (unsigned long) activeInterfaces,
                                      [lines componentsJoinedByString:@"\n"]];
}

static NSString *ISHWorkspaceBatterySummaryText(void) {
    if (UIDevice.currentDevice.batteryState == UIDeviceBatteryStateUnknown || UIDevice.currentDevice.batteryLevel < 0)
        return @"unavailable";

    NSString *stateDescription = @"On battery";
    switch (UIDevice.currentDevice.batteryState) {
        case UIDeviceBatteryStateCharging:
            stateDescription = @"Charging";
            break;
        case UIDeviceBatteryStateFull:
            stateDescription = @"Fully charged";
            break;
        case UIDeviceBatteryStateUnplugged:
            stateDescription = @"On battery";
            break;
        case UIDeviceBatteryStateUnknown:
            break;
    }
    NSInteger percent = (NSInteger) llround(UIDevice.currentDevice.batteryLevel * 100.0);
    return [NSString stringWithFormat:@"%@ (%ld%%)", stateDescription, (long) percent];
}

static NSString *ISHWorkspaceStorageSummaryText(void) {
    NSDictionary<NSFileAttributeKey, id> *attributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = attributes[NSFileSystemFreeSize];
    if (freeSize == nil)
        return @"Free storage: unavailable";
    NSString *formattedSize = [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue
                                                              countStyle:NSByteCountFormatterCountStyleFile];
    return [NSString stringWithFormat:@"Free storage: %@", formattedSize];
}

static NSString *ISHWorkspacePrimaryNetworkLine(void) {
    NSArray<NSString *> *lines = [ISHWorkspaceNetworkSummaryText() componentsSeparatedByString:@"\n"];
    if (lines.count >= 2)
        return lines[1];
    return lines.firstObject ?: @"Network: unavailable";
}

static NSString *ISHWorkspaceByteCountString(uint64_t bytes) {
    return [NSByteCountFormatter stringFromByteCount:(long long) bytes
                                          countStyle:NSByteCountFormatterCountStyleFile];
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceDirectoryUsage(NSURL *directoryURL) {
    if (directoryURL == nil)
        return @{@"bytes": @0, @"files": @0, @"directories": @0};

    uint64_t totalBytes = 0;
    NSUInteger fileCount = 0;
    NSUInteger directoryCount = 0;
    NSArray<NSURLResourceKey> *keys = @[
        NSURLIsDirectoryKey,
        NSURLFileAllocatedSizeKey,
        NSURLTotalFileAllocatedSizeKey,
        NSURLFileSizeKey,
    ];
    NSDirectoryEnumerator<NSURL *> *enumerator =
        [NSFileManager.defaultManager enumeratorAtURL:directoryURL
                           includingPropertiesForKeys:keys
                                              options:0
                                         errorHandler:^BOOL(__unused NSURL *url, __unused NSError *error) {
        return YES;
    }];
    for (NSURL *itemURL in enumerator) {
        NSNumber *isDirectory = nil;
        [itemURL getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
        if (isDirectory.boolValue) {
            directoryCount += 1;
            continue;
        }
        NSNumber *size = nil;
        [itemURL getResourceValue:&size forKey:NSURLTotalFileAllocatedSizeKey error:nil];
        if (size == nil)
            [itemURL getResourceValue:&size forKey:NSURLFileAllocatedSizeKey error:nil];
        if (size == nil)
            [itemURL getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        totalBytes += size.unsignedLongLongValue;
        fileCount += 1;
    }
    return @{
        @"bytes": @(totalBytes),
        @"files": @(fileCount),
        @"directories": @(directoryCount),
    };
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceRootUsageRecords(void) {
    NSMutableArray<NSDictionary<NSString *, id> *> *records = [NSMutableArray array];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    for (NSString *rootName in Roots.instance.roots) {
        NSURL *rootURL = [Roots.instance rootUrl:rootName];
        NSDictionary<NSString *, NSNumber *> *usage = ISHWorkspaceDirectoryUsage(rootURL);
        NSString *guestABI = [Roots.instance guestABIForRootNamed:rootName] ?: @"Unknown ABI";
        [records addObject:@{
            @"name": rootName,
            @"isDefault": @([rootName isEqualToString:defaultRoot]),
            @"abi": guestABI,
            @"path": rootURL.path ?: @"",
            @"bytes": usage[@"bytes"] ?: @0,
            @"files": usage[@"files"] ?: @0,
            @"directories": usage[@"directories"] ?: @0,
        }];
    }
    return records;
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceVisibleProcessRecords(NSUInteger limit) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t bufferSize = 0;
    if (sysctl(mib, 4, NULL, &bufferSize, NULL, 0) != 0 || bufferSize == 0)
        return @[];
    struct kinfo_proc *processes = malloc(bufferSize);
    if (processes == NULL)
        return @[];
    if (sysctl(mib, 4, processes, &bufferSize, NULL, 0) != 0) {
        free(processes);
        return @[];
    }

    NSMutableArray<NSDictionary<NSString *, id> *> *records = [NSMutableArray array];
    NSUInteger processCount = bufferSize / sizeof(struct kinfo_proc);
    pid_t currentPID = NSProcessInfo.processInfo.processIdentifier;
    for (NSUInteger index = 0; index < processCount; index++) {
        struct kinfo_proc process = processes[index];
        pid_t pid = process.kp_proc.p_pid;
        if (pid <= 0)
            continue;
        NSString *name = [NSString stringWithUTF8String:process.kp_proc.p_comm];
        if (name.length == 0)
            continue;
        NSString *state = @"other";
        switch (process.kp_proc.p_stat) {
            case SRUN:
                state = @"run";
                break;
            case SSLEEP:
                state = @"sleep";
                break;
            case SSTOP:
                state = @"stop";
                break;
            case SZOMB:
                state = @"zombie";
                break;
            default:
                break;
        }
        [records addObject:@{
            @"pid": @(pid),
            @"name": name,
            @"state": state,
            @"isCurrent": @(pid == currentPID),
        }];
    }
    free(processes);

    [records sortUsingComparator:^NSComparisonResult(NSDictionary<NSString *, id> *left,
                                                     NSDictionary<NSString *, id> *right) {
        BOOL leftCurrent = [left[@"isCurrent"] boolValue];
        BOOL rightCurrent = [right[@"isCurrent"] boolValue];
        if (leftCurrent && !rightCurrent)
            return NSOrderedAscending;
        if (!leftCurrent && rightCurrent)
            return NSOrderedDescending;
        NSString *leftName = left[@"name"] ?: @"";
        NSString *rightName = right[@"name"] ?: @"";
        NSComparisonResult nameResult = [leftName compare:rightName];
        if (nameResult != NSOrderedSame)
            return nameResult;
        if ([left[@"pid"] intValue] < [right[@"pid"] intValue])
            return NSOrderedAscending;
        if ([left[@"pid"] intValue] > [right[@"pid"] intValue])
            return NSOrderedDescending;
        return NSOrderedSame;
    }];

    if (limit > 0 && records.count > limit)
        return [records subarrayWithRange:NSMakeRange(0, limit)];
    return records;
}

static NSString *ISHWorkspaceUsageBarString(double ratio, NSUInteger width) {
    double clampedRatio = MAX(0.0, MIN(1.0, ratio));
    NSUInteger filled = (NSUInteger) llround(clampedRatio * (double) width);
    filled = MIN(width, filled);
    return [NSString stringWithFormat:@"[%@%@]",
                                      [@"" stringByPaddingToLength:filled withString:@"#" startingAtIndex:0],
                                      [@"" stringByPaddingToLength:(width - filled) withString:@"-" startingAtIndex:0]];
}

static NSString *ISHWorkspaceDurationString(NSTimeInterval interval) {
    NSInteger totalSeconds = MAX(0, (NSInteger) llround(interval));
    NSInteger days = totalSeconds / 86400;
    NSInteger hours = (totalSeconds % 86400) / 3600;
    NSInteger minutes = (totalSeconds % 3600) / 60;
    NSInteger seconds = totalSeconds % 60;
    if (days > 0)
        return [NSString stringWithFormat:@"%ldd %02ldh %02ldm", (long) days, (long) hours, (long) minutes];
    if (hours > 0)
        return [NSString stringWithFormat:@"%ldh %02ldm %02lds", (long) hours, (long) minutes, (long) seconds];
    return [NSString stringWithFormat:@"%ldm %02lds", (long) minutes, (long) seconds];
}

static BOOL ISHWorkspaceMemoryUsage(uint64_t *footprint, uint64_t *resident, uint64_t *physical) {
    if (footprint != NULL)
        *footprint = 0;
    if (resident != NULL)
        *resident = 0;
    if (physical != NULL)
        *physical = NSProcessInfo.processInfo.physicalMemory;

    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t vmInfoCount = TASK_VM_INFO_COUNT;
    kern_return_t result = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &vmInfo, &vmInfoCount);
    if (result == KERN_SUCCESS) {
        if (footprint != NULL)
            *footprint = vmInfo.phys_footprint;
        if (resident != NULL)
            *resident = vmInfo.resident_size;
        return YES;
    }

    task_basic_info_data_t basicInfo;
    mach_msg_type_number_t basicInfoCount = TASK_BASIC_INFO_COUNT;
    result = task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &basicInfo, &basicInfoCount);
    if (result == KERN_SUCCESS) {
        if (footprint != NULL)
            *footprint = basicInfo.resident_size;
        if (resident != NULL)
            *resident = basicInfo.resident_size;
        return YES;
    }
    return NO;
}

static NSString *ISHWorkspaceSystemStatusText(void) {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    NSString *build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    [lines addObject:[NSString stringWithFormat:@"App: %@ (%@)", version, build]];
    [lines addObject:[NSString stringWithFormat:@"Device: %@ / iOS %@",
                      UIDevice.currentDevice.model ?: @"Unknown",
                      UIDevice.currentDevice.systemVersion ?: @"?"]];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    [lines addObject:[NSString stringWithFormat:@"Current root: %@",
                      defaultRoot.length > 0 ? defaultRoot : @"unavailable"]];
    [lines addObject:ISHWorkspaceStorageSummaryText()];
    [lines addObject:[NSString stringWithFormat:@"Startup screen: %@", ISHInitialWindowTitle()]];
    [lines addObject:[NSString stringWithFormat:@"Installed roots: %lu",
                      (unsigned long) Roots.instance.roots.count]];
    [lines addObject:[NSString stringWithFormat:@"Active terminals: %lu",
                      (unsigned long) Terminal.activeTerminals.count]];
    if (@available(iOS 13.0, *)) {
        [lines addObject:[NSString stringWithFormat:@"Open scenes: %lu",
                          (unsigned long) UIApplication.sharedApplication.connectedScenes.count]];
    }
    [lines addObject:@""];
    [lines addObject:ISHWorkspaceNetworkSummaryText()];

    NSArray<NSDictionary<NSString *, id> *> *breadcrumbs = [ISHDiagnosticsStore recentBreadcrumbsWithLimit:5];
    if (breadcrumbs.count > 0) {
        [lines addObject:@""];
        [lines addObject:@"Recent events:"];
        for (NSDictionary<NSString *, id> *entry in breadcrumbs) {
            NSString *event = entry[@"event"] ?: @"event";
            NSString *timestamp = entry[@"timestamp"] ?: @"";
            [lines addObject:[NSString stringWithFormat:@"%@  %@", timestamp, event]];
        }
    }
    return [lines componentsJoinedByString:@"\n"];
}

static NSString *const ISHWorkspaceToolThemePreferenceKey = @"ISHWorkspaceToolTheme";
static NSString *const ISHWorkspaceCustomThemesDefaultsKey = @"ISHWorkspaceCustomThemes";
static NSString *const ISHWorkspaceToolDensityPreferenceKey = @"ISHWorkspaceToolDensity";
static NSString *const ISHWorkspaceToolThemeDidChangeNotification = @"ISHWorkspaceToolThemeDidChange";
static NSString *const ISHWorkspaceToolThemeAuroraIdentifier = @"aurora";
static NSString *const ISHWorkspaceToolThemeSolsticeIdentifier = @"solstice";
static NSString *const ISHWorkspaceToolThemeGraphiteIdentifier = @"graphite";

static UIColor *ISHWorkspaceThemeColor(CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha) {
    return [UIColor colorWithRed:red / 255.0
                           green:green / 255.0
                            blue:blue / 255.0
                           alpha:alpha];
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceThemeColorDescriptor(CGFloat red, CGFloat green, CGFloat blue) {
    return @{
        @"red": @(red),
        @"green": @(green),
        @"blue": @(blue),
    };
}

static UIColor *ISHWorkspaceThemeColorFromDescriptor(NSDictionary<NSString *, NSNumber *> *descriptor) {
    if (![descriptor isKindOfClass:NSDictionary.class])
        return UIColor.blackColor;
    return ISHWorkspaceThemeColor([descriptor[@"red"] doubleValue],
                                  [descriptor[@"green"] doubleValue],
                                  [descriptor[@"blue"] doubleValue],
                                  1.0);
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceThemeColorDescriptorFromUIColor(UIColor *color) {
    CGFloat red = 0;
    CGFloat green = 0;
    CGFloat blue = 0;
    CGFloat alpha = 0;
    if (![color getRed:&red green:&green blue:&blue alpha:&alpha]) {
        CGFloat white = 0;
        if ([color getWhite:&white alpha:&alpha]) {
            red = white;
            green = white;
            blue = white;
        }
    }
    return @{
        @"red": @(llround(red * 255.0)),
        @"green": @(llround(green * 255.0)),
        @"blue": @(llround(blue * 255.0)),
    };
}

static NSArray<NSString *> *ISHWorkspaceThemeEditableColorKeys(void) {
    return @[@"backgroundTop", @"backgroundBottom", @"card", @"primary", @"secondary", @"accent", @"accentAlt"];
}

static NSArray<NSDictionary<NSString *, NSString *> *> *ISHWorkspaceBuiltInThemeChoices(void) {
    return @[
        @{@"identifier": ISHWorkspaceToolThemeAuroraIdentifier, @"title": @"Aurora"},
        @{@"identifier": ISHWorkspaceToolThemeSolsticeIdentifier, @"title": @"Solstice"},
        @{@"identifier": ISHWorkspaceToolThemeGraphiteIdentifier, @"title": @"Graphite"},
    ];
}

static NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *ISHWorkspaceBuiltInThemePalette(NSString *identifier) {
    if ([identifier isEqualToString:ISHWorkspaceToolThemeSolsticeIdentifier]) {
        return @{
            @"backgroundTop": ISHWorkspaceThemeColorDescriptor(255, 240, 218),
            @"backgroundBottom": ISHWorkspaceThemeColorDescriptor(255, 174, 111),
            @"card": ISHWorkspaceThemeColorDescriptor(255, 250, 242),
            @"primary": ISHWorkspaceThemeColorDescriptor(71, 39, 24),
            @"secondary": ISHWorkspaceThemeColorDescriptor(122, 85, 63),
            @"accent": ISHWorkspaceThemeColorDescriptor(214, 97, 42),
            @"accentAlt": ISHWorkspaceThemeColorDescriptor(196, 133, 18),
        };
    }
    if ([identifier isEqualToString:ISHWorkspaceToolThemeGraphiteIdentifier]) {
        return @{
            @"backgroundTop": ISHWorkspaceThemeColorDescriptor(21, 29, 43),
            @"backgroundBottom": ISHWorkspaceThemeColorDescriptor(49, 63, 85),
            @"card": ISHWorkspaceThemeColorDescriptor(33, 42, 58),
            @"primary": ISHWorkspaceThemeColorDescriptor(239, 244, 255),
            @"secondary": ISHWorkspaceThemeColorDescriptor(177, 189, 214),
            @"accent": ISHWorkspaceThemeColorDescriptor(107, 226, 198),
            @"accentAlt": ISHWorkspaceThemeColorDescriptor(125, 164, 255),
        };
    }
    return @{
        @"backgroundTop": ISHWorkspaceThemeColorDescriptor(13, 34, 70),
        @"backgroundBottom": ISHWorkspaceThemeColorDescriptor(44, 129, 167),
        @"card": ISHWorkspaceThemeColorDescriptor(239, 251, 255),
        @"primary": ISHWorkspaceThemeColorDescriptor(14, 39, 63),
        @"secondary": ISHWorkspaceThemeColorDescriptor(69, 99, 128),
        @"accent": ISHWorkspaceThemeColorDescriptor(0, 156, 183),
        @"accentAlt": ISHWorkspaceThemeColorDescriptor(109, 204, 128),
    };
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceCustomThemeRecords(void) {
    NSArray *records = [NSUserDefaults.standardUserDefaults arrayForKey:ISHWorkspaceCustomThemesDefaultsKey];
    return [records isKindOfClass:NSArray.class] ? records : @[];
}

static NSDictionary<NSString *, id> *ISHWorkspaceThemeRecordForIdentifier(NSString *identifier) {
    for (NSDictionary<NSString *, NSString *> *choice in ISHWorkspaceBuiltInThemeChoices()) {
        if ([choice[@"identifier"] isEqualToString:identifier])
            return @{
                @"identifier": choice[@"identifier"],
                @"title": choice[@"title"],
                @"palette": ISHWorkspaceBuiltInThemePalette(identifier),
                @"builtIn": @YES,
            };
    }
    for (NSDictionary<NSString *, id> *record in ISHWorkspaceCustomThemeRecords()) {
        if ([record[@"identifier"] isEqualToString:identifier])
            return record;
    }
    return nil;
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceThemeChoices(void) {
    NSMutableArray<NSDictionary<NSString *, id> *> *choices = [NSMutableArray array];
    for (NSDictionary<NSString *, NSString *> *choice in ISHWorkspaceBuiltInThemeChoices()) {
        [choices addObject:@{
            @"identifier": choice[@"identifier"],
            @"title": choice[@"title"],
            @"palette": ISHWorkspaceBuiltInThemePalette(choice[@"identifier"]),
            @"builtIn": @YES,
        }];
    }
    [choices addObjectsFromArray:ISHWorkspaceCustomThemeRecords()];
    return choices;
}

static BOOL ISHWorkspaceThemeIdentifierIsValid(NSString *identifier) {
    return ISHWorkspaceThemeRecordForIdentifier(identifier) != nil;
}

static NSString *ISHWorkspaceCurrentThemeIdentifier(void) {
    NSString *identifier = [NSUserDefaults.standardUserDefaults stringForKey:ISHWorkspaceToolThemePreferenceKey];
    if (!ISHWorkspaceThemeIdentifierIsValid(identifier))
        return ISHWorkspaceToolThemeAuroraIdentifier;
    return identifier;
}

static NSString *ISHWorkspaceCurrentThemeTitle(void) {
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(ISHWorkspaceCurrentThemeIdentifier());
    return record[@"title"] ?: @"Aurora";
}

static NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *ISHWorkspaceThemeEditablePaletteForIdentifier(NSString *identifier) {
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(identifier);
    NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette = record[@"palette"];
    if (![palette isKindOfClass:NSDictionary.class])
        return ISHWorkspaceBuiltInThemePalette(ISHWorkspaceToolThemeAuroraIdentifier);
    return palette;
}

static NSDictionary<NSString *, UIColor *> *ISHWorkspaceThemeDescriptorFromEditablePalette(NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette) {
    UIColor *backgroundTop = ISHWorkspaceThemeColorFromDescriptor(palette[@"backgroundTop"]);
    UIColor *backgroundBottom = ISHWorkspaceThemeColorFromDescriptor(palette[@"backgroundBottom"]);
    UIColor *cardBase = ISHWorkspaceThemeColorFromDescriptor(palette[@"card"]);
    UIColor *primary = ISHWorkspaceThemeColorFromDescriptor(palette[@"primary"]);
    UIColor *secondary = ISHWorkspaceThemeColorFromDescriptor(palette[@"secondary"]);
    UIColor *accent = ISHWorkspaceThemeColorFromDescriptor(palette[@"accent"]);
    UIColor *accentAlt = ISHWorkspaceThemeColorFromDescriptor(palette[@"accentAlt"]);
    return @{
        @"backgroundTop": backgroundTop,
        @"backgroundBottom": backgroundBottom,
        @"card": [cardBase colorWithAlphaComponent:0.9],
        @"cardAlt": [cardBase colorWithAlphaComponent:0.76],
        @"primary": primary,
        @"secondary": secondary,
        @"accent": accent,
        @"accentAlt": accentAlt,
        @"stroke": [accent colorWithAlphaComponent:0.18],
    };
}

static NSDictionary<NSString *, UIColor *> *ISHWorkspaceThemeDescriptor(void) {
    return ISHWorkspaceThemeDescriptorFromEditablePalette(
        ISHWorkspaceThemeEditablePaletteForIdentifier(ISHWorkspaceCurrentThemeIdentifier()));
}

static CGFloat ISHWorkspaceCurrentDensity(void) {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:ISHWorkspaceToolDensityPreferenceKey];
    if (![value isKindOfClass:NSNumber.class])
        return 0.0;
    return MAX(0.0, MIN(1.0, [value doubleValue]));
}

static CGFloat ISHWorkspaceDensityValue(CGFloat compact, CGFloat roomy) {
    return compact + ((roomy - compact) * ISHWorkspaceCurrentDensity());
}

static CGFloat ISHWorkspaceThemeFontSize(UIFontTextStyle textStyle) {
    CGFloat size = 0;
    if ([textStyle isEqualToString:UIFontTextStyleCaption2])
        size = ISHWorkspaceDensityValue(8, 11);
    else if ([textStyle isEqualToString:UIFontTextStyleCaption1])
        size = ISHWorkspaceDensityValue(9, 12);
    else if ([textStyle isEqualToString:UIFontTextStyleFootnote])
        size = ISHWorkspaceDensityValue(10, 13);
    else if ([textStyle isEqualToString:UIFontTextStyleSubheadline])
        size = ISHWorkspaceDensityValue(11, 15);
    else if ([textStyle isEqualToString:UIFontTextStyleHeadline])
        size = ISHWorkspaceDensityValue(12, 17);
    else if ([textStyle isEqualToString:UIFontTextStyleBody])
        size = ISHWorkspaceDensityValue(12, 16);
    else if ([textStyle isEqualToString:UIFontTextStyleTitle3])
        size = ISHWorkspaceDensityValue(15, 21);
    else if ([textStyle isEqualToString:UIFontTextStyleTitle2])
        size = ISHWorkspaceDensityValue(17, 25);
    else if ([textStyle isEqualToString:UIFontTextStyleLargeTitle])
        size = ISHWorkspaceDensityValue(24, 34);
    else
        size = ISHWorkspaceDensityValue(12, 16);

    if (ISHWorkspaceUsesPhoneLayout())
        size *= 0.92;
    return size;
}

static void ISHWorkspaceSetCurrentDensity(CGFloat density) {
    CGFloat clamped = MAX(0.0, MIN(1.0, density));
    [NSUserDefaults.standardUserDefaults setDouble:clamped forKey:ISHWorkspaceToolDensityPreferenceKey];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static NSString *ISHWorkspaceCurrentDensityTitle(void) {
    CGFloat density = ISHWorkspaceCurrentDensity();
    if (density < 0.18)
        return @"Ultra Compact";
    if (density < 0.42)
        return @"Compact";
    if (density < 0.72)
        return @"Balanced";
    return @"Comfortable";
}

static void ISHWorkspaceThemeDrawLinearGradient(CGContextRef context,
                                                CGRect rect,
                                                UIColor *startColor,
                                                UIColor *endColor) {
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    NSArray *colors = @[(id) startColor.CGColor, (id) endColor.CGColor];
    CGGradientRef gradient = CGGradientCreateWithColors(colorSpace, (__bridge CFArrayRef) colors, NULL);
    CGPoint startPoint = CGPointMake(CGRectGetMidX(rect), CGRectGetMinY(rect));
    CGPoint endPoint = CGPointMake(CGRectGetMidX(rect), CGRectGetMaxY(rect));
    CGContextDrawLinearGradient(context, gradient, startPoint, endPoint, 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(colorSpace);
}

static UIImage *ISHWorkspaceThemeArtworkImage(NSString *identifier,
                                              NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette,
                                              CGSize size,
                                              BOOL wallpaper) {
    if (size.width < 1 || size.height < 1)
        return nil;

    NSDictionary<NSString *, UIColor *> *theme = ISHWorkspaceThemeDescriptorFromEditablePalette(palette);
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat defaultFormat];
    format.opaque = YES;
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size format:format];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext *rendererContext) {
        CGContextRef context = rendererContext.CGContext;
        CGRect rect = CGRectMake(0, 0, size.width, size.height);
        CGFloat width = CGRectGetWidth(rect);
        CGFloat height = CGRectGetHeight(rect);

        UIColor *backgroundTop = theme[@"backgroundTop"];
        UIColor *backgroundBottom = theme[@"backgroundBottom"];
        UIColor *card = [theme[@"card"] colorWithAlphaComponent:wallpaper ? 0.26 : 0.18];
        UIColor *accent = [theme[@"accent"] colorWithAlphaComponent:wallpaper ? 0.46 : 0.28];
        UIColor *accentAlt = [theme[@"accentAlt"] colorWithAlphaComponent:wallpaper ? 0.34 : 0.24];
        UIColor *secondary = [theme[@"secondary"] colorWithAlphaComponent:wallpaper ? 0.22 : 0.18];

        ISHWorkspaceThemeDrawLinearGradient(context, rect, backgroundTop, backgroundBottom);

        UIBezierPath *glowOrb = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(width * 0.58, -height * 0.1,
                                                                                   width * 0.44, height * 0.46)];
        [accent setFill];
        [glowOrb fill];

        UIBezierPath *secondaryOrb = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(-width * 0.16, height * 0.52,
                                                                                        width * 0.46, height * 0.38)];
        [accentAlt setFill];
        [secondaryOrb fill];

        if ([identifier isEqualToString:ISHWorkspaceToolThemeSolsticeIdentifier]) {
            UIBezierPath *sun = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(width * 0.62, height * 0.12,
                                                                                  width * 0.18, width * 0.18)];
            [[theme[@"card"] colorWithAlphaComponent:wallpaper ? 0.82 : 0.9] setFill];
            [sun fill];

            NSArray<NSNumber *> *bandOrigins = @[@0.58, @0.68, @0.79];
            NSArray<UIColor *> *bandColors = @[
                [theme[@"accent"] colorWithAlphaComponent:0.34],
                [theme[@"accentAlt"] colorWithAlphaComponent:0.28],
                [theme[@"card"] colorWithAlphaComponent:0.22],
            ];
            for (NSUInteger index = 0; index < bandOrigins.count; index++) {
                CGFloat origin = bandOrigins[index].doubleValue * height;
                UIBezierPath *band = [UIBezierPath bezierPath];
                [band moveToPoint:CGPointMake(-width * 0.1, origin)];
                [band addCurveToPoint:CGPointMake(width * 1.1, origin - height * 0.04)
                        controlPoint1:CGPointMake(width * 0.26, origin - height * 0.08)
                        controlPoint2:CGPointMake(width * 0.72, origin + height * 0.02)];
                [band addLineToPoint:CGPointMake(width * 1.1, height * 1.1)];
                [band addLineToPoint:CGPointMake(-width * 0.1, height * 1.1)];
                [band closePath];
                [bandColors[index] setFill];
                [band fill];
            }
        } else if ([identifier isEqualToString:ISHWorkspaceToolThemeGraphiteIdentifier]) {
            CGContextSaveGState(context);
            CGContextSetLineWidth(context, MAX(1.0, MIN(width, height) * 0.006));
            CGContextSetStrokeColorWithColor(context, [secondary colorWithAlphaComponent:0.55].CGColor);
            CGFloat step = MAX(26.0, MIN(width, height) * 0.12);
            for (CGFloat x = -height; x <= width + height; x += step) {
                CGContextMoveToPoint(context, x, 0);
                CGContextAddLineToPoint(context, x + height * 0.4, height);
            }
            CGContextStrokePath(context);
            CGContextRestoreGState(context);

            UIBezierPath *panel = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(width * 0.1, height * 0.16,
                                                                                     width * 0.48, height * 0.26)
                                                             cornerRadius:MIN(width, height) * 0.06];
            [[theme[@"card"] colorWithAlphaComponent:wallpaper ? 0.18 : 0.28] setFill];
            [panel fill];
        } else {
            UIBezierPath *ribbonA = [UIBezierPath bezierPath];
            [ribbonA moveToPoint:CGPointMake(-width * 0.1, height * 0.28)];
            [ribbonA addCurveToPoint:CGPointMake(width * 1.05, height * 0.12)
                       controlPoint1:CGPointMake(width * 0.22, height * 0.54)
                       controlPoint2:CGPointMake(width * 0.7, -height * 0.02)];
            [ribbonA addLineToPoint:CGPointMake(width * 1.05, height * 0.3)];
            [ribbonA addCurveToPoint:CGPointMake(-width * 0.1, height * 0.46)
                       controlPoint1:CGPointMake(width * 0.74, height * 0.5)
                       controlPoint2:CGPointMake(width * 0.2, height * 0.18)];
            [ribbonA closePath];
            [accent setFill];
            [ribbonA fill];

            UIBezierPath *ribbonB = [UIBezierPath bezierPath];
            [ribbonB moveToPoint:CGPointMake(-width * 0.1, height * 0.5)];
            [ribbonB addCurveToPoint:CGPointMake(width * 1.05, height * 0.38)
                       controlPoint1:CGPointMake(width * 0.28, height * 0.68)
                       controlPoint2:CGPointMake(width * 0.76, height * 0.18)];
            [ribbonB addLineToPoint:CGPointMake(width * 1.05, height * 0.55)];
            [ribbonB addCurveToPoint:CGPointMake(-width * 0.1, height * 0.66)
                       controlPoint1:CGPointMake(width * 0.76, height * 0.72)
                       controlPoint2:CGPointMake(width * 0.28, height * 0.42)];
            [ribbonB closePath];
            [accentAlt setFill];
            [ribbonB fill];
        }

        NSArray<NSValue *> *cards = @[
            [NSValue valueWithCGRect:CGRectMake(width * 0.08, height * 0.12, width * 0.18, height * 0.11)],
            [NSValue valueWithCGRect:CGRectMake(width * 0.32, height * 0.22, width * 0.22, height * 0.12)],
            [NSValue valueWithCGRect:CGRectMake(width * 0.62, height * 0.62, width * 0.22, height * 0.12)],
        ];
        for (NSValue *value in cards) {
            UIBezierPath *cardPath = [UIBezierPath bezierPathWithRoundedRect:value.CGRectValue
                                                                cornerRadius:MIN(width, height) * 0.05];
            [card setFill];
            [cardPath fill];
        }
    }];
}

static void ISHWorkspaceSetCurrentThemeIdentifier(NSString *identifier) {
    if (!ISHWorkspaceThemeIdentifierIsValid(identifier))
        return;
    NSString *currentIdentifier = ISHWorkspaceCurrentThemeIdentifier();
    if ([currentIdentifier isEqualToString:identifier])
        return;
    [NSUserDefaults.standardUserDefaults setObject:identifier forKey:ISHWorkspaceToolThemePreferenceKey];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static NSString *ISHWorkspaceCreateCustomThemeIdentifier(void) {
    return [NSString stringWithFormat:@"custom-%@", NSUUID.UUID.UUIDString.lowercaseString];
}

static void ISHWorkspaceSaveCustomThemeRecord(NSString *identifier,
                                              NSString *title,
                                              NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette) {
    if (identifier.length == 0 || title.length == 0 || ![palette isKindOfClass:NSDictionary.class])
        return;

    NSMutableArray<NSDictionary<NSString *, id> *> *records = [ISHWorkspaceCustomThemeRecords() mutableCopy];
    NSMutableDictionary<NSString *, id> *record = [@{
        @"identifier": identifier,
        @"title": title,
        @"palette": palette,
        @"builtIn": @NO,
    } mutableCopy];
    BOOL replaced = NO;
    for (NSUInteger index = 0; index < records.count; index++) {
        if ([records[index][@"identifier"] isEqualToString:identifier]) {
            records[index] = record;
            replaced = YES;
            break;
        }
    }
    if (!replaced)
        [records addObject:record];
    [NSUserDefaults.standardUserDefaults setObject:records forKey:ISHWorkspaceCustomThemesDefaultsKey];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static void ISHWorkspaceDeleteCustomThemeRecord(NSString *identifier) {
    if (identifier.length == 0)
        return;
    NSMutableArray<NSDictionary<NSString *, id> *> *records = [ISHWorkspaceCustomThemeRecords() mutableCopy];
    NSIndexSet *indexes = [records indexesOfObjectsPassingTest:^BOOL(NSDictionary<NSString *, id> *record, NSUInteger idx, BOOL *stop) {
        return [record[@"identifier"] isEqualToString:identifier];
    }];
    if (indexes.count == 0)
        return;
    [records removeObjectsAtIndexes:indexes];
    [NSUserDefaults.standardUserDefaults setObject:records forKey:ISHWorkspaceCustomThemesDefaultsKey];
    if ([[NSUserDefaults.standardUserDefaults stringForKey:ISHWorkspaceToolThemePreferenceKey] isEqualToString:identifier]) {
        [NSUserDefaults.standardUserDefaults setObject:ISHWorkspaceToolThemeAuroraIdentifier
                                                forKey:ISHWorkspaceToolThemePreferenceKey];
    }
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static BOOL ISHWorkspaceThemeIdentifierIsBuiltIn(NSString *identifier) {
    for (NSDictionary<NSString *, NSString *> *choice in ISHWorkspaceBuiltInThemeChoices()) {
        if ([choice[@"identifier"] isEqualToString:identifier])
            return YES;
    }
    return NO;
}

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

@interface WorkspaceClockToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceInfoToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceMonitorToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceNetworksToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceStatusToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceWorkspacesToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceProcessesToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceSessionsToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceStorageToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceShortcutsToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceThemesToolViewController : WorkspaceThemedToolViewController
@end

static UIViewController *ISHCreateWorkspaceToolViewController(NSString *toolIdentifier) {
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return [WorkspaceClockToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return [WorkspaceInfoToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return [WorkspaceMonitorToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return [WorkspaceNetworksToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return [WorkspaceStatusToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return [WorkspaceWorkspacesToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolProcessesIdentifier])
        return [WorkspaceProcessesToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return [WorkspaceSessionsToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return [WorkspaceStorageToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return [WorkspaceShortcutsToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return [WorkspaceThemesToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return ISHCreateRootsViewController();
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier]) {
        UINavigationController *navigationController = ISHCreateAboutNavigationController(NO, NO);
        return navigationController.topViewController;
    }
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return ISHCreateDiagnosticsViewController();
    return nil;
}

NSString *ISHWorkspaceToolIdentifierForViewController(UIViewController *viewController) {
    if ([viewController isKindOfClass:WorkspaceClockToolViewController.class])
        return ISHWorkspaceToolClockIdentifier;
    if ([viewController isKindOfClass:WorkspaceInfoToolViewController.class])
        return ISHWorkspaceToolInfoIdentifier;
    if ([viewController isKindOfClass:WorkspaceMonitorToolViewController.class])
        return ISHWorkspaceToolMonitorIdentifier;
    if ([viewController isKindOfClass:WorkspaceNetworksToolViewController.class])
        return ISHWorkspaceToolNetworksIdentifier;
    if ([viewController isKindOfClass:WorkspaceStatusToolViewController.class])
        return ISHWorkspaceToolStatusIdentifier;
    if ([viewController isKindOfClass:WorkspaceWorkspacesToolViewController.class])
        return ISHWorkspaceToolWorkspacesIdentifier;
    if ([viewController isKindOfClass:WorkspaceProcessesToolViewController.class])
        return ISHWorkspaceToolProcessesIdentifier;
    if ([viewController isKindOfClass:WorkspaceSessionsToolViewController.class])
        return ISHWorkspaceToolSessionsIdentifier;
    if ([viewController isKindOfClass:WorkspaceStorageToolViewController.class])
        return ISHWorkspaceToolStorageIdentifier;
    if ([viewController isKindOfClass:WorkspaceShortcutsToolViewController.class])
        return ISHWorkspaceToolShortcutsIdentifier;
    if ([viewController isKindOfClass:WorkspaceThemesToolViewController.class])
        return ISHWorkspaceToolThemesIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"DiagnosticsViewController")])
        return ISHWorkspaceToolDiagnosticsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"AboutViewController")])
        return ISHWorkspaceToolSettingsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"RootsTableViewController")])
        return ISHWorkspaceToolFilesystemsIdentifier;
    return nil;
}

@implementation WorkspaceViewController

- (void)applyWorkspaceWallpaperImage:(UIImage *)image {
    self.desktopWallpaperView.image = image;
    self.desktopWallpaperView.hidden = (image == nil);
    [self.desktopSurfaceView sendSubviewToBack:self.desktopWallpaperView];
    self.desktopSurfaceView.layer.contents = nil;
}

- (CGSize)workspaceWallpaperTargetImageSize {
    CGSize targetSize = self.desktopSurfaceView.bounds.size;
    if (targetSize.width <= 1 || targetSize.height <= 1)
        targetSize = self.view.bounds.size;
    if (targetSize.width <= 1 || targetSize.height <= 1)
        targetSize = UIScreen.mainScreen.bounds.size;

    CGFloat scale = MAX(UIScreen.mainScreen.scale, 2.0);
    return CGSizeMake(MAX(1.0, round(targetSize.width * scale)),
                      MAX(1.0, round(targetSize.height * scale)));
}

- (void)applyCurrentThemeWallpaperIfNeededForced:(BOOL)forced {
    CGSize imageSize = [self workspaceWallpaperTargetImageSize];
    if (imageSize.width <= 1 || imageSize.height <= 1)
        return;

    NSString *identifier = ISHWorkspaceCurrentThemeIdentifier();
    BOOL sizeMatches = CGSizeEqualToSize(self.appliedWallpaperImageSize, imageSize);
    if (!forced &&
        self.desktopWallpaperView.image != nil &&
        sizeMatches &&
        [self.appliedWallpaperThemeIdentifier isEqualToString:identifier]) {
        return;
    }

    UIImage *image = ISHWorkspaceThemeArtworkImage(identifier,
                                                   ISHWorkspaceThemeEditablePaletteForIdentifier(identifier),
                                                   imageSize,
                                                   YES);
    if (image == nil)
        return;

    [self applyWorkspaceWallpaperImage:image];
    self.appliedWallpaperThemeIdentifier = identifier;
    self.appliedWallpaperImageSize = imageSize;
}

- (UILabel *)workspaceLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    UILabel *label = [UILabel new];
    label.numberOfLines = 0;
    UIFont *preferredFont = [UIFont preferredFontForTextStyle:textStyle];
    if (@available(iOS 13.0, *)) {
        label.textColor = UIColor.labelColor;
        if (monospaced) {
            label.font = [UIFont monospacedDigitSystemFontOfSize:preferredFont.pointSize
                                                           weight:UIFontWeightSemibold];
        } else {
            label.font = preferredFont;
        }
    } else {
        label.textColor = UIColor.blackColor;
        label.font = preferredFont;
    }
    return label;
}

- (UILabel *)workspaceSectionTitle:(NSString *)title {
    UILabel *label = [self workspaceLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    label.text = title;
    return label;
}

- (UIButton *)workspaceActionButtonWithTitle:(NSString *)title selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (UIButton *)workspaceDockTileButtonWithTitle:(NSString *)title
                                      selector:(SEL)selector
                                    identifier:(NSString *)identifier {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityIdentifier = identifier;
    if (ISHWorkspaceUsesPhoneLayout()) {
        button.contentEdgeInsets = UIEdgeInsetsMake(1, 4, 1, 4);
        button.layer.cornerRadius = 8;
        [button.heightAnchor constraintGreaterThanOrEqualToConstant:18].active = YES;
    } else {
        button.contentEdgeInsets = UIEdgeInsetsMake(1, 5, 1, 5);
        button.layer.cornerRadius = 10;
        [button.heightAnchor constraintGreaterThanOrEqualToConstant:20].active = YES;
    }
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    button.contentVerticalAlignment = UIControlContentVerticalAlignmentCenter;
    button.titleLabel.numberOfLines = 2;
    button.titleLabel.textAlignment = NSTextAlignmentCenter;
    button.titleLabel.adjustsFontForContentSizeCategory = YES;
    button.layer.borderWidth = 1;
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)configureDockTileButton:(UIButton *)button
                          title:(NSString *)title
                          state:(NSString *)state
                         active:(BOOL)active
                      frontmost:(BOOL)frontmost {
    if (button == nil)
        return;

    CGFloat dockFontSize = ISHWorkspaceUsesPhoneLayout() ? 9.0 : 10.0;
    UIFont *titleFont = [UIFont systemFontOfSize:dockFontSize weight:UIFontWeightSemibold];
    UIFont *stateFont = [UIFont systemFontOfSize:dockFontSize weight:UIFontWeightMedium];
    NSMutableParagraphStyle *paragraphStyle = [NSMutableParagraphStyle new];
    paragraphStyle.alignment = NSTextAlignmentCenter;

    UIColor *fillColor = nil;
    UIColor *borderColor = nil;
    UIColor *titleColor = nil;
    UIColor *stateColor = nil;
    if (@available(iOS 13.0, *)) {
        if (frontmost) {
            fillColor = [UIColor.systemBlueColor colorWithAlphaComponent:0.18];
            borderColor = [UIColor.systemBlueColor colorWithAlphaComponent:0.45];
            titleColor = UIColor.labelColor;
            stateColor = UIColor.systemBlueColor;
        } else if (active) {
            fillColor = [UIColor.secondarySystemFillColor colorWithAlphaComponent:0.72];
            borderColor = [UIColor.separatorColor colorWithAlphaComponent:0.75];
            titleColor = UIColor.labelColor;
            stateColor = UIColor.secondaryLabelColor;
        } else {
            fillColor = [UIColor.tertiarySystemBackgroundColor colorWithAlphaComponent:0.92];
            borderColor = [UIColor.separatorColor colorWithAlphaComponent:0.65];
            titleColor = UIColor.secondaryLabelColor;
            stateColor = UIColor.systemBlueColor;
        }
    } else {
        if (frontmost) {
            fillColor = [UIColor colorWithRed:0.84 green:0.90 blue:1.0 alpha:1.0];
            borderColor = [UIColor colorWithRed:0.26 green:0.46 blue:0.92 alpha:1.0];
            titleColor = UIColor.blackColor;
            stateColor = [UIColor colorWithRed:0.13 green:0.31 blue:0.82 alpha:1.0];
        } else if (active) {
            fillColor = [UIColor colorWithWhite:0.92 alpha:1.0];
            borderColor = [UIColor colorWithWhite:0.75 alpha:1.0];
            titleColor = UIColor.blackColor;
            stateColor = UIColor.darkGrayColor;
        } else {
            fillColor = [UIColor colorWithWhite:0.96 alpha:1.0];
            borderColor = [UIColor colorWithWhite:0.78 alpha:1.0];
            titleColor = UIColor.darkGrayColor;
            stateColor = [UIColor colorWithRed:0.13 green:0.31 blue:0.82 alpha:1.0];
        }
    }

    NSDictionary<NSAttributedStringKey, id> *titleAttributes = @{
        NSFontAttributeName: titleFont,
        NSForegroundColorAttributeName: titleColor,
        NSParagraphStyleAttributeName: paragraphStyle,
    };
    NSDictionary<NSAttributedStringKey, id> *stateAttributes = @{
        NSFontAttributeName: stateFont,
        NSForegroundColorAttributeName: stateColor,
        NSParagraphStyleAttributeName: paragraphStyle,
    };
    NSMutableAttributedString *attributedTitle =
        [[NSMutableAttributedString alloc] initWithString:title attributes:titleAttributes];
    [attributedTitle appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:stateAttributes]];
    [attributedTitle appendAttributedString:[[NSAttributedString alloc] initWithString:state attributes:stateAttributes]];
    [button setAttributedTitle:attributedTitle forState:UIControlStateNormal];
    button.backgroundColor = fillColor;
    button.layer.borderColor = borderColor.CGColor;
    button.accessibilityValue = state;
}

- (UIView *)workspaceCardWithContentStack:(UIStackView **)contentStackOut {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    CGFloat cornerRadius = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 18.0;
    CGFloat contentInset = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 18.0;
    CGFloat contentSpacing = ISHWorkspaceUsesPhoneLayout() ? 10.0 : 14.0;
    CGFloat shadowRadius = ISHWorkspaceUsesPhoneLayout() ? 12.0 : 18.0;
    CGFloat shadowOffset = ISHWorkspaceUsesPhoneLayout() ? 6.0 : 8.0;
    card.layer.cornerRadius = cornerRadius;
    card.layer.masksToBounds = NO;
    card.layer.shadowColor = UIColor.blackColor.CGColor;
    card.layer.shadowOpacity = 0.08;
    card.layer.shadowRadius = shadowRadius;
    card.layer.shadowOffset = CGSizeMake(0, shadowOffset);
    if (@available(iOS 13.0, *)) {
        card.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        card.backgroundColor = [UIColor colorWithWhite:0.96 alpha:1.0];
    }

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = contentSpacing;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contentStack];

    [NSLayoutConstraint activateConstraints:@[
        [contentStack.topAnchor constraintEqualToAnchor:card.topAnchor constant:contentInset],
        [contentStack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:contentInset],
        [contentStack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-contentInset],
        [contentStack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-contentInset],
    ]];

    if (contentStackOut != NULL)
        *contentStackOut = contentStack;
    return card;
}

- (CGRect)desktopUsableBounds {
    UIEdgeInsets insets = self.view.safeAreaInsets;
    CGRect bounds = self.desktopSurfaceView.bounds;
    if (ISHWorkspaceUsesPhoneLayout()) {
        return UIEdgeInsetsInsetRect(bounds, UIEdgeInsetsMake(insets.top, insets.left, insets.bottom, insets.right));
    }
    return UIEdgeInsetsInsetRect(bounds, UIEdgeInsetsMake(insets.top, 0, 0, 0));
}

- (CGRect)desktopFrameForWindowWithPreferredSize:(CGSize)preferredSize {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat width = MIN(preferredSize.width, CGRectGetWidth(usableBounds));
    CGFloat height = MIN(preferredSize.height, CGRectGetHeight(usableBounds));
    CGFloat cascadeStep = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 28.0;
    NSUInteger cascadeCount = ISHWorkspaceUsesPhoneLayout() ? 4 : 6;
    CGFloat verticalOffsetFactor = ISHWorkspaceUsesPhoneLayout() ? 0.08 : 0.16;
    CGFloat offset = (CGFloat) (self.desktopWindowCascadeIndex % cascadeCount) * cascadeStep;
    CGFloat originX = CGRectGetMinX(usableBounds) + MAX(0, (CGRectGetWidth(usableBounds) - width) * 0.5) + offset;
    CGFloat originY = CGRectGetMinY(usableBounds) + MAX(0, (CGRectGetHeight(usableBounds) - height) * verticalOffsetFactor) + offset;
    originX = MIN(originX, CGRectGetMaxX(usableBounds) - width);
    originY = MIN(originY, CGRectGetMaxY(usableBounds) - height);
    self.desktopWindowCascadeIndex += 1;
    return CGRectIntegral(CGRectMake(originX, originY, width, height));
}

- (CGRect)clampedDesktopFrame:(CGRect)frame forWindow:(ISHWorkspaceContainedWindowView *)windowView {
    CGRect usableBounds = [self desktopUsableBounds];
    if (CGRectGetWidth(frame) > CGRectGetWidth(usableBounds))
        frame.size.width = CGRectGetWidth(usableBounds);
    if (CGRectGetHeight(frame) > CGRectGetHeight(usableBounds))
        frame.size.height = CGRectGetHeight(usableBounds);

    CGFloat visibleWidth = MIN(CGRectGetWidth(frame), 140.0);
    CGFloat minX = CGRectGetMinX(usableBounds) - (CGRectGetWidth(frame) - visibleWidth);
    CGFloat maxX = CGRectGetMaxX(usableBounds) - visibleWidth;
    CGFloat visibleHeight = MIN(CGRectGetHeight(frame), ISHWorkspaceWindowTitleBarHeight);
    CGFloat minY = CGRectGetMinY(usableBounds);
    CGFloat maxY = CGRectGetMaxY(usableBounds) - visibleHeight;

    if (maxX < minX)
        maxX = minX;
    if (maxY < minY)
        maxY = minY;

    frame.origin.x = MIN(MAX(frame.origin.x, minX), maxX);
    frame.origin.y = MIN(MAX(frame.origin.y, minY), maxY);
    return ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
}

- (void)clampDesktopWindowToVisibleBounds:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.superview == nil || CGRectIsEmpty(windowView.bounds))
        return;

    windowView.frame = [self clampedDesktopFrame:windowView.frame forWindow:windowView];
}

- (void)pinDesktopWindowToBottomCenter:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil || windowView.superview == nil)
        return;
    CGRect usableBounds = [self desktopUsableBounds];
    CGRect frame = windowView.frame;
    frame.origin.x = CGRectGetMinX(usableBounds) + (CGRectGetWidth(usableBounds) - CGRectGetWidth(frame)) * 0.5;
    frame.origin.y = CGRectGetMaxY(usableBounds) - CGRectGetHeight(frame);
    windowView.frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
}

- (void)positionDesktopWindowAtTopRight:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil || windowView.superview == nil)
        return;
    CGRect usableBounds = [self desktopUsableBounds];
    CGRect frame = windowView.frame;
    CGFloat inset = ISHWorkspaceUsesPhoneLayout() ? 8.0 : 12.0;
    frame.origin.x = CGRectGetMaxX(usableBounds) - CGRectGetWidth(frame) - inset;
    frame.origin.y = CGRectGetMinY(usableBounds) + inset;
    windowView.frame = [self clampedDesktopFrame:frame forWindow:windowView];
}

- (void)resizeDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView
                     toSize:(CGSize)size
                   animated:(BOOL)animated {
    if (windowView == nil || windowView.superview == nil)
        return;

    CGRect usableBounds = [self desktopUsableBounds];
    CGSize minimumSize = windowView.minimumSize;
    CGFloat width = MAX(minimumSize.width, size.width);
    CGFloat height = MAX(minimumSize.height, size.height);
    if (windowView.maximumSize.width > 0) {
        width = MIN(width, windowView.maximumSize.width);
    }
    if (windowView.maximumSize.height > 0) {
        height = MIN(height, windowView.maximumSize.height);
    }
    width = MIN(width, CGRectGetWidth(usableBounds));
    height = MIN(height, CGRectGetHeight(usableBounds));

    CGRect frame = windowView.frame;
    frame.size = CGSizeMake(width, height);
    frame = [self clampedDesktopFrame:frame forWindow:windowView];
    windowView.preferredSize = frame.size;
    if (windowView.pinnedToBottomCenter) {
        frame.origin.x = CGRectGetMinX(usableBounds) + (CGRectGetWidth(usableBounds) - CGRectGetWidth(frame)) * 0.5;
        frame.origin.y = CGRectGetMaxY(usableBounds) - CGRectGetHeight(frame);
        frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
    }
    void (^changes)(void) = ^{
        windowView.frame = frame;
    };
    if (animated) {
        [UIView animateWithDuration:0.22 animations:changes];
    } else {
        changes();
    }
}

- (NSDictionary<NSString *, NSNumber *> *)normalizedFrameDescriptorForFrame:(CGRect)frame {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat usableWidth = CGRectGetWidth(usableBounds);
    CGFloat usableHeight = CGRectGetHeight(usableBounds);
    if (usableWidth <= 0 || usableHeight <= 0)
        return nil;

    return @{
        @"x": @((CGRectGetMinX(frame) - CGRectGetMinX(usableBounds)) / usableWidth),
        @"y": @((CGRectGetMinY(frame) - CGRectGetMinY(usableBounds)) / usableHeight),
        @"width": @(CGRectGetWidth(frame) / usableWidth),
        @"height": @(CGRectGetHeight(frame) / usableHeight),
    };
}

- (CGRect)frameFromNormalizedDescriptor:(NSDictionary<NSString *, id> *)descriptor
                           fallbackSize:(CGSize)fallbackSize {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat usableWidth = CGRectGetWidth(usableBounds);
    CGFloat usableHeight = CGRectGetHeight(usableBounds);
    if (usableWidth <= 0 || usableHeight <= 0)
        return CGRectMake(0, 0, MAX(1, fallbackSize.width), MAX(1, fallbackSize.height));

    CGFloat normalizedWidth = [descriptor[@"width"] doubleValue];
    CGFloat normalizedHeight = [descriptor[@"height"] doubleValue];
    CGFloat width = normalizedWidth > 0 ? normalizedWidth * usableWidth : fallbackSize.width;
    CGFloat height = normalizedHeight > 0 ? normalizedHeight * usableHeight : fallbackSize.height;
    width = MIN(MAX(width, 1), usableWidth);
    height = MIN(MAX(height, 1), usableHeight);

    CGFloat normalizedX = [descriptor[@"x"] doubleValue];
    CGFloat normalizedY = [descriptor[@"y"] doubleValue];
    CGFloat originX = CGRectGetMinX(usableBounds) + normalizedX * usableWidth;
    CGFloat originY = CGRectGetMinY(usableBounds) + normalizedY * usableHeight;
    originX = MIN(MAX(originX, CGRectGetMinX(usableBounds)), CGRectGetMaxX(usableBounds) - width);
    originY = MIN(MAX(originY, CGRectGetMinY(usableBounds)), CGRectGetMaxY(usableBounds) - height);
    return CGRectIntegral(CGRectMake(originX, originY, width, height));
}

- (void)applySavedFrameDescriptor:(NSDictionary<NSString *, id> *)descriptor
                         toWindow:(ISHWorkspaceContainedWindowView *)windowView
                     fallbackSize:(CGSize)fallbackSize {
    if (![descriptor isKindOfClass:NSDictionary.class] || windowView == nil)
        return;
    CGRect frame = [self frameFromNormalizedDescriptor:descriptor fallbackSize:fallbackSize];
    windowView.frame = frame;
    windowView.preferredSize = frame.size;
    windowView.didApplyInitialFrame = YES;
    [self clampDesktopWindowToVisibleBounds:windowView];
}

- (void)applyInitialFrameIfNeededToDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.didApplyInitialFrame || self.desktopSurfaceView.bounds.size.width <= 0 || self.desktopSurfaceView.bounds.size.height <= 0)
        return;
    windowView.frame = [self desktopFrameForWindowWithPreferredSize:windowView.preferredSize];
    windowView.didApplyInitialFrame = YES;
}

- (ISHWorkspaceContainedWindowView *)createDesktopWindowWithTitle:(NSString *)title
                                                    preferredSize:(CGSize)preferredSize
                                                 showsCloseButton:(BOOL)showsCloseButton {
    ISHWorkspaceContainedWindowView *windowView = [[ISHWorkspaceContainedWindowView alloc] initWithTitle:title
                                                                                         showsCloseButton:showsCloseButton];
    windowView.preferredSize = preferredSize;
    windowView.frame = CGRectIntegral(CGRectMake(0, 0,
                                                 MAX(1, preferredSize.width),
                                                 MAX(1, preferredSize.height)));
    [self.desktopSurfaceView addSubview:windowView];
    [self.desktopWindows addObject:windowView];
    [self applyInitialFrameIfNeededToDesktopWindow:windowView];
    [self.desktopSurfaceView bringSubviewToFront:windowView];
    __weak typeof(self) weakSelf = self;
    windowView.didBecomeFrontmostHandler = ^{
        [weakSelf refreshDockButtons];
    };
    return windowView;
}

- (void)attachViewController:(UIViewController *)viewController toDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if ([viewController isKindOfClass:WorkspaceThemedToolViewController.class]) {
        ((WorkspaceThemedToolViewController *) viewController).workspaceHostViewController = self;
    }
    [self addChildViewController:viewController];
    viewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    [windowView.contentContainerView addSubview:viewController.view];
    [NSLayoutConstraint activateConstraints:@[
        [viewController.view.topAnchor constraintEqualToAnchor:windowView.contentContainerView.topAnchor],
        [viewController.view.leadingAnchor constraintEqualToAnchor:windowView.contentContainerView.leadingAnchor],
        [viewController.view.trailingAnchor constraintEqualToAnchor:windowView.contentContainerView.trailingAnchor],
        [viewController.view.bottomAnchor constraintEqualToAnchor:windowView.contentContainerView.bottomAnchor],
    ]];
    [viewController didMoveToParentViewController:self];

    __weak typeof(self) weakSelf = self;
    __weak typeof(viewController) weakViewController = viewController;
    __weak typeof(windowView) weakWindowView = windowView;
    windowView.closeHandler = ^{
        typeof(self) strongSelf = weakSelf;
        UIViewController *strongViewController = weakViewController;
        ISHWorkspaceContainedWindowView *strongWindowView = weakWindowView;
        if (strongSelf == nil || strongViewController == nil || strongWindowView == nil)
            return;
        [strongViewController willMoveToParentViewController:nil];
        [strongViewController.view removeFromSuperview];
        [strongViewController removeFromParentViewController];
        [strongSelf.desktopWindows removeObject:strongWindowView];
        [strongWindowView removeFromSuperview];
        [strongSelf refreshDockButtons];
    };
}

- (NSUUID *)persistentTerminalUUIDForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
    if (terminalViewController == nil)
        return nil;
    NSUUID *sessionUUID = terminalViewController.sessionTerminalUUID;
    if (sessionUUID != nil)
        return sessionUUID;
    return terminalViewController.terminal.uuid;
}

- (NSUUID *)displayedTerminalUUIDForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
    if (terminalViewController == nil)
        return nil;
    return terminalViewController.terminal.uuid;
}

- (NSString *)persistentTerminalRoleForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.workspaceTerminalRole.length > 0)
        return windowView.workspaceTerminalRole;
    TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
    if (terminalViewController == nil)
        return nil;
    return ISHWorkspaceTerminalRoleForTerminal(terminalViewController.terminal);
}

- (NSDictionary<NSString *, id> *)savedLayoutDescriptorForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    NSDictionary<NSString *, NSNumber *> *frameDescriptor = [self normalizedFrameDescriptorForFrame:windowView.frame];
    if (frameDescriptor == nil)
        return nil;

    if (windowView == self.dashboardWindow) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindDashboard,
            @"frame": frameDescriptor,
            @"compact": @(self.dashboardIsCompact),
            @"hidden": @(self.dashboardWindow.hidden),
            @"expandedSize": ISHWorkspaceSizeDescriptor(self.dashboardExpandedSize),
        };
    }

    if (windowView == self.dockWindow) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindDock,
            @"frame": frameDescriptor,
            @"size": ISHWorkspaceSizeDescriptor(windowView.bounds.size),
            @"pinned": @(windowView.pinnedToBottomCenter),
        };
    }

    if (windowView.hostedTerminalViewController != nil) {
        NSUUID *sessionTerminalUUID = [self persistentTerminalUUIDForWindow:windowView];
        NSUUID *displayTerminalUUID = [self displayedTerminalUUIDForWindow:windowView];
        NSString *terminalRole = [self persistentTerminalRoleForWindow:windowView];
        if (sessionTerminalUUID == nil && displayTerminalUUID == nil)
            return nil;
        NSMutableDictionary<NSString *, id> *descriptor = [@{
            @"kind": ISHWorkspaceSavedLayoutKindTerminal,
            @"frame": frameDescriptor,
            @"terminalUUID": (displayTerminalUUID ?: sessionTerminalUUID).UUIDString,
        } mutableCopy];
        if (sessionTerminalUUID != nil)
            descriptor[@"sessionTerminalUUID"] = sessionTerminalUUID.UUIDString;
        if (terminalRole.length > 0)
            descriptor[@"terminalRole"] = terminalRole;
        return descriptor;
    }

    if (windowView.workspaceToolIdentifier.length > 0) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindTool,
            @"frame": frameDescriptor,
            @"toolIdentifier": windowView.workspaceToolIdentifier,
        };
    }

    return nil;
}

- (void)closeAllRestorableDesktopWindows {
    for (ISHWorkspaceContainedWindowView *windowView in self.desktopWindows.copy) {
        if (windowView == self.dashboardWindow || windowView == self.dockWindow)
            continue;
        if (windowView.closeHandler != nil)
            windowView.closeHandler();
    }
}

- (void)applySavedDashboardDescriptor:(NSDictionary<NSString *, id> *)descriptor {
    NSDictionary<NSString *, id> *frameDescriptor = descriptor[@"frame"];
    CGSize expandedSize = ISHWorkspaceSizeFromDescriptor(descriptor[@"expandedSize"]);
    if (expandedSize.width > 0 && expandedSize.height > 0) {
        self.dashboardExpandedSize = expandedSize;
    } else if (self.dashboardWindow.bounds.size.width > 0 && self.dashboardWindow.bounds.size.height > 0) {
        self.dashboardExpandedSize = self.dashboardWindow.bounds.size;
    }

    self.dashboardIsCompact = [descriptor[@"compact"] boolValue];
    [self.dashboardWindow setUtilityButtonTitle:(self.dashboardIsCompact ? @"Full" : @"Mini")
                                        handler:self.dashboardWindow.utilityHandler];
    CGSize fallbackSize = self.dashboardWindow.bounds.size.width > 0
        ? self.dashboardWindow.bounds.size
        : self.dashboardWindow.preferredSize;
    [self applySavedFrameDescriptor:frameDescriptor toWindow:self.dashboardWindow fallbackSize:fallbackSize];
    self.dashboardWindow.hidden = [descriptor[@"hidden"] boolValue];
}

- (NSString *)terminalRoleFromSavedDescriptor:(NSDictionary<NSString *, id> *)descriptor
                               displayTerminal:(Terminal *)displayTerminal {
    NSString *terminalRole = descriptor[@"terminalRole"];
    if (terminalRole.length > 0)
        return terminalRole;
    if (displayTerminal != nil)
        return ISHWorkspaceTerminalRoleForTerminal(displayTerminal);
    return ISHWorkspaceTerminalRoleSessionShell;
}

- (void)configureRestoredTerminalViewController:(TerminalViewController *)terminalViewController
                                    inWindow:(ISHWorkspaceContainedWindowView *)windowView
                            displayTerminalUUID:(NSUUID *)displayTerminalUUID
                                  terminalRole:(NSString *)terminalRole {
    Terminal *displayTerminal = displayTerminalUUID != nil ? [Terminal terminalWithUUID:displayTerminalUUID] : terminalViewController.terminal;
    if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSystemConsole]) {
        if ([ISHWorkspaceTerminalRoleForTerminal(displayTerminal) isEqualToString:ISHWorkspaceTerminalRoleSystemConsole]) {
            terminalViewController.terminal = displayTerminal;
        } else {
            [terminalViewController showSystemConsoleForCurrentSession];
            displayTerminal = terminalViewController.terminal;
        }
    } else if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSessionShell]) {
        if ([ISHWorkspaceTerminalRoleForTerminal(displayTerminal) isEqualToString:ISHWorkspaceTerminalRoleSessionShell]) {
            terminalViewController.terminal = displayTerminal;
        } else {
            [terminalViewController showSessionShellForCurrentSession];
            displayTerminal = terminalViewController.terminal;
        }
    } else if (displayTerminal != nil) {
        terminalViewController.terminal = displayTerminal;
    }
    NSString *effectiveRole = terminalRole.length > 0 ? terminalRole : ISHWorkspaceTerminalRoleForTerminal(displayTerminal);
    windowView.workspaceTerminalRole = effectiveRole;
    windowView.titleLabel.text = ISHWorkspaceTitleForTerminalRole(effectiveRole, displayTerminal);
}

- (ISHWorkspaceContainedWindowView *)restoreDesktopTerminalWindowWithSessionUUID:(NSUUID *)sessionTerminalUUID
                                                             displayTerminalUUID:(NSUUID *)displayTerminalUUID
                                                                   terminalRole:(NSString *)terminalRole {
    if (displayTerminalUUID == nil && sessionTerminalUUID == nil)
        return nil;

    if (@available(iOS 13.0, *)) {
        UISceneSession *existingSession = displayTerminalUUID != nil
            ? [self sceneSessionHostingTerminalUUID:displayTerminalUUID]
            : nil;
        if (existingSession == nil && sessionTerminalUUID != nil) {
            existingSession = [self sceneSessionHostingTerminalUUID:sessionTerminalUUID];
        }
        UISceneSession *currentSession = self.view.window.windowScene.session;
        if (existingSession != nil && existingSession != currentSession)
            return nil;
    }

    ISHWorkspaceContainedWindowView *containedWindow = displayTerminalUUID != nil
        ? [self desktopWindowDisplayingTerminalUUID:displayTerminalUUID]
        : nil;
    if (containedWindow == nil && sessionTerminalUUID != nil) {
        containedWindow = [self desktopWindowHostingTerminalUUID:sessionTerminalUUID];
        if (containedWindow != nil && terminalRole.length > 0 &&
            ![containedWindow.workspaceTerminalRole isEqualToString:terminalRole]) {
            containedWindow = nil;
        }
    }
    if (containedWindow != nil)
        return containedWindow;

    Terminal *displayTerminal = displayTerminalUUID != nil ? [Terminal terminalWithUUID:displayTerminalUUID] : nil;
    if (displayTerminal != nil && displayTerminal.webView.superview != nil)
        return nil;

    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil)
        return nil;

    NSUUID *restoreUUID = sessionTerminalUUID ?: displayTerminalUUID;
    NSString *title = ISHWorkspaceTitleForTerminalRole(terminalRole, displayTerminal);
    ISHWorkspaceContainedWindowView *windowView =
        [self openDesktopTerminalWindowWithTitle:title terminalViewController:terminalViewController];
    [terminalViewController reconnectSessionFromTerminalUUID:restoreUUID];
    [self configureRestoredTerminalViewController:terminalViewController
                                         inWindow:windowView
                                 displayTerminalUUID:displayTerminalUUID
                                       terminalRole:terminalRole];
    return windowView;
}

- (ISHWorkspaceContainedWindowView *)openWorkspaceToolWindowWithIdentifier:(NSString *)toolIdentifier {
    UIViewController *viewController = ISHCreateWorkspaceToolViewController(toolIdentifier);
    if (viewController == nil)
        return nil;
    CGSize preferredSize = ISHWorkspacePreferredToolContentSize(toolIdentifier);
    viewController.preferredContentSize = preferredSize;
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:ISHWorkspaceToolTitle(toolIdentifier)
                             preferredSize:preferredSize
                          showsCloseButton:YES];
    windowView.workspaceToolIdentifier = toolIdentifier;
    CGSize minimumSize = ISHWorkspaceMinimumToolContentSize(toolIdentifier);
    if (!CGSizeEqualToSize(minimumSize, CGSizeZero)) {
        windowView.resizable = YES;
        windowView.minimumSize = minimumSize;
    }
    [self attachViewController:viewController toDesktopWindow:windowView];
    [self refreshDockButtons];
    return windowView;
}

- (void)updateDashboardUtilityButton {
    if (self.dashboardWindow == nil)
        return;

    __weak typeof(self) weakSelf = self;
    NSString *title = self.dashboardIsCompact ? @"Full" : @"Mini";
    [self.dashboardWindow setUtilityButtonTitle:title handler:^{
        [weakSelf toggleDashboardCompactMode];
    }];
}

- (void)toggleDashboardCompactMode {
    ISHWorkspaceContainedWindowView *dashboardWindow = self.dashboardWindow;
    if (dashboardWindow == nil)
        return;

    if (self.dashboardIsCompact) {
        self.dashboardIsCompact = NO;
        [self updateDashboardUtilityButton];
        [self resizeDesktopWindow:dashboardWindow
                           toSize:self.dashboardExpandedSize
                         animated:YES];
        return;
    }

    self.dashboardExpandedSize = dashboardWindow.bounds.size;
    self.dashboardIsCompact = YES;
    [self updateDashboardUtilityButton];
    [self resizeDesktopWindow:dashboardWindow
                       toSize:ISHWorkspaceCompactDashboardSize()
                     animated:YES];
}

- (void)openDashboardWindow:(id)sender {
    if (self.dashboardWindow != nil) {
        self.dashboardWindow.hidden = NO;
        [self focusDesktopWindow:self.dashboardWindow];
    }
}

- (ISHWorkspaceContainedWindowView *)createDockWindow {
    CGSize preferredSize = ISHWorkspacePreferredDockContentSize();
    CGFloat stackInsetY = ISHWorkspaceUsesPhoneLayout() ? 2.0 : 3.0;
    CGFloat stackInsetX = ISHWorkspaceUsesPhoneLayout() ? 4.0 : 6.0;
    CGFloat tileSpacing = ISHWorkspaceUsesPhoneLayout() ? 3.0 : 4.0;
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:@"Dock"
                             preferredSize:preferredSize
                          showsCloseButton:NO];
    self.dockWindow = windowView;
    windowView.draggable = YES;
    windowView.resizable = YES;
    windowView.resizeHandleAtTopRight = YES;
    windowView.minimumSize = ISHWorkspaceMinimumDockContentSize();
    windowView.maximumSize = ISHWorkspaceMaximumDockContentSize();
    windowView.pinnedToBottomCenter = YES;

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 0;
    [windowView.contentContainerView addSubview:stack];

    UIStackView *appsRow = [UIStackView new];
    appsRow.axis = UILayoutConstraintAxisHorizontal;
    appsRow.spacing = tileSpacing;
    appsRow.distribution = UIStackViewDistributionFillEqually;
    self.dockUtilsButton = [self workspaceDockTileButtonWithTitle:@"Utils"
                                                         selector:@selector(toggleClockFromDock:)
                                                       identifier:@"utils"];
    UILongPressGestureRecognizer *utilsLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleUtilsDockLongPress:)];
    utilsLongPressRecognizer.minimumPressDuration = 0.25;
    [self.dockUtilsButton addGestureRecognizer:utilsLongPressRecognizer];
    self.dockTerminalButton = [self workspaceDockTileButtonWithTitle:@"Terminal"
                                                            selector:@selector(openOrFocusTerminalFromDock:)
                                                          identifier:ISHWorkspaceTerminalRoleSessionShell];
    UILongPressGestureRecognizer *terminalLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleTerminalDockLongPress:)];
    terminalLongPressRecognizer.minimumPressDuration = 0.25;
    [self.dockTerminalButton addGestureRecognizer:terminalLongPressRecognizer];

    [appsRow addArrangedSubview:self.dockUtilsButton];
    [appsRow addArrangedSubview:self.dockTerminalButton];

    [stack addArrangedSubview:appsRow];

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:windowView.contentContainerView.topAnchor constant:stackInsetY],
        [stack.leadingAnchor constraintEqualToAnchor:windowView.contentContainerView.leadingAnchor constant:stackInsetX],
        [stack.trailingAnchor constraintEqualToAnchor:windowView.contentContainerView.trailingAnchor constant:-stackInsetX],
        [stack.bottomAnchor constraintEqualToAnchor:windowView.contentContainerView.bottomAnchor constant:-stackInsetY],
    ]];

    [self pinDesktopWindowToBottomCenter:windowView];
    [self refreshDockButtons];
    return windowView;
}

- (void)saveWorkspaceLayout:(id)sender {
    NSMutableArray<NSDictionary<NSString *, id> *> *layout = [NSMutableArray array];
    for (UIView *subview in self.desktopSurfaceView.subviews) {
        if (![subview isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        NSDictionary<NSString *, id> *descriptor =
            [self savedLayoutDescriptorForWindow:(ISHWorkspaceContainedWindowView *) subview];
        if (descriptor != nil)
            [layout addObject:descriptor];
    }
    [NSUserDefaults.standardUserDefaults setObject:layout forKey:ISHWorkspaceSavedLayoutDefaultsKey];
}

- (void)restoreWorkspaceLayout:(id)sender {
    NSArray<NSDictionary<NSString *, id> *> *layout =
        [NSUserDefaults.standardUserDefaults arrayForKey:ISHWorkspaceSavedLayoutDefaultsKey];
    if (![layout isKindOfClass:NSArray.class] || layout.count == 0) {
        UIAlertController *alert =
            [UIAlertController alertControllerWithTitle:@"No Saved Layout"
                                                message:@"Save a workspace arrangement first, then restore it from here."
                                         preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    NSDictionary<NSString *, id> *dashboardDescriptor = nil;
    NSDictionary<NSString *, id> *dockDescriptor = nil;
    NSMutableArray<NSDictionary<NSString *, id> *> *windowDescriptors = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *descriptor in layout) {
        NSString *kind = descriptor[@"kind"];
        if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindDashboard]) {
            dashboardDescriptor = descriptor;
        } else if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindDock]) {
            dockDescriptor = descriptor;
        } else {
            [windowDescriptors addObject:descriptor];
        }
    }

    [self closeAllRestorableDesktopWindows];
    if (dashboardDescriptor != nil)
        [self applySavedDashboardDescriptor:dashboardDescriptor];
    if (dockDescriptor != nil && self.dockWindow != nil) {
        CGSize savedDockSize = ISHWorkspaceSizeFromDescriptor(dockDescriptor[@"size"]);
        if (savedDockSize.width > 0 && savedDockSize.height > 0) {
            [self resizeDesktopWindow:self.dockWindow toSize:savedDockSize animated:NO];
        }
        if ([dockDescriptor[@"pinned"] boolValue]) {
            self.dockWindow.pinnedToBottomCenter = YES;
            [self pinDesktopWindowToBottomCenter:self.dockWindow];
        } else {
            self.dockWindow.pinnedToBottomCenter = NO;
            [self applySavedFrameDescriptor:dockDescriptor[@"frame"]
                                   toWindow:self.dockWindow
                               fallbackSize:self.dockWindow.bounds.size];
        }
    }

    NSSet<NSString *> *deduplicatedTerminalRoles =
        [NSSet setWithArray:@[ISHWorkspaceTerminalRoleSessionShell, ISHWorkspaceTerminalRoleSystemConsole]];
    NSMutableSet<NSString *> *restoredTerminalRoles = [NSMutableSet set];
    for (NSDictionary<NSString *, id> *descriptor in windowDescriptors) {
        NSString *kind = descriptor[@"kind"];
        NSDictionary<NSString *, id> *frameDescriptor = descriptor[@"frame"];
        if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindTool]) {
            NSString *toolIdentifier = descriptor[@"toolIdentifier"];
            if (toolIdentifier.length == 0)
                continue;
            ISHWorkspaceContainedWindowView *windowView = [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
            [self applySavedFrameDescriptor:frameDescriptor
                                   toWindow:windowView
                               fallbackSize:ISHWorkspacePreferredToolContentSize(toolIdentifier)];
            continue;
        }
        if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindTerminal]) {
            NSUUID *displayTerminalUUID = [[NSUUID alloc] initWithUUIDString:descriptor[@"terminalUUID"]];
            NSUUID *sessionTerminalUUID = [[NSUUID alloc] initWithUUIDString:descriptor[@"sessionTerminalUUID"]];
            if (displayTerminalUUID == nil && sessionTerminalUUID == nil)
                continue;
            Terminal *displayTerminal = displayTerminalUUID != nil ? [Terminal terminalWithUUID:displayTerminalUUID] : nil;
            NSString *terminalRole = [self terminalRoleFromSavedDescriptor:descriptor displayTerminal:displayTerminal];
            if ([deduplicatedTerminalRoles containsObject:terminalRole] && [restoredTerminalRoles containsObject:terminalRole])
                continue;
            ISHWorkspaceContainedWindowView *windowView =
                [self restoreDesktopTerminalWindowWithSessionUUID:(sessionTerminalUUID ?: displayTerminalUUID)
                                              displayTerminalUUID:displayTerminalUUID
                                                    terminalRole:terminalRole];
            if (windowView != nil) {
                if ([deduplicatedTerminalRoles containsObject:terminalRole])
                    [restoredTerminalRoles addObject:terminalRole];
                [self applySavedFrameDescriptor:frameDescriptor
                                       toWindow:windowView
                                   fallbackSize:ISHWorkspacePreferredTerminalContentSize()];
            }
        }
    }

    [self refreshWorkspaceStatus];
    [self applyCompactSizingToOpenWorkspaceToolWindows];
}

- (ISHWorkspaceContainedWindowView *)desktopWindowDisplayingTerminalUUID:(NSUUID *)terminalUUID {
    if (terminalUUID == nil)
        return nil;

    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
        if (terminalViewController == nil)
            continue;
        if ([terminalViewController.terminal.uuid isEqual:terminalUUID])
            return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)desktopWindowForTerminalRole:(NSString *)terminalRole {
    if (terminalRole.length == 0)
        return nil;

    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView.hostedTerminalViewController == nil)
            continue;
        NSString *windowRole = [self persistentTerminalRoleForWindow:windowView];
        if ([windowRole isEqualToString:terminalRole])
            return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)frontmostDesktopTerminalWindow {
    for (UIView *view in [self.desktopSurfaceView.subviews reverseObjectEnumerator]) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView.hidden || windowView.hostedTerminalViewController == nil)
            continue;
        return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)desktopWindowForToolIdentifier:(NSString *)toolIdentifier {
    if (toolIdentifier.length == 0)
        return nil;

    for (UIView *view in [self.desktopSurfaceView.subviews reverseObjectEnumerator]) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView.hidden)
            continue;
        if ([windowView.workspaceToolIdentifier isEqualToString:toolIdentifier])
            return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)frontmostDesktopWindowExcludingDock {
    for (UIView *view in [self.desktopSurfaceView.subviews reverseObjectEnumerator]) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView == self.dockWindow || windowView.hidden)
            continue;
        return windowView;
    }
    return nil;
}

- (void)focusDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil)
        return;
    [self.desktopSurfaceView bringSubviewToFront:windowView];
    [self refreshDockButtons];
}

- (ISHWorkspaceContainedWindowView *)desktopWindowHostingTerminalUUID:(NSUUID *)terminalUUID {
    if (terminalUUID == nil)
        return nil;

    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
        if (terminalViewController == nil)
            continue;
        if ([terminalViewController.sessionTerminalUUID isEqual:terminalUUID])
            return windowView;
        if ([terminalViewController.terminal.uuid isEqual:terminalUUID])
            return windowView;
    }
    return nil;
}

- (TerminalViewController *)createDesktopTerminalViewController {
    TerminalViewController *terminalViewController =
        (TerminalViewController *) [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    if (![terminalViewController isKindOfClass:TerminalViewController.class])
        return nil;
    if (@available(iOS 13.0, *)) {
        terminalViewController.sceneSession = self.view.window.windowScene.session;
    }
    terminalViewController.showsWorkspaceDashboardButton = NO;
    terminalViewController.embeddedInWorkspaceWindow = YES;
    return terminalViewController;
}

- (ISHWorkspaceContainedWindowView *)openDesktopTerminalWindowWithTitle:(NSString *)title
                                                 terminalViewController:(TerminalViewController *)terminalViewController {
    CGSize preferredSize = ISHWorkspacePreferredTerminalContentSize();
    terminalViewController.preferredContentSize = preferredSize;
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:title
                             preferredSize:preferredSize
                          showsCloseButton:YES];
    windowView.hostedTerminalViewController = terminalViewController;
    windowView.resizable = YES;
    windowView.minimumSize = ISHWorkspaceMinimumTerminalContentSize();
    [self attachViewController:terminalViewController toDesktopWindow:windowView];
    return windowView;
}

- (void)workspaceClearArrangedSubviewsFromStack:(UIStackView *)stackView {
    NSArray<UIView *> *arrangedSubviews = stackView.arrangedSubviews.copy;
    for (UIView *view in arrangedSubviews) {
        [stackView removeArrangedSubview:view];
        [view removeFromSuperview];
    }
}

- (void)rebuildWorkspaceColumns {
    [self workspaceClearArrangedSubviewsFromStack:self.bodyStack];
    self.bodyStack.axis = UILayoutConstraintAxisVertical;
    self.bodyStack.spacing = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 20.0;
    self.bodyStack.alignment = UIStackViewAlignmentFill;
    self.bodyStack.distribution = UIStackViewDistributionFill;
    if (self.windowCard != nil)
        [self.bodyStack addArrangedSubview:self.windowCard];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Desktop";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }
    self.desktopWindows = [NSMutableArray array];

    self.desktopSurfaceView = [UIView new];
    self.desktopSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.desktopSurfaceView.backgroundColor = [UIColor.systemGroupedBackgroundColor colorWithAlphaComponent:1.0];
    } else {
        self.desktopSurfaceView.backgroundColor = [UIColor colorWithWhite:0.92 alpha:1.0];
    }
    [self.view addSubview:self.desktopSurfaceView];

    [NSLayoutConstraint activateConstraints:@[
        [self.desktopSurfaceView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.desktopSurfaceView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.desktopSurfaceView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.desktopSurfaceView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    ]];

    self.desktopWallpaperView = [UIImageView new];
    self.desktopWallpaperView.translatesAutoresizingMaskIntoConstraints = NO;
    self.desktopWallpaperView.contentMode = UIViewContentModeScaleAspectFill;
    self.desktopWallpaperView.clipsToBounds = YES;
    self.desktopWallpaperView.hidden = YES;
    [self.desktopSurfaceView addSubview:self.desktopWallpaperView];
    [NSLayoutConstraint activateConstraints:@[
        [self.desktopWallpaperView.topAnchor constraintEqualToAnchor:self.desktopSurfaceView.topAnchor],
        [self.desktopWallpaperView.leadingAnchor constraintEqualToAnchor:self.desktopSurfaceView.leadingAnchor],
        [self.desktopWallpaperView.trailingAnchor constraintEqualToAnchor:self.desktopSurfaceView.trailingAnchor],
        [self.desktopWallpaperView.bottomAnchor constraintEqualToAnchor:self.desktopSurfaceView.bottomAnchor],
    ]];

    ISHWorkspaceContainedWindowView *dashboardWindow =
        [self createDesktopWindowWithTitle:@"Dashboard"
                             preferredSize:ISHWorkspacePreferredDashboardContentSize()
                          showsCloseButton:YES];
    self.dashboardWindow = dashboardWindow;
    self.dashboardExpandedSize = dashboardWindow.preferredSize;
    self.dashboardIsCompact = NO;
    dashboardWindow.draggable = YES;
    dashboardWindow.resizable = YES;
    dashboardWindow.minimumSize = ISHWorkspaceMinimumDashboardContentSize();
    __weak typeof(self) weakSelf = self;
    __weak typeof(dashboardWindow) weakDashboardWindow = dashboardWindow;
    dashboardWindow.closeHandler = ^{
        weakDashboardWindow.hidden = YES;
        [weakSelf refreshDockButtons];
    };
    [self updateDashboardUtilityButton];
    [self createDockWindow];

    UIScrollView *scrollView = [UIScrollView new];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [dashboardWindow.contentContainerView addSubview:scrollView];

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 20.0;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [scrollView addSubview:contentStack];

    self.windowSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.sceneWindowsStack = [UIStackView new];
    self.sceneWindowsStack.axis = UILayoutConstraintAxisVertical;
    self.sceneWindowsStack.spacing = 10;
    self.sceneWindowsStack.translatesAutoresizingMaskIntoConstraints = NO;

    UIStackView *windowCardStack = nil;
    self.windowCard = [self workspaceCardWithContentStack:&windowCardStack];
    [windowCardStack addArrangedSubview:[self workspaceSectionTitle:@"Workspace layout"]];
    UILabel *windowSummaryDetailLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    if (@available(iOS 13.0, *)) {
        windowSummaryDetailLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        windowSummaryDetailLabel.textColor = UIColor.darkGrayColor;
    }
    windowSummaryDetailLabel.text = @"Dashboard is limited to workspace layout and scene management. Use the Doc's Utils and Terminal cards for tools, status, and terminal actions.";
    [windowCardStack addArrangedSubview:windowSummaryDetailLabel];
    [windowCardStack addArrangedSubview:self.windowSummaryLabel];
    [windowCardStack addArrangedSubview:self.sceneWindowsStack];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Save Current Layout"
                                                                   selector:@selector(saveWorkspaceLayout:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Restore Saved Layout"
                                                                   selector:@selector(restoreWorkspaceLayout:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"New Workspace Window"
                                                                   selector:@selector(openNewWorkspaceWindow:)]];

    self.bodyStack = [UIStackView new];
    self.bodyStack.translatesAutoresizingMaskIntoConstraints = NO;
    [contentStack addArrangedSubview:self.bodyStack];
    [self rebuildWorkspaceColumns];

    UILayoutGuide *safeArea = dashboardWindow.contentContainerView.safeAreaLayoutGuide;
    CGFloat contentInsetY = ISHWorkspaceUsesPhoneLayout() ? 16.0 : 24.0;
    CGFloat contentInsetX = ISHWorkspaceUsesPhoneLayout() ? 12.0 : 20.0;
    [NSLayoutConstraint activateConstraints:@[
        [scrollView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [scrollView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor],

        [contentStack.topAnchor constraintEqualToAnchor:scrollView.topAnchor constant:contentInsetY],
        [contentStack.leadingAnchor constraintEqualToAnchor:scrollView.leadingAnchor constant:contentInsetX],
        [contentStack.trailingAnchor constraintEqualToAnchor:scrollView.trailingAnchor constant:-contentInsetX],
        [contentStack.bottomAnchor constraintEqualToAnchor:scrollView.bottomAnchor constant:-contentInsetY],
        [contentStack.widthAnchor constraintEqualToAnchor:scrollView.widthAnchor constant:-(contentInsetX * 2.0)],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:TerminalRegistryDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:TerminalDidLoadNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:TerminalLoadFailedNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(workspaceThemeDidChange:)
                                               name:ISHWorkspaceToolThemeDidChangeNotification
                                             object:nil];
    if (@available(iOS 13.0, *)) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceStatus)
                                                   name:UISceneDidActivateNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceStatus)
                                                   name:UISceneDidDisconnectNotification
                                                 object:nil];
    }

    [self refreshWorkspaceStatus];
    [self refreshDockButtons];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    [super traitCollectionDidChange:previousTraitCollection];
    if (previousTraitCollection == nil)
        return;
    if (previousTraitCollection.horizontalSizeClass != self.traitCollection.horizontalSizeClass) {
        [self rebuildWorkspaceColumns];
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self applyCurrentThemeWallpaperIfNeededForced:NO];
    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        [self applyInitialFrameIfNeededToDesktopWindow:windowView];
        [self clampDesktopWindowToVisibleBounds:windowView];
        if (windowView.pinnedToBottomCenter)
            [self pinDesktopWindowToBottomCenter:windowView];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshWorkspaceStatus];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self applyCurrentThemeWallpaperIfNeededForced:NO];
    [self applyCompactSizingToOpenWorkspaceToolWindows];
    if (!self.didOpenInitialTool && self.initialToolIdentifier.length > 0) {
        self.didOpenInitialTool = YES;
        [self openWorkspaceToolWithIdentifier:self.initialToolIdentifier];
    }
    if (!self.didEnsureDefaultWorkspaceUtilities) {
        self.didEnsureDefaultWorkspaceUtilities = YES;
        [self ensureDefaultWorkspaceUtilitiesOpen];
    }
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
}

- (void)workspaceThemeDidChange:(__unused NSNotification *)notification {
    [self applyCurrentThemeWallpaperIfNeededForced:YES];
    [self applyCompactSizingToOpenWorkspaceToolWindows];
    [self refreshDockButtons];
}

- (void)refreshWorkspaceStatus {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self refreshWorkspaceStatus];
        });
        return;
    }

    [self refreshWindowSummary];
    [self refreshSceneWindows];
    [self refreshDockButtons];
}

- (void)refreshSceneWindows {
    for (UIView *subview in self.sceneWindowsStack.arrangedSubviews) {
        [self.sceneWindowsStack removeArrangedSubview:subview];
        [subview removeFromSuperview];
    }

    if (@available(iOS 13.0, *)) {
        UIWindowScene *currentWindowScene = self.view.window.windowScene;
        NSArray<UIScene *> *connectedScenes =
            [UIApplication.sharedApplication.connectedScenes.allObjects sortedArrayUsingComparator:^NSComparisonResult(UIScene *left, UIScene *right) {
            if (left == currentWindowScene)
                return NSOrderedAscending;
            if (right == currentWindowScene)
                return NSOrderedDescending;
            return [left.session.persistentIdentifier compare:right.session.persistentIdentifier];
        }];

        if (connectedScenes.count == 0) {
            UILabel *emptyLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
            emptyLabel.text = @"No live windows detected.";
            [self.sceneWindowsStack addArrangedSubview:emptyLabel];
            return;
        }

        for (UIScene *scene in connectedScenes) {
            UIStackView *row = [UIStackView new];
            row.axis = UILayoutConstraintAxisHorizontal;
            row.spacing = 10;
            row.alignment = UIStackViewAlignmentCenter;

            UILabel *label = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
            NSString *role = ISHWorkspaceSceneRoleDescription(scene.session);
            NSString *state = ISHWorkspaceSceneActivationDescription(scene);
            NSString *identifier = scene.session.persistentIdentifier ?: @"";
            if (identifier.length > 8)
                identifier = [identifier substringFromIndex:identifier.length - 8];

            NSMutableArray<NSString *> *parts = [NSMutableArray arrayWithObjects:role, state, nil];
            NSString *terminalUUID = scene.session.stateRestorationActivity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
            if (terminalUUID.length > 0) {
                Terminal *terminal = [Terminal terminalWithUUID:[[NSUUID alloc] initWithUUIDString:terminalUUID]];
                NSString *terminalLabel = terminal != nil ? ISHWorkspaceTerminalDisplayName(terminal) : @"Detached terminal";
                [parts addObject:terminalLabel];
            }
            NSString *currentMarker = scene == currentWindowScene ? @"Current window" : [NSString stringWithFormat:@"Scene %@", identifier];
            label.text = [NSString stringWithFormat:@"%@\n%@", currentMarker, [parts componentsJoinedByString:@"  |  "]];

            UIButton *focusButton = [UIButton buttonWithType:UIButtonTypeSystem];
            focusButton.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
            focusButton.accessibilityIdentifier = scene.session.persistentIdentifier;
            [focusButton setTitle:(scene == currentWindowScene ? @"Here" : @"Focus") forState:UIControlStateNormal];
            focusButton.enabled = scene != currentWindowScene;
            [focusButton addTarget:self action:@selector(focusExistingSceneFromButton:) forControlEvents:UIControlEventTouchUpInside];

            [row addArrangedSubview:label];
            [row addArrangedSubview:focusButton];
            [label setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
            [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
            [focusButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
            [self.sceneWindowsStack addArrangedSubview:row];
        }
        return;
    } else {
        UILabel *legacyLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
        legacyLabel.text = @"Live window enumeration requires iOS 13 scene APIs.";
        [self.sceneWindowsStack addArrangedSubview:legacyLabel];
        return;
    }
}

- (void)refreshWindowSummary {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSUInteger terminalCount = [Terminal activeTerminals].count;
    [lines addObject:[NSString stringWithFormat:@"Guest terminals: %lu", (unsigned long) terminalCount]];

    if (@available(iOS 13.0, *)) {
        NSUInteger workspaceSceneCount = 0;
        NSUInteger terminalSceneCount = 0;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIScene.class])
                continue;
            NSString *role = ISHWorkspaceSceneRoleDescription(scene.session);
            if ([role isEqualToString:@"Workspace"]) {
                workspaceSceneCount += 1;
            } else if ([role isEqualToString:@"Terminal"]) {
                terminalSceneCount += 1;
            }
        }
        [lines addObject:[NSString stringWithFormat:@"Workspace windows: %lu", (unsigned long) workspaceSceneCount]];
        [lines addObject:[NSString stringWithFormat:@"Terminal windows: %lu", (unsigned long) terminalSceneCount]];
        NSString *currentRole = self.view.window.windowScene != nil
            ? ISHWorkspaceSceneRoleDescription(self.view.window.windowScene.session)
            : @"Unknown";
        [lines addObject:[NSString stringWithFormat:@"Current window: %@", currentRole]];
    } else {
        [lines addObject:@"Workspace windows: 1"];
        [lines addObject:@"Terminal windows: 1"];
        [lines addObject:@"Current window: Workspace"];
    }

    self.windowSummaryLabel.text = [lines componentsJoinedByString:@"\n"];
}

- (void)refreshDockButtons {
    ISHWorkspaceContainedWindowView *frontmostWindow = [self frontmostDesktopWindowExcludingDock];

    ISHWorkspaceContainedWindowView *clockWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolClockIdentifier];
    ISHWorkspaceContainedWindowView *workspacesWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
    BOOL utilsFrontmost = frontmostWindow == clockWindow || frontmostWindow == workspacesWindow;
    BOOL utilsActive = clockWindow != nil || workspacesWindow != nil;
    NSString *utilsState = clockWindow != nil ? @"Clock" : (workspacesWindow != nil ? @"Scenes" : @"Menu");
    [self configureDockTileButton:self.dockUtilsButton
                            title:@"Utils"
                            state:utilsState
                           active:utilsActive
                        frontmost:utilsFrontmost];

    ISHWorkspaceContainedWindowView *shellWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSessionShell];
    ISHWorkspaceContainedWindowView *frontmostTerminalWindow = [self frontmostDesktopTerminalWindow];
    BOOL hasTerminalWindow = frontmostTerminalWindow != nil;
    BOOL frontmostIsTerminal = frontmostTerminalWindow != nil && frontmostWindow == frontmostTerminalWindow;
    NSString *terminalState = @"Shell";
    if (shellWindow != nil || hasTerminalWindow)
        terminalState = @"Active";
    [self configureDockTileButton:self.dockTerminalButton
                            title:@"Terminal"
                            state:terminalState
                           active:hasTerminalWindow
                        frontmost:frontmostIsTerminal];

    NSDictionary<NSString *, UIColor *> *theme = ISHWorkspaceThemeDescriptor();
    for (ISHWorkspaceContainedWindowView *windowView in self.desktopWindows.copy) {
        if (![windowView isKindOfClass:ISHWorkspaceContainedWindowView.class] || windowView.hidden)
            continue;
        BOOL activeWindow = windowView == self.desktopSurfaceView.subviews.lastObject;
        [windowView applyWorkspaceChromeTheme:theme active:activeWindow];
    }
}

- (void)applyCompactSizingToOpenWorkspaceToolWindows {
    for (ISHWorkspaceContainedWindowView *windowView in self.desktopWindows.copy) {
        NSString *toolIdentifier = windowView.workspaceToolIdentifier;
        if (toolIdentifier.length == 0)
            continue;
        if (!ISHWorkspaceUsesPhoneLayout() && [toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
            continue;

        CGSize targetSize = ISHWorkspacePreferredToolContentSize(toolIdentifier);
        CGRect currentFrame = windowView.frame;
        BOOL shouldShrinkWidth = CGRectGetWidth(currentFrame) > targetSize.width + 24.0;
        BOOL shouldShrinkHeight = CGRectGetHeight(currentFrame) > targetSize.height + 24.0;
        if (!shouldShrinkWidth && !shouldShrinkHeight)
            continue;

        CGSize resized = CGSizeMake(shouldShrinkWidth ? targetSize.width : CGRectGetWidth(currentFrame),
                                    shouldShrinkHeight ? targetSize.height : CGRectGetHeight(currentFrame));
        [self resizeDesktopWindow:windowView toSize:resized animated:NO];
    }
}

- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier {
    [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
}

- (void)ensureDefaultWorkspaceUtilitiesOpen {
    if ([self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier] != nil)
        return;
    ISHWorkspaceContainedWindowView *windowView =
        [self openWorkspaceToolWindowWithIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
    if (windowView != nil) {
        [self positionDesktopWindowAtTopRight:windowView];
        [self refreshDockButtons];
    }
}

- (void)openDiagnostics:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolDiagnosticsIdentifier];
}

- (void)toggleClockFromDock:(id)sender {
    ISHWorkspaceContainedWindowView *clockWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolClockIdentifier];
    if (clockWindow != nil) {
        if (clockWindow.closeHandler != nil)
            clockWindow.closeHandler();
        return;
    }
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolClockIdentifier];
}

- (void)openOrFocusWorkspaceToolFromDock:(UIButton *)sender {
    NSString *toolIdentifier = sender.accessibilityIdentifier;
    if (toolIdentifier.length == 0)
        return;

    [self openOrFocusWorkspaceToolIdentifier:toolIdentifier];
}

- (void)openOrFocusTerminalFromDock:(UIButton *)sender {
    (void) sender;
    [self openDesktopTerminalHerePreferringConsole:NO
                                     reuseExisting:NO
                                   trackPrimaryRole:NO];
}

- (void)openOrFocusWorkspaceToolIdentifier:(NSString *)toolIdentifier {
    if (toolIdentifier.length == 0)
        return;
    ISHWorkspaceContainedWindowView *existingWindow = [self desktopWindowForToolIdentifier:toolIdentifier];
    if (existingWindow != nil) {
        [self focusDesktopWindow:existingWindow];
        return;
    }
    [self openWorkspaceToolWithIdentifier:toolIdentifier];
}

- (NSArray<NSDictionary<NSString *, id> *> *)dockUtilityGroupDescriptors {
    return @[
        @{
            @"title": @"Workspace",
            @"message": @"Launchers and workspace-wide controls.",
            @"items": @[
                @{@"title": @"Dashboard", @"identifier": @"dashboard"},
                @{@"title": @"Workspaces", @"identifier": ISHWorkspaceToolWorkspacesIdentifier},
                @{@"title": @"Shortcuts", @"identifier": ISHWorkspaceToolShortcutsIdentifier},
                @{@"title": @"Sessions", @"identifier": ISHWorkspaceToolSessionsIdentifier},
                @{@"title": @"Themes", @"identifier": ISHWorkspaceToolThemesIdentifier},
            ],
        },
        @{
            @"title": @"Status",
            @"message": @"Live clocks, runtime summaries, and process views.",
            @"items": @[
                @{@"title": @"Clock", @"identifier": ISHWorkspaceToolClockIdentifier},
                @{@"title": @"Info", @"identifier": ISHWorkspaceToolInfoIdentifier},
                @{@"title": @"Monitor", @"identifier": ISHWorkspaceToolMonitorIdentifier},
                @{@"title": @"Processes", @"identifier": ISHWorkspaceToolProcessesIdentifier},
                @{@"title": @"Networks", @"identifier": ISHWorkspaceToolNetworksIdentifier},
                @{@"title": @"System Status", @"identifier": ISHWorkspaceToolStatusIdentifier},
            ],
        },
        @{
            @"title": @"Storage",
            @"message": @"Roots, filesystem summaries, and storage detail.",
            @"items": @[
                @{@"title": @"Storage", @"identifier": ISHWorkspaceToolStorageIdentifier},
                @{@"title": @"Filesystems", @"identifier": ISHWorkspaceToolFilesystemsIdentifier},
            ],
        },
        @{
            @"title": @"Support",
            @"message": @"Settings and diagnostics tools.",
            @"items": @[
                @{@"title": @"Settings", @"identifier": ISHWorkspaceToolSettingsIdentifier},
                @{@"title": @"Diagnostics", @"identifier": ISHWorkspaceToolDiagnosticsIdentifier},
            ],
        },
    ];
}

- (void)handleUtilsDockLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentUtilsDockActionsFromView:recognizer.view];
}

- (void)presentUtilityGroup:(NSDictionary<NSString *, id> *)groupDescriptor fromView:(UIView *)sourceView {
    NSString *groupTitle = groupDescriptor[@"title"] ?: @"Utils";
    NSString *groupMessage = groupDescriptor[@"message"];
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:groupTitle
                                            message:groupMessage
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSDictionary<NSString *, NSString *> *descriptor in groupDescriptor[@"items"]) {
        NSString *toolIdentifier = descriptor[@"identifier"];
        NSString *title = descriptor[@"title"];
        BOOL isDashboardDescriptor = [toolIdentifier isEqualToString:@"dashboard"];
        ISHWorkspaceContainedWindowView *existingWindow = isDashboardDescriptor
            ? (self.dashboardWindow.hidden ? nil : self.dashboardWindow)
            : [self desktopWindowForToolIdentifier:toolIdentifier];
        NSString *actionTitle = existingWindow != nil
            ? [NSString stringWithFormat:@"Focus %@", title]
            : [NSString stringWithFormat:@"Open %@", title];
        [sheet addAction:[UIAlertAction actionWithTitle:actionTitle
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            if (existingWindow != nil) {
                [self focusDesktopWindow:existingWindow];
            } else if (isDashboardDescriptor) {
                [self openDashboardWindow:nil];
            } else {
                [self openWorkspaceToolWithIdentifier:toolIdentifier];
            }
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Back"
                                              style:UIAlertActionStyleCancel
                                            handler:^(__unused UIAlertAction *action) {
        [self presentUtilsDockActionsFromView:sourceView];
    }]];

    UIPopoverPresentationController *popoverPresentationController = sheet.popoverPresentationController;
    if (popoverPresentationController != nil) {
        popoverPresentationController.sourceView = sourceView ?: self.dockUtilsButton;
        popoverPresentationController.sourceRect = sourceView != nil ? sourceView.bounds : self.dockUtilsButton.bounds;
        popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)presentUtilsDockActionsFromView:(UIView *)sourceView {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Utils"
                                            message:@"Choose a utility group."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSDictionary<NSString *, id> *groupDescriptor in [self dockUtilityGroupDescriptors]) {
        NSString *title = groupDescriptor[@"title"] ?: @"Group";
        NSArray *items = groupDescriptor[@"items"];
        NSString *actionTitle = items.count > 0
            ? [NSString stringWithFormat:@"%@ (%lu)", title, (unsigned long) items.count]
            : title;
        [sheet addAction:[UIAlertAction actionWithTitle:actionTitle
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self presentUtilityGroup:groupDescriptor fromView:sourceView];
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popoverPresentationController = sheet.popoverPresentationController;
    if (popoverPresentationController != nil) {
        popoverPresentationController.sourceView = sourceView ?: self.dockUtilsButton;
        popoverPresentationController.sourceRect = sourceView != nil ? sourceView.bounds : self.dockUtilsButton.bounds;
        popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)handleTerminalDockLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentTerminalDockActionsFromView:recognizer.view];
}

- (void)presentTerminalDockActionsFromView:(UIView *)sourceView {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Terminal"
                                            message:@"Open or focus shell, console, or another active terminal."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    ISHWorkspaceContainedWindowView *primaryShellWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSessionShell];
    NSString *primaryActionTitle = primaryShellWindow != nil ? @"Focus Session Shell" : @"Open Session Shell";
    [sheet addAction:[UIAlertAction actionWithTitle:primaryActionTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openTerminalHerePreferringConsole:NO];
    }]];

    ISHWorkspaceContainedWindowView *primaryConsoleWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSystemConsole];
    NSString *consoleActionTitle = primaryConsoleWindow != nil ? @"Focus System Console" : @"Open System Console";
    [sheet addAction:[UIAlertAction actionWithTitle:consoleActionTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openTerminalHerePreferringConsole:YES];
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:@"Open Another Shell Window"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openDesktopTerminalHerePreferringConsole:NO
                                         reuseExisting:NO
                                       trackPrimaryRole:NO];
    }]];

    NSUUID *primaryTerminalUUID = primaryShellWindow.hostedTerminalViewController.terminal.uuid;
    for (Terminal *terminal in [Terminal activeTerminals]) {
        if (primaryTerminalUUID != nil && [terminal.uuid isEqual:primaryTerminalUUID])
            continue;
        NSString *title = [NSString stringWithFormat:@"Focus %@", ISHWorkspaceTerminalDisplayName(terminal)];
        [sheet addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self openExistingTerminalHereWithUUID:terminal.uuid];
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popoverPresentationController = sheet.popoverPresentationController;
    if (popoverPresentationController != nil) {
        popoverPresentationController.sourceView = sourceView ?: self.dockTerminalButton;
        popoverPresentationController.sourceRect = sourceView != nil ? sourceView.bounds : self.dockTerminalButton.bounds;
        popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)openDesktopTerminalHerePreferringConsole:(BOOL)preferConsole
                                  reuseExisting:(BOOL)reuseExisting
                                trackPrimaryRole:(BOOL)trackPrimaryRole {
    NSString *terminalRole = preferConsole ? ISHWorkspaceTerminalRoleSystemConsole : ISHWorkspaceTerminalRoleSessionShell;
    if (reuseExisting) {
        ISHWorkspaceContainedWindowView *existingWindow = [self desktopWindowForTerminalRole:terminalRole];
        if (existingWindow != nil) {
            [self focusDesktopWindow:existingWindow];
            return;
        }
    }

    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    terminalViewController.freshSessionTerminalDisplayMode =
        preferConsole ? ISHFreshSessionTerminalDisplayModeSystemConsole
                      : ISHFreshSessionTerminalDisplayModeSessionShell;

    NSString *title = preferConsole ? @"System Console" : @"Session Shell";
    ISHWorkspaceContainedWindowView *windowView =
        [self openDesktopTerminalWindowWithTitle:title terminalViewController:terminalViewController];
    [terminalViewController startNewSession];
    if (preferConsole) {
        [terminalViewController showSystemConsoleForCurrentSession];
    } else {
        [terminalViewController showSessionShellForCurrentSession];
    }

    if (trackPrimaryRole) {
        windowView.workspaceTerminalRole = terminalRole;
        windowView.titleLabel.text = ISHWorkspaceTitleForTerminalRole(terminalRole, terminalViewController.terminal);
    } else {
        windowView.workspaceTerminalRole = ISHWorkspaceTerminalRoleGeneric;
        windowView.titleLabel.text = title;
    }
    [self refreshDockButtons];
}

- (void)openTerminalHerePreferringConsole:(BOOL)preferConsole {
    [self openDesktopTerminalHerePreferringConsole:preferConsole
                                     reuseExisting:YES
                                   trackPrimaryRole:YES];
}

- (UISceneSession *)sceneSessionHostingTerminalUUID:(NSUUID *)terminalUUID {
    NSString *terminalUUIDString = terminalUUID.UUIDString;
    if (terminalUUIDString.length == 0)
        return nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        NSString *sceneTerminalUUID = scene.session.stateRestorationActivity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
        if ([sceneTerminalUUID isEqualToString:terminalUUIDString])
            return scene.session;
    }
    return nil;
}

- (BOOL)focusSceneSession:(UISceneSession *)sceneSession title:(NSString *)title {
    if (sceneSession == nil)
        return NO;
    [UIApplication.sharedApplication requestSceneSessionActivation:sceneSession
                                                     userActivity:nil
                                                          options:nil
                                                     errorHandler:^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentSceneActivationError:error title:title];
        });
    }];
    return YES;
}

- (void)focusSceneWithPersistentIdentifier:(NSString *)identifier {
    if (@available(iOS 13.0, *)) {
    } else {
        return;
    }
    if (identifier.length == 0) {
        [self presentSceneActivationError:nil title:@"Unable to focus window"];
        return;
    }
    UISceneSession *targetSession = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene.session.persistentIdentifier isEqualToString:identifier]) {
            targetSession = scene.session;
            break;
        }
    }
    if (targetSession == nil) {
        [self presentSceneActivationError:nil title:@"Unable to focus window"];
        return;
    }
    [UIApplication.sharedApplication requestSceneSessionActivation:targetSession
                                                     userActivity:nil
                                                          options:nil
                                                     errorHandler:^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentSceneActivationError:error title:@"Unable to focus window"];
        });
    }];
}

- (void)focusExistingSceneFromButton:(UIButton *)sender {
    if (@available(iOS 13.0, *)) {
        [self focusSceneWithPersistentIdentifier:sender.accessibilityIdentifier];
    } else {
        [self presentSceneActivationError:nil title:@"Unable to focus window"];
    }
}

- (void)openExistingTerminalHereWithUUID:(NSUUID *)terminalUUID {
    Terminal *terminal = [Terminal terminalWithUUID:terminalUUID];
    if (terminal == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    if (@available(iOS 13.0, *)) {
        UISceneSession *existingSession = [self sceneSessionHostingTerminalUUID:terminalUUID];
        UISceneSession *currentSession = self.view.window.windowScene.session;
        if (existingSession != nil && existingSession != currentSession) {
            [self focusSceneSession:existingSession title:@"Unable to focus terminal window"];
            return;
        }
    }
    ISHWorkspaceContainedWindowView *containedWindow = [self desktopWindowHostingTerminalUUID:terminalUUID];
    if (containedWindow != nil) {
        [self focusDesktopWindow:containedWindow];
        return;
    }
    if (terminal.webView.superview != nil) {
        [self presentSceneActivationError:nil title:@"Terminal already open in another window"];
        return;
    }

    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    [self openDesktopTerminalWindowWithTitle:ISHWorkspaceTerminalDisplayName(terminal)
                      terminalViewController:terminalViewController];
    [terminalViewController reconnectSessionFromTerminalUUID:terminalUUID];
    [self refreshDockButtons];
}

- (void)openNewWorkspaceWindow:(id)sender {
    [self requestSceneWithActivityType:ISHSceneActivityTypeWorkspace
                               title:@"Unable to open workspace"
                            userInfo:nil];
}

- (void)requestSceneWithActivityType:(NSString *)activityType
                               title:(NSString *)title
                            userInfo:(NSDictionary<NSString *, id> *)userInfo {
    if (@available(iOS 13.0, *)) {
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:activityType];
        if (userInfo.count > 0)
            activity.userInfo = userInfo;
        [UIApplication.sharedApplication requestSceneSessionActivation:nil
                                                         userActivity:activity
                                                              options:nil
                                                         errorHandler:^(NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentSceneActivationError:error title:title];
            });
        }];
        return;
    }

    [self presentSceneActivationError:nil title:title];
}

- (void)presentSceneActivationError:(NSError *)error {
    [self presentSceneActivationError:error title:@"Unable to open terminal"];
}

- (void)presentSceneActivationError:(NSError *)error title:(NSString *)title {
    NSString *message = @"This device cannot open a separate terminal scene right now.";
    if (error.localizedDescription.length > 0) {
        message = error.localizedDescription;
    }
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:title
                                            message:message
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

@implementation WorkspaceThemedToolViewController {
    CAGradientLayer *_backgroundGradientLayer;
    UIView *_toolContentView;
    NSMutableArray<UIView *> *_trackedCardViews;
    NSMutableArray<UILabel *> *_trackedPrimaryLabels;
    NSMutableArray<UILabel *> *_trackedSecondaryLabels;
    NSMutableArray<UILabel *> *_trackedAccentLabels;
    NSMutableArray<UITextView *> *_trackedTextViews;
    NSMutableArray<UIProgressView *> *_trackedProgressViews;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    _trackedCardViews = [NSMutableArray array];
    _trackedPrimaryLabels = [NSMutableArray array];
    _trackedSecondaryLabels = [NSMutableArray array];
    _trackedAccentLabels = [NSMutableArray array];
    _trackedTextViews = [NSMutableArray array];
    _trackedProgressViews = [NSMutableArray array];

    self.view.clipsToBounds = YES;
    self.view.backgroundColor = UIColor.blackColor;

    _backgroundGradientLayer = [CAGradientLayer layer];
    _backgroundGradientLayer.startPoint = CGPointMake(0.12, 0.0);
    _backgroundGradientLayer.endPoint = CGPointMake(0.88, 1.0);
    [self.view.layer insertSublayer:_backgroundGradientLayer atIndex:0];

    _toolContentView = [UIView new];
    _toolContentView.translatesAutoresizingMaskIntoConstraints = NO;
    _toolContentView.backgroundColor = UIColor.clearColor;
    [self.view addSubview:_toolContentView];

    [NSLayoutConstraint activateConstraints:@[
        [_toolContentView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [_toolContentView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [_toolContentView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [_toolContentView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(workspaceThemeDidChange:)
                                               name:ISHWorkspaceToolThemeDidChangeNotification
                                             object:nil];
    [self workspaceApplyTheme];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:ISHWorkspaceToolThemeDidChangeNotification
                                                object:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _backgroundGradientLayer.frame = self.view.bounds;
}

- (UIView *)toolContentView {
    return _toolContentView;
}

- (NSDictionary<NSString *, UIColor *> *)workspaceTheme {
    return ISHWorkspaceThemeDescriptor();
}

- (UILabel *)workspaceThemeLabelWithTextStyle:(UIFontTextStyle)textStyle
                                   monospaced:(BOOL)monospaced
                                      tracker:(NSMutableArray<UILabel *> *)tracker {
    UILabel *label = [UILabel new];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.numberOfLines = 0;
    CGFloat pointSize = ISHWorkspaceThemeFontSize(textStyle);
    UIFont *font = [UIFont systemFontOfSize:pointSize];
    if (monospaced) {
        if (@available(iOS 13.0, *)) {
            font = [UIFont monospacedSystemFontOfSize:pointSize weight:UIFontWeightMedium];
        } else {
            font = [UIFont fontWithName:@"Menlo-Regular" size:pointSize] ?: font;
        }
    }
    label.font = font;
    [tracker addObject:label];
    return label;
}

- (UILabel *)workspaceThemePrimaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    return [self workspaceThemeLabelWithTextStyle:textStyle monospaced:monospaced tracker:_trackedPrimaryLabels];
}

- (UILabel *)workspaceThemeSecondaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    return [self workspaceThemeLabelWithTextStyle:textStyle monospaced:monospaced tracker:_trackedSecondaryLabels];
}

- (UILabel *)workspaceThemeAccentLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    return [self workspaceThemeLabelWithTextStyle:textStyle monospaced:monospaced tracker:_trackedAccentLabels];
}

- (UIView *)workspaceThemeCardView {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    CGFloat compactCornerRadius = ISHWorkspaceUsesPhoneLayout() ? 10.0 : 14.0;
    CGFloat roomyCornerRadius = ISHWorkspaceUsesPhoneLayout() ? 16.0 : 22.0;
    card.layer.cornerRadius = ISHWorkspaceDensityValue(compactCornerRadius, roomyCornerRadius);
    card.layer.borderWidth = 1;
    card.layer.shadowColor = UIColor.blackColor.CGColor;
    card.layer.shadowOpacity = 0.16;
    card.layer.shadowRadius = ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 7.0 : 10.0,
                                                       ISHWorkspaceUsesPhoneLayout() ? 12.0 : 18.0);
    card.layer.shadowOffset = CGSizeMake(0, ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 4.0 : 6.0,
                                                                     ISHWorkspaceUsesPhoneLayout() ? 7.0 : 10.0));
    [_trackedCardViews addObject:card];
    return card;
}

- (UITextView *)workspaceThemeTextView {
    UITextView *textView = [UITextView new];
    textView.translatesAutoresizingMaskIntoConstraints = NO;
    textView.editable = NO;
    textView.alwaysBounceVertical = YES;
    textView.backgroundColor = UIColor.clearColor;
    CGFloat pointSize = ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 8.5 : 9.5,
                                                 ISHWorkspaceUsesPhoneLayout() ? 11.0 : 12.0);
    if (@available(iOS 13.0, *)) {
        textView.font = [UIFont monospacedSystemFontOfSize:pointSize weight:UIFontWeightRegular];
    } else {
        textView.font = [UIFont fontWithName:@"Menlo-Regular" size:pointSize] ?: [UIFont systemFontOfSize:pointSize];
    }
    [_trackedTextViews addObject:textView];
    return textView;
}

- (UIProgressView *)workspaceThemeProgressView {
    UIProgressView *progressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    progressView.translatesAutoresizingMaskIntoConstraints = NO;
    progressView.transform = CGAffineTransformMakeScale(1.0,
                                                        ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 0.98 : 1.04,
                                                                                 ISHWorkspaceUsesPhoneLayout() ? 1.16 : 1.28));
    [_trackedProgressViews addObject:progressView];
    return progressView;
}

- (void)workspaceThemeDidChange:(__unused NSNotification *)notification {
    [self workspaceApplyTheme];
}

- (void)workspaceApplyTheme {
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _backgroundGradientLayer.colors = @[
        (id) theme[@"backgroundTop"].CGColor,
        (id) theme[@"backgroundBottom"].CGColor,
    ];

    for (UIView *card in _trackedCardViews) {
        card.layer.cornerRadius = ISHWorkspaceDensityValue(14, 22);
        card.layer.shadowRadius = ISHWorkspaceDensityValue(10, 18);
        card.layer.shadowOffset = CGSizeMake(0, ISHWorkspaceDensityValue(6, 10));
        card.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.95];
        card.layer.borderColor = theme[@"stroke"].CGColor;
    }
    for (UILabel *label in _trackedPrimaryLabels) {
        label.textColor = theme[@"primary"];
    }
    for (UILabel *label in _trackedSecondaryLabels) {
        label.textColor = theme[@"secondary"];
    }
    for (UILabel *label in _trackedAccentLabels) {
        label.textColor = theme[@"accent"];
    }
    for (UITextView *textView in _trackedTextViews) {
        CGFloat pointSize = ISHWorkspaceDensityValue(9.5, 12.0);
        if (@available(iOS 13.0, *)) {
            textView.font = [UIFont monospacedSystemFontOfSize:pointSize weight:UIFontWeightRegular];
        } else {
            textView.font = [UIFont fontWithName:@"Menlo-Regular" size:pointSize] ?: [UIFont systemFontOfSize:pointSize];
        }
        textView.textColor = theme[@"primary"];
        textView.tintColor = theme[@"accent"];
    }
    for (NSUInteger index = 0; index < _trackedProgressViews.count; index++) {
        UIProgressView *progressView = _trackedProgressViews[index];
        progressView.trackTintColor = [theme[@"accentAlt"] colorWithAlphaComponent:0.18];
        progressView.progressTintColor = index % 2 == 0 ? theme[@"accent"] : theme[@"accentAlt"];
    }
}

@end

@implementation WorkspaceThemesToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UIStackView *_themeListStack;
    UILabel *_activeThemeLabel;
    UILabel *_editorThemeLabel;
    UIView *_previewSurfaceView;
    CAGradientLayer *_previewGradientLayer;
    UILabel *_previewTitleLabel;
    UILabel *_previewBodyLabel;
    UIProgressView *_previewProgressView;
    UIImageView *_backgroundPreviewImageView;
    UILabel *_densityValueLabel;
    UISlider *_densitySlider;
    NSMutableDictionary<NSString *, UIView *> *_swatchViewsByKey;
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, UISlider *> *> *_channelSlidersByKey;
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, UILabel *> *> *_channelValueLabelsByKey;
    NSMutableDictionary<NSString *, UIImageView *> *_themePreviewImageViewsByIdentifier;
    NSMutableDictionary<NSString *, UILabel *> *_themeTitleLabelsByIdentifier;
    NSMutableDictionary<NSString *, UILabel *> *_themeDetailLabelsByIdentifier;
    NSMutableArray<UIButton *> *_themeSelectionButtons;
    NSMutableArray<UIButton *> *_editorActionButtons;
    NSString *_editingThemeIdentifier;
}

- (NSString *)themeEditorTitleForKey:(NSString *)key {
    if ([key isEqualToString:@"backgroundTop"])
        return @"Background Top";
    if ([key isEqualToString:@"backgroundBottom"])
        return @"Background Bottom";
    if ([key isEqualToString:@"card"])
        return @"Card Surface";
    if ([key isEqualToString:@"primary"])
        return @"Primary Text";
    if ([key isEqualToString:@"secondary"])
        return @"Secondary Text";
    if ([key isEqualToString:@"accent"])
        return @"Accent";
    if ([key isEqualToString:@"accentAlt"])
        return @"Accent Alt";
    return key;
}

- (NSMutableDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *)mutablePaletteForIdentifier:(NSString *)identifier {
    NSMutableDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette =
        [[ISHWorkspaceThemeEditablePaletteForIdentifier(identifier) mutableCopy] ?: @{}.mutableCopy mutableCopy];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSDictionary<NSString *, NSNumber *> *descriptor = palette[key];
        if (![descriptor isKindOfClass:NSDictionary.class]) {
            descriptor = ISHWorkspaceBuiltInThemePalette(ISHWorkspaceToolThemeAuroraIdentifier)[key];
        }
        if (descriptor != nil)
            palette[key] = [descriptor copy];
    }
    return palette;
}

- (NSMutableDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *)draftPalette {
    NSMutableDictionary *palette = [NSMutableDictionary dictionary];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSMutableDictionary<NSString *, UISlider *> *channels = _channelSlidersByKey[key];
        if (channels == nil)
            continue;
        palette[key] = @{
            @"red": @((NSInteger) lround(channels[@"red"].value)),
            @"green": @((NSInteger) lround(channels[@"green"].value)),
            @"blue": @((NSInteger) lround(channels[@"blue"].value)),
        };
    }
    return palette;
}

- (UIButton *)themeUtilityButtonWithTitle:(NSString *)title selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.layer.cornerRadius = 10;
    button.layer.borderWidth = 1;
    button.contentEdgeInsets = UIEdgeInsetsMake(7, 10, 7, 10);
    button.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1) weight:UIFontWeightSemibold];
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    [_editorActionButtons addObject:button];
    return button;
}

- (UIButton *)themeSelectionButtonWithTitle:(NSString *)title identifier:(NSString *)identifier {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeCustom];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;
    button.clipsToBounds = NO;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:68].active = YES;
    button.accessibilityIdentifier = identifier;
    [button addTarget:self action:@selector(selectThemeFromButton:) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *contentRow = [UIStackView new];
    contentRow.translatesAutoresizingMaskIntoConstraints = NO;
    contentRow.axis = UILayoutConstraintAxisHorizontal;
    contentRow.spacing = 8;
    contentRow.alignment = UIStackViewAlignmentCenter;
    contentRow.userInteractionEnabled = NO;
    [button addSubview:contentRow];

    UIImageView *previewView = [[UIImageView alloc] initWithImage:
        ISHWorkspaceThemeArtworkImage(identifier,
                                      ISHWorkspaceThemeEditablePaletteForIdentifier(identifier),
                                      CGSizeMake(92, 52),
                                      NO)];
    previewView.translatesAutoresizingMaskIntoConstraints = NO;
    previewView.contentMode = UIViewContentModeScaleAspectFill;
    previewView.clipsToBounds = YES;
    previewView.layer.cornerRadius = 8;
    previewView.layer.borderWidth = 1;
    [previewView.widthAnchor constraintEqualToConstant:92].active = YES;
    [previewView.heightAnchor constraintEqualToConstant:52].active = YES;
    [contentRow addArrangedSubview:previewView];

    UIStackView *textStack = [UIStackView new];
    textStack.axis = UILayoutConstraintAxisVertical;
    textStack.spacing = 2;
    textStack.alignment = UIStackViewAlignmentLeading;
    textStack.userInteractionEnabled = NO;
    UILabel *titleLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline) weight:UIFontWeightSemibold];
    titleLabel.numberOfLines = 1;
    titleLabel.text = title;
    UILabel *detailLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    detailLabel.numberOfLines = 2;
    [textStack addArrangedSubview:titleLabel];
    [textStack addArrangedSubview:detailLabel];
    [contentRow addArrangedSubview:textStack];

    [NSLayoutConstraint activateConstraints:@[
        [contentRow.topAnchor constraintEqualToAnchor:button.topAnchor constant:7],
        [contentRow.leadingAnchor constraintEqualToAnchor:button.leadingAnchor constant:8],
        [contentRow.trailingAnchor constraintEqualToAnchor:button.trailingAnchor constant:-8],
        [contentRow.bottomAnchor constraintEqualToAnchor:button.bottomAnchor constant:-7],
    ]];

    _themePreviewImageViewsByIdentifier[identifier] = previewView;
    _themeTitleLabelsByIdentifier[identifier] = titleLabel;
    _themeDetailLabelsByIdentifier[identifier] = detailLabel;
    [_themeSelectionButtons addObject:button];
    return button;
}

- (UIView *)sliderRowWithTitle:(NSString *)title
                         key:(NSString *)key {
    UIView *card = [self workspaceThemeCardView];

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 3;
    [card addSubview:stack];

    UIStackView *headerRow = [UIStackView new];
    headerRow.axis = UILayoutConstraintAxisHorizontal;
    headerRow.spacing = 8;
    headerRow.alignment = UIStackViewAlignmentCenter;

    UILabel *titleLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    titleLabel.text = title;
    UIView *swatch = [UIView new];
    swatch.translatesAutoresizingMaskIntoConstraints = NO;
    swatch.layer.cornerRadius = 8;
    swatch.layer.borderWidth = 1;
    [_swatchViewsByKey setObject:swatch forKey:key];
    [swatch.widthAnchor constraintEqualToConstant:34].active = YES;
    [swatch.heightAnchor constraintEqualToConstant:18].active = YES;
    [headerRow addArrangedSubview:titleLabel];
    [headerRow addArrangedSubview:swatch];
    [titleLabel setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [swatch setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

    [stack addArrangedSubview:headerRow];

    NSMutableDictionary<NSString *, UISlider *> *channels = [NSMutableDictionary dictionary];
    NSArray<NSDictionary<NSString *, NSString *> *> *channelDescriptors = @[
        @{@"name": @"R", @"key": @"red"},
        @{@"name": @"G", @"key": @"green"},
        @{@"name": @"B", @"key": @"blue"},
    ];
    for (NSDictionary<NSString *, NSString *> *channelDescriptor in channelDescriptors) {
        UIStackView *row = [UIStackView new];
        row.axis = UILayoutConstraintAxisHorizontal;
        row.spacing = 6;
        row.alignment = UIStackViewAlignmentCenter;

        UILabel *channelLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:YES];
        channelLabel.text = channelDescriptor[@"name"];
        channelLabel.textAlignment = NSTextAlignmentCenter;
        [channelLabel.widthAnchor constraintEqualToConstant:16].active = YES;

        UISlider *slider = [UISlider new];
        slider.minimumValue = 0.0f;
        slider.maximumValue = 255.0f;
        slider.accessibilityIdentifier = [NSString stringWithFormat:@"%@:%@", key, channelDescriptor[@"key"]];
        [slider addTarget:self action:@selector(themeSliderChanged:) forControlEvents:UIControlEventValueChanged];

        UILabel *valueLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:YES];
        valueLabel.textAlignment = NSTextAlignmentRight;
        [valueLabel.widthAnchor constraintEqualToConstant:28].active = YES;

        [row addArrangedSubview:channelLabel];
        [row addArrangedSubview:slider];
        [row addArrangedSubview:valueLabel];
        [channelLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [valueLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [stack addArrangedSubview:row];
        channels[channelDescriptor[@"key"]] = slider;
        if (_channelValueLabelsByKey[key] == nil)
            _channelValueLabelsByKey[key] = [NSMutableDictionary dictionary];
        _channelValueLabelsByKey[key][channelDescriptor[@"key"]] = valueLabel;
    }
    _channelSlidersByKey[key] = channels;

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:10],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-10],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-10],
    ]];
    return card;
}

- (void)updateSliderValueLabels {
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSDictionary<NSString *, UISlider *> *channels = _channelSlidersByKey[key];
        for (NSString *channel in channels) {
            UILabel *label = _channelValueLabelsByKey[key][channel];
            label.text = [NSString stringWithFormat:@"%ld", (long) lround(channels[channel].value)];
        }
    }
}

- (void)clearArrangedSubviewsFromStack:(UIStackView *)stackView {
    NSArray<UIView *> *arrangedSubviews = stackView.arrangedSubviews.copy;
    for (UIView *view in arrangedSubviews) {
        [stackView removeArrangedSubview:view];
        [view removeFromSuperview];
    }
}

- (void)loadThemeIntoEditorWithIdentifier:(NSString *)identifier {
    _editingThemeIdentifier = identifier;
    NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette = [self mutablePaletteForIdentifier:identifier];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSDictionary<NSString *, NSNumber *> *descriptor = palette[key];
        NSDictionary<NSString *, UISlider *> *channels = _channelSlidersByKey[key];
        channels[@"red"].value = [descriptor[@"red"] floatValue];
        channels[@"green"].value = [descriptor[@"green"] floatValue];
        channels[@"blue"].value = [descriptor[@"blue"] floatValue];
    }
    [self updateSliderValueLabels];
    [self updateDraftPreview];
    [self refreshThemeSelectionButtons];
}

- (void)refreshThemeSelectionButtons {
    [self clearArrangedSubviewsFromStack:_themeListStack];
    [_themeSelectionButtons removeAllObjects];
    [_themePreviewImageViewsByIdentifier removeAllObjects];
    [_themeTitleLabelsByIdentifier removeAllObjects];
    [_themeDetailLabelsByIdentifier removeAllObjects];

    NSString *currentIdentifier = ISHWorkspaceCurrentThemeIdentifier();
    for (NSDictionary<NSString *, id> *choice in ISHWorkspaceThemeChoices()) {
        NSString *identifier = choice[@"identifier"];
        NSString *title = choice[@"title"];
        NSString *detail = [choice[@"builtIn"] boolValue] ? @"Built-in theme" : @"Saved custom theme";
        UIButton *button = [self themeSelectionButtonWithTitle:title identifier:identifier];
        UILabel *detailLabel = _themeDetailLabelsByIdentifier[identifier];
        detailLabel.text = [identifier isEqualToString:currentIdentifier]
            ? [NSString stringWithFormat:@"Applied • %@", detail]
            : detail;
        [_themeListStack addArrangedSubview:button];
    }
    [self workspaceApplyTheme];
}

- (void)updateDraftPreview {
    NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette = [self draftPalette];
    NSDictionary<NSString *, UIColor *> *previewTheme = ISHWorkspaceThemeDescriptorFromEditablePalette(palette);
    _previewGradientLayer.colors = @[
        (id) previewTheme[@"backgroundTop"].CGColor,
        (id) previewTheme[@"backgroundBottom"].CGColor,
    ];
    _previewSurfaceView.backgroundColor = [previewTheme[@"card"] colorWithAlphaComponent:0.96];
    _previewSurfaceView.layer.borderColor = previewTheme[@"stroke"].CGColor;
    _previewTitleLabel.textColor = previewTheme[@"accent"];
    _previewBodyLabel.textColor = previewTheme[@"primary"];
    _previewProgressView.trackTintColor = [previewTheme[@"accentAlt"] colorWithAlphaComponent:0.18];
    _previewProgressView.progressTintColor = previewTheme[@"accentAlt"];
    _previewProgressView.progress = 0.72f;
    _backgroundPreviewImageView.image = ISHWorkspaceThemeArtworkImage(_editingThemeIdentifier,
                                                                      palette,
                                                                      CGSizeMake(1200, 675),
                                                                      YES);
    _backgroundPreviewImageView.layer.borderColor = previewTheme[@"stroke"].CGColor;
    _densityValueLabel.text = ISHWorkspaceCurrentDensityTitle();

    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        UIView *swatch = _swatchViewsByKey[key];
        UIColor *color = ISHWorkspaceThemeColorFromDescriptor(palette[key]);
        swatch.backgroundColor = color;
        swatch.layer.borderColor = [previewTheme[@"stroke"] colorWithAlphaComponent:0.9].CGColor;
    }

    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier);
    NSString *title = record[@"title"];
    if (title.length == 0)
        title = @"Draft Theme";
    _editorThemeLabel.text = [NSString stringWithFormat:@"Editing: %@", title];
}

- (void)themeSliderChanged:(UISlider *)sender {
    sender.value = roundf(sender.value);
    [self updateSliderValueLabels];
    [self updateDraftPreview];
}

- (void)selectThemeFromButton:(UIButton *)sender {
    NSString *identifier = sender.accessibilityIdentifier;
    if (identifier.length == 0)
        return;
    ISHWorkspaceSetCurrentThemeIdentifier(identifier);
    [self loadThemeIntoEditorWithIdentifier:identifier];
}

- (void)saveThemeAsNew:(id)sender {
    (void) sender;
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Save Theme"
                                            message:@"Save the current editor palette as a custom Workspace theme."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Theme name";
        textField.text = @"Custom Theme";
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSString *title = alert.textFields.firstObject.text ?: @"";
        title = [title stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (title.length == 0)
            title = @"Custom Theme";
        NSString *identifier = ISHWorkspaceCreateCustomThemeIdentifier();
        ISHWorkspaceSaveCustomThemeRecord(identifier, title, [self draftPalette]);
        ISHWorkspaceSetCurrentThemeIdentifier(identifier);
        [self loadThemeIntoEditorWithIdentifier:identifier];
        [self refreshThemeSelectionButtons];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)updateSelectedCustomTheme:(id)sender {
    (void) sender;
    if (ISHWorkspaceThemeIdentifierIsBuiltIn(_editingThemeIdentifier))
        return;
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier);
    NSString *title = record[@"title"] ?: @"Custom Theme";
    ISHWorkspaceSaveCustomThemeRecord(_editingThemeIdentifier, title, [self draftPalette]);
    ISHWorkspaceSetCurrentThemeIdentifier(_editingThemeIdentifier);
    [self refreshThemeSelectionButtons];
    [self loadThemeIntoEditorWithIdentifier:_editingThemeIdentifier];
}

- (void)deleteSelectedCustomTheme:(id)sender {
    (void) sender;
    if (ISHWorkspaceThemeIdentifierIsBuiltIn(_editingThemeIdentifier))
        return;
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier);
    NSString *title = record[@"title"] ?: @"this theme";
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Delete Theme?"
                                            message:[NSString stringWithFormat:@"Remove %@ from saved custom themes.", title]
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete"
                                              style:UIAlertActionStyleDestructive
                                            handler:^(__unused UIAlertAction *action) {
        ISHWorkspaceDeleteCustomThemeRecord(self->_editingThemeIdentifier);
        ISHWorkspaceSetCurrentThemeIdentifier(ISHWorkspaceToolThemeAuroraIdentifier);
        [self loadThemeIntoEditorWithIdentifier:ISHWorkspaceCurrentThemeIdentifier()];
        [self refreshThemeSelectionButtons];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)densitySliderChanged:(UISlider *)sender {
    sender.value = roundf(sender.value * 100.0f) / 100.0f;
    ISHWorkspaceSetCurrentDensity(sender.value);
    _densityValueLabel.text = ISHWorkspaceCurrentDensityTitle();
}

- (void)generateThemeBackgroundImage:(id)sender {
    (void) sender;
    WorkspaceViewController *workspaceViewController = self.workspaceHostViewController;
    if (workspaceViewController == nil)
        return;
    CGSize targetSize = workspaceViewController.desktopSurfaceView.bounds.size;
    if (targetSize.width <= 1 || targetSize.height <= 1)
        targetSize = UIScreen.mainScreen.bounds.size;
    CGFloat scale = MAX(UIScreen.mainScreen.scale, 2.0);
    CGSize imageSize = CGSizeMake(MAX(1.0, targetSize.width * scale), MAX(1.0, targetSize.height * scale));
    UIImage *image = ISHWorkspaceThemeArtworkImage(_editingThemeIdentifier,
                                                   [self draftPalette],
                                                   imageSize,
                                                   YES);
    if (image == nil)
        return;
    [workspaceViewController applyWorkspaceWallpaperImage:image];
    _backgroundPreviewImageView.image = image;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Themes";

    _swatchViewsByKey = [NSMutableDictionary dictionary];
    _channelSlidersByKey = [NSMutableDictionary dictionary];
    _channelValueLabelsByKey = [NSMutableDictionary dictionary];
    _themePreviewImageViewsByIdentifier = [NSMutableDictionary dictionary];
    _themeTitleLabelsByIdentifier = [NSMutableDictionary dictionary];
    _themeDetailLabelsByIdentifier = [NSMutableDictionary dictionary];
    _themeSelectionButtons = [NSMutableArray array];
    _editorActionButtons = [NSMutableArray array];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 12;
    [_scrollView addSubview:_contentStack];

    UIView *headerCard = [self workspaceThemeCardView];
    UIStackView *headerStack = [UIStackView new];
    headerStack.translatesAutoresizingMaskIntoConstraints = NO;
    headerStack.axis = UILayoutConstraintAxisVertical;
    headerStack.spacing = 6;
    [headerCard addSubview:headerStack];
    UILabel *headerEyebrow = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    headerEyebrow.text = @"WORKSPACE THEMES";
    headerEyebrow.font = [UIFont systemFontOfSize:11 weight:UIFontWeightSemibold];
    _activeThemeLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleTitle2 monospaced:NO];
    _activeThemeLabel.numberOfLines = 0;
    UILabel *headerBody = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    headerBody.text = @"Change themes from one native utility, then fine-tune and save custom palettes for every Workspace app.";
    [headerStack addArrangedSubview:headerEyebrow];
    [headerStack addArrangedSubview:_activeThemeLabel];
    [headerStack addArrangedSubview:headerBody];
    [NSLayoutConstraint activateConstraints:@[
        [headerStack.topAnchor constraintEqualToAnchor:headerCard.topAnchor constant:14],
        [headerStack.leadingAnchor constraintEqualToAnchor:headerCard.leadingAnchor constant:14],
        [headerStack.trailingAnchor constraintEqualToAnchor:headerCard.trailingAnchor constant:-14],
        [headerStack.bottomAnchor constraintEqualToAnchor:headerCard.bottomAnchor constant:-14],
    ]];

    UIView *libraryCard = [self workspaceThemeCardView];
    UIStackView *libraryStack = [UIStackView new];
    libraryStack.translatesAutoresizingMaskIntoConstraints = NO;
    libraryStack.axis = UILayoutConstraintAxisVertical;
    libraryStack.spacing = 8;
    [libraryCard addSubview:libraryStack];
    UILabel *libraryTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    libraryTitle.text = @"Theme Library";
    UILabel *librarySubtitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    librarySubtitle.text = @"Tap any theme to apply it everywhere and load it into the editor below.";
    _themeListStack = [UIStackView new];
    _themeListStack.axis = UILayoutConstraintAxisVertical;
    _themeListStack.spacing = 6;
    [libraryStack addArrangedSubview:libraryTitle];
    [libraryStack addArrangedSubview:librarySubtitle];
    [libraryStack addArrangedSubview:_themeListStack];
    [NSLayoutConstraint activateConstraints:@[
        [libraryStack.topAnchor constraintEqualToAnchor:libraryCard.topAnchor constant:14],
        [libraryStack.leadingAnchor constraintEqualToAnchor:libraryCard.leadingAnchor constant:14],
        [libraryStack.trailingAnchor constraintEqualToAnchor:libraryCard.trailingAnchor constant:-14],
        [libraryStack.bottomAnchor constraintEqualToAnchor:libraryCard.bottomAnchor constant:-14],
    ]];

    UIView *editorCard = [self workspaceThemeCardView];
    UIStackView *editorStack = [UIStackView new];
    editorStack.translatesAutoresizingMaskIntoConstraints = NO;
    editorStack.axis = UILayoutConstraintAxisVertical;
    editorStack.spacing = 10;
    [editorCard addSubview:editorStack];
    UILabel *editorTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    editorTitle.text = @"Palette Editor";
    _editorThemeLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];

    _previewSurfaceView = [UIView new];
    _previewSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    _previewSurfaceView.layer.cornerRadius = 18;
    _previewSurfaceView.layer.borderWidth = 1;
    _previewSurfaceView.layer.masksToBounds = YES;
    _previewGradientLayer = [CAGradientLayer layer];
    _previewGradientLayer.startPoint = CGPointMake(0.1, 0.0);
    _previewGradientLayer.endPoint = CGPointMake(0.9, 1.0);
    [_previewSurfaceView.layer insertSublayer:_previewGradientLayer atIndex:0];

    UIStackView *previewStack = [UIStackView new];
    previewStack.translatesAutoresizingMaskIntoConstraints = NO;
    previewStack.axis = UILayoutConstraintAxisVertical;
    previewStack.spacing = 6;
    [_previewSurfaceView addSubview:previewStack];
    _previewTitleLabel = [UILabel new];
    _previewTitleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle3];
    _previewTitleLabel.text = @"Preview";
    _previewBodyLabel = [UILabel new];
    _previewBodyLabel.numberOfLines = 0;
    _previewBodyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _previewBodyLabel.text = @"Buttons, cards, text, and monitor bars will all update when you apply this theme.";
    _previewProgressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _previewProgressView.translatesAutoresizingMaskIntoConstraints = NO;
    _previewProgressView.transform = CGAffineTransformMakeScale(1.0, 1.4);
    [previewStack addArrangedSubview:_previewTitleLabel];
    [previewStack addArrangedSubview:_previewBodyLabel];
    [previewStack addArrangedSubview:_previewProgressView];
    [NSLayoutConstraint activateConstraints:@[
        [_previewSurfaceView.heightAnchor constraintGreaterThanOrEqualToConstant:126],
        [previewStack.topAnchor constraintEqualToAnchor:_previewSurfaceView.topAnchor constant:14],
        [previewStack.leadingAnchor constraintEqualToAnchor:_previewSurfaceView.leadingAnchor constant:14],
        [previewStack.trailingAnchor constraintEqualToAnchor:_previewSurfaceView.trailingAnchor constant:-14],
        [previewStack.bottomAnchor constraintEqualToAnchor:_previewSurfaceView.bottomAnchor constant:-14],
    ]];

    UIView *backgroundCard = [self workspaceThemeCardView];
    UIStackView *backgroundStack = [UIStackView new];
    backgroundStack.translatesAutoresizingMaskIntoConstraints = NO;
    backgroundStack.axis = UILayoutConstraintAxisVertical;
    backgroundStack.spacing = 6;
    [backgroundCard addSubview:backgroundStack];
    UILabel *backgroundTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    backgroundTitle.text = @"Wallpaper Preview";
    UILabel *backgroundSubtitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    backgroundSubtitle.text = @"Generated from the current palette and sized for the current screen.";
    _backgroundPreviewImageView = [UIImageView new];
    _backgroundPreviewImageView.translatesAutoresizingMaskIntoConstraints = NO;
    _backgroundPreviewImageView.contentMode = UIViewContentModeScaleAspectFill;
    _backgroundPreviewImageView.clipsToBounds = YES;
    _backgroundPreviewImageView.layer.cornerRadius = 12;
    _backgroundPreviewImageView.layer.borderWidth = 1;
    [backgroundStack addArrangedSubview:backgroundTitle];
    [backgroundStack addArrangedSubview:backgroundSubtitle];
    [backgroundStack addArrangedSubview:_backgroundPreviewImageView];
    [NSLayoutConstraint activateConstraints:@[
        [_backgroundPreviewImageView.heightAnchor constraintEqualToConstant:124],
        [backgroundStack.topAnchor constraintEqualToAnchor:backgroundCard.topAnchor constant:14],
        [backgroundStack.leadingAnchor constraintEqualToAnchor:backgroundCard.leadingAnchor constant:14],
        [backgroundStack.trailingAnchor constraintEqualToAnchor:backgroundCard.trailingAnchor constant:-14],
        [backgroundStack.bottomAnchor constraintEqualToAnchor:backgroundCard.bottomAnchor constant:-14],
    ]];

    UIView *densityCard = [self workspaceThemeCardView];
    UIStackView *densityStack = [UIStackView new];
    densityStack.translatesAutoresizingMaskIntoConstraints = NO;
    densityStack.axis = UILayoutConstraintAxisVertical;
    densityStack.spacing = 6;
    [densityCard addSubview:densityStack];
    UILabel *densityTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    densityTitle.text = @"Utility Density";
    _densityValueLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    _densitySlider = [UISlider new];
    _densitySlider.minimumValue = 0.0f;
    _densitySlider.maximumValue = 1.0f;
    _densitySlider.value = (float) ISHWorkspaceCurrentDensity();
    [_densitySlider addTarget:self action:@selector(densitySliderChanged:) forControlEvents:UIControlEventValueChanged];
    UILabel *densitySubtitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    densitySubtitle.text = @"Lower values pack the utility cards tighter for smaller screens.";
    [densityStack addArrangedSubview:densityTitle];
    [densityStack addArrangedSubview:_densityValueLabel];
    [densityStack addArrangedSubview:_densitySlider];
    [densityStack addArrangedSubview:densitySubtitle];
    [NSLayoutConstraint activateConstraints:@[
        [densityStack.topAnchor constraintEqualToAnchor:densityCard.topAnchor constant:14],
        [densityStack.leadingAnchor constraintEqualToAnchor:densityCard.leadingAnchor constant:14],
        [densityStack.trailingAnchor constraintEqualToAnchor:densityCard.trailingAnchor constant:-14],
        [densityStack.bottomAnchor constraintEqualToAnchor:densityCard.bottomAnchor constant:-14],
    ]];

    UIStackView *actionStack = [UIStackView new];
    actionStack.axis = UILayoutConstraintAxisVertical;
    actionStack.spacing = 8;

    UIStackView *actionRowTop = [UIStackView new];
    actionRowTop.axis = UILayoutConstraintAxisHorizontal;
    actionRowTop.spacing = 8;
    actionRowTop.distribution = UIStackViewDistributionFillEqually;
    [actionRowTop addArrangedSubview:[self themeUtilityButtonWithTitle:@"Save As New"
                                                              selector:@selector(saveThemeAsNew:)]];
    [actionRowTop addArrangedSubview:[self themeUtilityButtonWithTitle:@"Update Selected"
                                                              selector:@selector(updateSelectedCustomTheme:)]];

    UIStackView *actionRowBottom = [UIStackView new];
    actionRowBottom.axis = UILayoutConstraintAxisHorizontal;
    actionRowBottom.spacing = 8;
    actionRowBottom.distribution = UIStackViewDistributionFillEqually;
    [actionRowBottom addArrangedSubview:[self themeUtilityButtonWithTitle:@"Delete Selected"
                                                                 selector:@selector(deleteSelectedCustomTheme:)]];
    [actionRowBottom addArrangedSubview:[self themeUtilityButtonWithTitle:@"Apply Wallpaper"
                                                                 selector:@selector(generateThemeBackgroundImage:)]];

    [actionStack addArrangedSubview:actionRowTop];
    [actionStack addArrangedSubview:actionRowBottom];

    [editorStack addArrangedSubview:editorTitle];
    [editorStack addArrangedSubview:_editorThemeLabel];
    [editorStack addArrangedSubview:_previewSurfaceView];
    [editorStack addArrangedSubview:densityCard];
    [editorStack addArrangedSubview:actionStack];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        [editorStack addArrangedSubview:[self sliderRowWithTitle:[self themeEditorTitleForKey:key] key:key]];
    }
    [NSLayoutConstraint activateConstraints:@[
        [editorStack.topAnchor constraintEqualToAnchor:editorCard.topAnchor constant:14],
        [editorStack.leadingAnchor constraintEqualToAnchor:editorCard.leadingAnchor constant:14],
        [editorStack.trailingAnchor constraintEqualToAnchor:editorCard.trailingAnchor constant:-14],
        [editorStack.bottomAnchor constraintEqualToAnchor:editorCard.bottomAnchor constant:-14],
    ]];

    [_contentStack addArrangedSubview:headerCard];
    [_contentStack addArrangedSubview:backgroundCard];
    [_contentStack addArrangedSubview:libraryCard];
    [_contentStack addArrangedSubview:editorCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:14],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:14],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-14],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-14],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-28],
    ]];

    [self refreshThemeSelectionButtons];
    [self loadThemeIntoEditorWithIdentifier:ISHWorkspaceCurrentThemeIdentifier()];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _previewGradientLayer.frame = _previewSurfaceView.bounds;
}

- (void)workspaceThemeDidChange:(NSNotification *)notification {
    [super workspaceThemeDidChange:notification];
    _activeThemeLabel.text = [NSString stringWithFormat:@"Active Theme: %@", ISHWorkspaceCurrentThemeTitle()];
    [self refreshThemeSelectionButtons];
    if (_editingThemeIdentifier.length == 0 || ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier) == nil) {
        [self loadThemeIntoEditorWithIdentifier:ISHWorkspaceCurrentThemeIdentifier()];
    }
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _activeThemeLabel.text = [NSString stringWithFormat:@"Active Theme: %@", ISHWorkspaceCurrentThemeTitle()];

    for (UIButton *button in _themeSelectionButtons) {
        NSString *identifier = button.accessibilityIdentifier;
        BOOL selected = [identifier isEqualToString:ISHWorkspaceCurrentThemeIdentifier()];
        button.backgroundColor = selected
            ? [theme[@"accent"] colorWithAlphaComponent:0.34]
            : [theme[@"cardAlt"] colorWithAlphaComponent:0.92];
        button.layer.borderWidth = selected ? 2.0 : 1.0;
        button.layer.borderColor = (selected ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        button.layer.shadowColor = selected ? theme[@"accent"].CGColor : UIColor.clearColor.CGColor;
        button.layer.shadowOpacity = selected ? 0.35 : 0.0;
        button.layer.shadowRadius = selected ? 10.0 : 0.0;
        button.layer.shadowOffset = CGSizeMake(0, 0);
        _themePreviewImageViewsByIdentifier[identifier].layer.borderColor =
            (selected ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        _themeTitleLabelsByIdentifier[identifier].textColor = selected ? theme[@"card"] : theme[@"primary"];
        _themeDetailLabelsByIdentifier[identifier].textColor = selected ? theme[@"cardAlt"] : theme[@"secondary"];
    }
    BOOL editingCustom = !ISHWorkspaceThemeIdentifierIsBuiltIn(_editingThemeIdentifier);
    for (UIButton *button in _editorActionButtons) {
        button.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.92];
        button.layer.borderColor = theme[@"stroke"].CGColor;
        [button setTitleColor:theme[@"accent"] forState:UIControlStateNormal];
    }
    _backgroundPreviewImageView.layer.borderColor = theme[@"stroke"].CGColor;
    _densitySlider.minimumTrackTintColor = theme[@"accent"];
    _densitySlider.maximumTrackTintColor = [theme[@"accentAlt"] colorWithAlphaComponent:0.24];
    _densityValueLabel.text = ISHWorkspaceCurrentDensityTitle();
    if (_editorActionButtons.count >= 3) {
        _editorActionButtons[1].enabled = editingCustom;
        _editorActionButtons[1].alpha = editingCustom ? 1.0 : 0.45;
        _editorActionButtons[2].enabled = editingCustom;
        _editorActionButtons[2].alpha = editingCustom ? 1.0 : 0.45;
    }
}

@end

@implementation WorkspaceProcessesToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UITextView *_detailsTextView;
    NSTimer *_timer;
    NSUInteger _refreshGeneration;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Processes";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 6;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"VISIBLE PROCESSES";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:12],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:12],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-12],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-12],
    ]];

    UIView *detailsCard = [self workspaceThemeCardView];
    _detailsTextView = [self workspaceThemeTextView];
    [detailsCard addSubview:_detailsTextView];
    [NSLayoutConstraint activateConstraints:@[
        [detailsCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 116.0 : 144.0)],
        [_detailsTextView.topAnchor constraintEqualToAnchor:detailsCard.topAnchor constant:8],
        [_detailsTextView.leadingAnchor constraintEqualToAnchor:detailsCard.leadingAnchor constant:8],
        [_detailsTextView.trailingAnchor constraintEqualToAnchor:detailsCard.trailingAnchor constant:-8],
        [_detailsTextView.bottomAnchor constraintEqualToAnchor:detailsCard.bottomAnchor constant:-8],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:detailsCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshProcesses:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:4.0
                                              target:self
                                            selector:@selector(refreshProcesses:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshProcesses:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

- (void)refreshProcesses:(id)sender {
    (void) sender;
    NSUInteger generation = ++_refreshGeneration;
    _detailsTextView.text = @"Refreshing process table…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<NSDictionary<NSString *, id> *> *processes = ISHWorkspaceVisibleProcessRecords(8);
        uint64_t footprint = 0;
        BOOL hasMemory = ISHWorkspaceMemoryUsage(&footprint, NULL, NULL);
        NSMutableArray<NSString *> *lines = [NSMutableArray array];
        for (NSDictionary<NSString *, id> *process in processes) {
            NSString *marker = [process[@"isCurrent"] boolValue] ? @"current app" : process[@"state"];
            [lines addObject:[NSString stringWithFormat:@"%5d  %@\n       %@",
                              [process[@"pid"] intValue],
                              process[@"name"],
                              marker]];
        }
        NSString *summary = hasMemory
            ? [NSString stringWithFormat:@"App footprint %@  •  %lu visible processes",
                                          ISHWorkspaceByteCountString(footprint),
                                          (unsigned long) processes.count]
            : [NSString stringWithFormat:@"%lu visible processes", (unsigned long) processes.count];
        NSString *details = lines.count > 0
            ? [lines componentsJoinedByString:@"\n\n"]
            : @"Per-process inspection is unavailable on this device. App memory and terminal activity are still tracked by the other Workspace utilities.";
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != self->_refreshGeneration)
                return;
            self->_summaryLabel.text = summary;
            self->_detailsTextView.text = details;
        });
    });
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    _summaryLabel.textColor = self.workspaceTheme[@"accent"];
}

@end

@implementation WorkspaceSessionsToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UIStackView *_quickActionsStack;
    UIStackView *_sessionButtonsStack;
    NSMutableArray<UIButton *> *_trackedButtons;
}

- (UIButton *)sessionButtonWithTitle:(NSString *)title subtitle:(NSString *)subtitle selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentEdgeInsets = UIEdgeInsetsMake(7, 10, 7, 10);
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.numberOfLines = 0;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;

    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.alignment = NSTextAlignmentLeft;
    NSMutableAttributedString *titleString =
        [[NSMutableAttributedString alloc] initWithString:title
                                               attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline)
                                               weight:UIFontWeightSemibold],
        NSParagraphStyleAttributeName: style,
    }];
    [titleString appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                        attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [titleString appendAttributedString:[[NSAttributedString alloc] initWithString:subtitle
                                                                        attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [button setAttributedTitle:titleString forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    [_trackedButtons addObject:button];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Sessions";
    _trackedButtons = [NSMutableArray array];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 6;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"ACTIVE SESSIONS";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:12],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:12],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-12],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-12],
    ]];

    UIView *quickActionsCard = [self workspaceThemeCardView];
    _quickActionsStack = [UIStackView new];
    _quickActionsStack.translatesAutoresizingMaskIntoConstraints = NO;
    _quickActionsStack.axis = UILayoutConstraintAxisVertical;
    _quickActionsStack.spacing = 6;
    [quickActionsCard addSubview:_quickActionsStack];

    UIStackView *rowOne = [UIStackView new];
    rowOne.axis = UILayoutConstraintAxisHorizontal;
    rowOne.spacing = 6;
    rowOne.distribution = UIStackViewDistributionFillEqually;
    UIButton *shellButton = [self sessionButtonWithTitle:@"Session Shell"
                                                subtitle:@"Open or focus the primary shell"
                                                selector:@selector(openShellShortcut:)];
    UIButton *consoleButton = [self sessionButtonWithTitle:@"System Console"
                                                  subtitle:@"Open or focus the console"
                                                  selector:@selector(openConsoleShortcut:)];
    [rowOne addArrangedSubview:shellButton];
    [rowOne addArrangedSubview:consoleButton];

    UIStackView *rowTwo = [UIStackView new];
    rowTwo.axis = UILayoutConstraintAxisHorizontal;
    rowTwo.spacing = 6;
    rowTwo.distribution = UIStackViewDistributionFillEqually;
    UIButton *dashboardButton = [self sessionButtonWithTitle:@"Dashboard"
                                                    subtitle:@"Jump back to workspace layout"
                                                    selector:@selector(openDashboardShortcut:)];
    UIButton *workspaceButton = [self sessionButtonWithTitle:@"New Workspace"
                                                    subtitle:@"Open another workspace window"
                                                    selector:@selector(openWorkspaceShortcut:)];
    [rowTwo addArrangedSubview:dashboardButton];
    [rowTwo addArrangedSubview:workspaceButton];
    [_quickActionsStack addArrangedSubview:rowOne];
    [_quickActionsStack addArrangedSubview:rowTwo];
    [NSLayoutConstraint activateConstraints:@[
        [_quickActionsStack.topAnchor constraintEqualToAnchor:quickActionsCard.topAnchor constant:8],
        [_quickActionsStack.leadingAnchor constraintEqualToAnchor:quickActionsCard.leadingAnchor constant:8],
        [_quickActionsStack.trailingAnchor constraintEqualToAnchor:quickActionsCard.trailingAnchor constant:-8],
        [_quickActionsStack.bottomAnchor constraintEqualToAnchor:quickActionsCard.bottomAnchor constant:-8],
    ]];

    UIView *sessionsCard = [self workspaceThemeCardView];
    UIStackView *sessionsStack = [UIStackView new];
    sessionsStack.translatesAutoresizingMaskIntoConstraints = NO;
    sessionsStack.axis = UILayoutConstraintAxisVertical;
    sessionsStack.spacing = 6;
    [sessionsCard addSubview:sessionsStack];
    UILabel *sessionsTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    sessionsTitle.text = @"Live terminals";
    _sessionButtonsStack = [UIStackView new];
    _sessionButtonsStack.axis = UILayoutConstraintAxisVertical;
    _sessionButtonsStack.spacing = 6;
    [sessionsStack addArrangedSubview:sessionsTitle];
    [sessionsStack addArrangedSubview:_sessionButtonsStack];
    [NSLayoutConstraint activateConstraints:@[
        [sessionsStack.topAnchor constraintEqualToAnchor:sessionsCard.topAnchor constant:10],
        [sessionsStack.leadingAnchor constraintEqualToAnchor:sessionsCard.leadingAnchor constant:10],
        [sessionsStack.trailingAnchor constraintEqualToAnchor:sessionsCard.trailingAnchor constant:-10],
        [sessionsStack.bottomAnchor constraintEqualToAnchor:sessionsCard.bottomAnchor constant:-10],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:quickActionsCard];
    [_contentStack addArrangedSubview:sessionsCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshSessionsNotification:)
                                               name:TerminalRegistryDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshSessionsNotification:)
                                               name:TerminalDidLoadNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshSessionsNotification:)
                                               name:TerminalLoadFailedNotification
                                             object:nil];
    [self refreshSessions];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:TerminalRegistryDidChangeNotification
                                                object:nil];
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:TerminalDidLoadNotification
                                                object:nil];
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:TerminalLoadFailedNotification
                                                object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshSessions];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
    _quickActionsStack.spacing = ISHWorkspaceDensityValue(4, 6);
    _sessionButtonsStack.spacing = ISHWorkspaceDensityValue(4, 6);
}

- (void)refreshSessionsNotification:(__unused NSNotification *)notification {
    [self refreshSessions];
}

- (void)refreshSessions {
    NSUInteger sceneCount = 0;
    if (@available(iOS 13.0, *)) {
        sceneCount = UIApplication.sharedApplication.connectedScenes.count;
    }
    _summaryLabel.text = [NSString stringWithFormat:@"%lu terminals  •  %lu roots  •  %lu scenes",
                          (unsigned long) Terminal.activeTerminals.count,
                          (unsigned long) Roots.instance.roots.count,
                          (unsigned long) sceneCount];

    NSArray<UIView *> *existingRows = _sessionButtonsStack.arrangedSubviews.copy;
    for (UIView *view in existingRows) {
        [_sessionButtonsStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    if (_trackedButtons.count > 4) {
        [_trackedButtons removeObjectsInRange:NSMakeRange(4, _trackedButtons.count - 4)];
    }

    if (Terminal.activeTerminals.count == 0) {
        UILabel *emptyLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
        emptyLabel.text = @"No active terminals. Use the quick actions above to start a shell or console.";
        [_sessionButtonsStack addArrangedSubview:emptyLabel];
        return;
    }

    for (Terminal *terminal in Terminal.activeTerminals) {
        NSString *uuidString = terminal.uuid.UUIDString ?: @"";
        ISHWorkspaceContainedWindowView *existingWindow = [self.workspaceHostViewController desktopWindowHostingTerminalUUID:terminal.uuid];
        NSString *subtitle = existingWindow != nil
            ? @"Focus current workspace window"
            : (terminal.webView.superview != nil ? @"Focus another window" : @"Reconnect in this workspace");
        UIButton *button = [self sessionButtonWithTitle:ISHWorkspaceTerminalDisplayName(terminal)
                                               subtitle:subtitle
                                               selector:@selector(openSessionTerminal:)];
        button.accessibilityIdentifier = uuidString;
        [_sessionButtonsStack addArrangedSubview:button];
    }
    [self workspaceApplyTheme];
}

- (void)openShellShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openTerminalHerePreferringConsole:NO];
}

- (void)openConsoleShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openTerminalHerePreferringConsole:YES];
}

- (void)openDashboardShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openDashboardWindow:nil];
}

- (void)openWorkspaceShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openNewWorkspaceWindow:nil];
}

- (void)openSessionTerminal:(UIButton *)sender {
    NSString *uuidString = sender.accessibilityIdentifier;
    if (uuidString.length == 0)
        return;
    [self.workspaceHostViewController openExistingTerminalHereWithUUID:[[NSUUID alloc] initWithUUIDString:uuidString]];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _summaryLabel.textColor = theme[@"accentAlt"];
    for (UIButton *button in _trackedButtons) {
        button.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
        button.layer.borderColor = theme[@"stroke"].CGColor;
        NSMutableAttributedString *title =
            [[NSMutableAttributedString alloc] initWithAttributedString:[button attributedTitleForState:UIControlStateNormal]];
        [title addAttributes:@{
            NSForegroundColorAttributeName: theme[@"primary"],
        } range:NSMakeRange(0, title.length)];
        if (title.length > 0) {
            NSRange newlineRange = [[title string] rangeOfString:@"\n"];
            if (newlineRange.location != NSNotFound) {
                NSUInteger subtitleLocation = newlineRange.location + newlineRange.length;
                if (subtitleLocation < title.length) {
                    [title addAttributes:@{
                        NSForegroundColorAttributeName: theme[@"secondary"],
                    } range:NSMakeRange(subtitleLocation, title.length - subtitleLocation)];
                }
            }
        }
        [button setAttributedTitle:title forState:UIControlStateNormal];
    }
}

@end

@implementation WorkspaceStorageToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UITextView *_detailsTextView;
    UIButton *_refreshButton;
    NSUInteger _refreshGeneration;
}

- (UIButton *)storageActionButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.layer.cornerRadius = 10;
    button.layer.borderWidth = 1;
    button.contentEdgeInsets = UIEdgeInsetsMake(5, 10, 5, 10);
    button.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightSemibold];
    [button setTitle:@"Refresh" forState:UIControlStateNormal];
    [button addTarget:self action:@selector(refreshStorage:) forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Storage";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 6;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"ROOT STORAGE";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:12],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:12],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-12],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-12],
    ]];

    UIView *actionsCard = [self workspaceThemeCardView];
    UIStackView *actionsStack = [UIStackView new];
    actionsStack.translatesAutoresizingMaskIntoConstraints = NO;
    actionsStack.axis = UILayoutConstraintAxisHorizontal;
    actionsStack.alignment = UIStackViewAlignmentCenter;
    actionsStack.spacing = 8;
    [actionsCard addSubview:actionsStack];
    UILabel *actionsLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    actionsLabel.text = @"Rescan roots and container directories.";
    _refreshButton = [self storageActionButton];
    [actionsStack addArrangedSubview:actionsLabel];
    [actionsStack addArrangedSubview:_refreshButton];
    [actionsLabel setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [actionsStack.topAnchor constraintEqualToAnchor:actionsCard.topAnchor constant:8],
        [actionsStack.leadingAnchor constraintEqualToAnchor:actionsCard.leadingAnchor constant:10],
        [actionsStack.trailingAnchor constraintEqualToAnchor:actionsCard.trailingAnchor constant:-10],
        [actionsStack.bottomAnchor constraintEqualToAnchor:actionsCard.bottomAnchor constant:-8],
    ]];

    UIView *detailsCard = [self workspaceThemeCardView];
    _detailsTextView = [self workspaceThemeTextView];
    [detailsCard addSubview:_detailsTextView];
    [NSLayoutConstraint activateConstraints:@[
        [detailsCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 132.0 : 168.0)],
        [_detailsTextView.topAnchor constraintEqualToAnchor:detailsCard.topAnchor constant:8],
        [_detailsTextView.leadingAnchor constraintEqualToAnchor:detailsCard.leadingAnchor constant:8],
        [_detailsTextView.trailingAnchor constraintEqualToAnchor:detailsCard.trailingAnchor constant:-8],
        [_detailsTextView.bottomAnchor constraintEqualToAnchor:detailsCard.bottomAnchor constant:-8],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:actionsCard];
    [_contentStack addArrangedSubview:detailsCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshStorage:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshStorage:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

- (void)refreshStorage:(id)sender {
    (void) sender;
    NSUInteger generation = ++_refreshGeneration;
    _detailsTextView.text = @"Scanning roots and container directories…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSArray<NSDictionary<NSString *, id> *> *rootRecords = ISHWorkspaceRootUsageRecords();
        NSDictionary<NSString *, NSNumber *> *tmpUsage = ISHWorkspaceDirectoryUsage([NSURL fileURLWithPath:NSTemporaryDirectory()]);
        NSArray<NSURL *> *cacheDirectories =
            [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask];
        NSDictionary<NSString *, NSNumber *> *cacheUsage =
            ISHWorkspaceDirectoryUsage(cacheDirectories.firstObject ?: [NSURL fileURLWithPath:NSHomeDirectory()]);
        uint64_t totalRootBytes = 0;
        NSMutableArray<NSString *> *lines = [NSMutableArray array];
        [lines addObject:@"Workspace roots:"];
        for (NSDictionary<NSString *, id> *record in rootRecords) {
            totalRootBytes += [record[@"bytes"] unsignedLongLongValue];
            NSString *marker = [record[@"isDefault"] boolValue] ? @"default" : @"root";
            [lines addObject:[NSString stringWithFormat:@"%@  •  %@  •  %@",
                              record[@"name"],
                              record[@"abi"],
                              marker]];
            [lines addObject:[NSString stringWithFormat:@"  %@  •  %@ files  •  %@ dirs",
                              ISHWorkspaceByteCountString([record[@"bytes"] unsignedLongLongValue]),
                              record[@"files"],
                              record[@"directories"]]];
        }
        if (rootRecords.count == 0) {
            [lines addObject:@"No installed roots."];
        }
        [lines addObject:@""];
        [lines addObject:@"Container directories:"];
        [lines addObject:[NSString stringWithFormat:@"tmp  •  %@", ISHWorkspaceByteCountString([tmpUsage[@"bytes"] unsignedLongLongValue])]];
        [lines addObject:[NSString stringWithFormat:@"cache  •  %@", ISHWorkspaceByteCountString([cacheUsage[@"bytes"] unsignedLongLongValue])]];

        NSDictionary<NSFileAttributeKey, id> *attributes =
            [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
        uint64_t freeBytes = [attributes[NSFileSystemFreeSize] unsignedLongLongValue];
        NSString *summary = [NSString stringWithFormat:@"%@ free  •  %@ across %lu roots",
                                                       ISHWorkspaceByteCountString(freeBytes),
                                                       ISHWorkspaceByteCountString(totalRootBytes),
                                                       (unsigned long) rootRecords.count];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != self->_refreshGeneration)
                return;
            self->_summaryLabel.text = summary;
            self->_detailsTextView.text = [lines componentsJoinedByString:@"\n"];
        });
    });
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _summaryLabel.textColor = theme[@"accent"];
    _refreshButton.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.96];
    _refreshButton.layer.borderColor = theme[@"stroke"].CGColor;
    [_refreshButton setTitleColor:theme[@"accentAlt"] forState:UIControlStateNormal];
}

@end

@implementation WorkspaceShortcutsToolViewController {
    UIStackView *_contentStack;
    NSMutableArray<UIButton *> *_shortcutButtons;
}

- (UIButton *)shortcutButtonWithTitle:(NSString *)title subtitle:(NSString *)subtitle identifier:(NSString *)identifier {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityIdentifier = identifier;
    button.contentEdgeInsets = UIEdgeInsetsMake(8, 10, 8, 10);
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.numberOfLines = 0;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;

    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.alignment = NSTextAlignmentLeft;
    NSMutableAttributedString *label =
        [[NSMutableAttributedString alloc] initWithString:title
                                               attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline)
                                               weight:UIFontWeightSemibold],
        NSParagraphStyleAttributeName: style,
    }];
    [label appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                  attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [label appendAttributedString:[[NSAttributedString alloc] initWithString:subtitle
                                                                  attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [button setAttributedTitle:label forState:UIControlStateNormal];
    [button addTarget:self action:@selector(runShortcut:) forControlEvents:UIControlEventTouchUpInside];
    [_shortcutButtons addObject:button];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Shortcuts";
    _shortcutButtons = [NSMutableArray array];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 6;
    [self.toolContentView addSubview:_contentStack];

    UILabel *header = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    header.text = @"Quick workspace actions";
    UILabel *detail = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    detail.text = @"Open the most common tools and terminal actions without leaving the workspace.";
    [_contentStack addArrangedSubview:header];
    [_contentStack addArrangedSubview:detail];

    NSArray<NSArray<NSDictionary<NSString *, NSString *> *> *> *rows = @[
        @[
            @{@"title": @"Dashboard", @"subtitle": @"Layout and scene overview", @"identifier": @"dashboard"},
            @{@"title": @"Workspaces", @"subtitle": @"Tiny scene switcher", @"identifier": ISHWorkspaceToolWorkspacesIdentifier},
        ],
        @[
            @{@"title": @"Session Shell", @"subtitle": @"Open or focus the shell", @"identifier": @"shell"},
            @{@"title": @"System Console", @"subtitle": @"Open or focus the console", @"identifier": @"console"},
        ],
        @[
            @{@"title": @"Sessions", @"subtitle": @"Inspect live terminals", @"identifier": ISHWorkspaceToolSessionsIdentifier},
            @{@"title": @"Storage", @"subtitle": @"Root and container usage", @"identifier": ISHWorkspaceToolStorageIdentifier},
        ],
        @[
            @{@"title": @"Themes", @"subtitle": @"Colors, density, wallpaper", @"identifier": ISHWorkspaceToolThemesIdentifier},
            @{@"title": @"Filesystems", @"subtitle": @"Manage installed roots", @"identifier": ISHWorkspaceToolFilesystemsIdentifier},
        ],
        @[
            @{@"title": @"New Workspace", @"subtitle": @"Open another workspace window", @"identifier": @"new-workspace"},
            @{@"title": @"Clock", @"subtitle": @"Quick local time", @"identifier": ISHWorkspaceToolClockIdentifier},
        ],
    ];

    for (NSArray<NSDictionary<NSString *, NSString *> *> *rowDescriptors in rows) {
        UIStackView *row = [UIStackView new];
        row.axis = UILayoutConstraintAxisHorizontal;
        row.spacing = 6;
        row.distribution = UIStackViewDistributionFillEqually;
        for (NSDictionary<NSString *, NSString *> *descriptor in rowDescriptors) {
            [row addArrangedSubview:[self shortcutButtonWithTitle:descriptor[@"title"]
                                                         subtitle:descriptor[@"subtitle"]
                                                       identifier:descriptor[@"identifier"]]];
        }
        [_contentStack addArrangedSubview:row];
    }

    [NSLayoutConstraint activateConstraints:@[
        [_contentStack.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:8],
        [_contentStack.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:8],
        [_contentStack.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-8],
        [_contentStack.bottomAnchor constraintLessThanOrEqualToAnchor:self.toolContentView.bottomAnchor constant:-8],
    ]];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 6);
}

- (void)runShortcut:(UIButton *)sender {
    NSString *identifier = sender.accessibilityIdentifier;
    if (identifier.length == 0)
        return;
    if ([identifier isEqualToString:@"dashboard"]) {
        [self.workspaceHostViewController openDashboardWindow:nil];
    } else if ([identifier isEqualToString:@"shell"]) {
        [self.workspaceHostViewController openTerminalHerePreferringConsole:NO];
    } else if ([identifier isEqualToString:@"console"]) {
        [self.workspaceHostViewController openTerminalHerePreferringConsole:YES];
    } else if ([identifier isEqualToString:@"new-workspace"]) {
        [self.workspaceHostViewController openNewWorkspaceWindow:nil];
    } else {
        [self.workspaceHostViewController openOrFocusWorkspaceToolIdentifier:identifier];
    }
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    for (UIButton *button in _shortcutButtons) {
        button.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
        button.layer.borderColor = theme[@"stroke"].CGColor;
        NSMutableAttributedString *title =
            [[NSMutableAttributedString alloc] initWithAttributedString:[button attributedTitleForState:UIControlStateNormal]];
        [title addAttributes:@{NSForegroundColorAttributeName: theme[@"primary"]} range:NSMakeRange(0, title.length)];
        NSRange newlineRange = [[title string] rangeOfString:@"\n"];
        if (newlineRange.location != NSNotFound) {
            NSUInteger subtitleLocation = newlineRange.location + newlineRange.length;
            if (subtitleLocation < title.length) {
                [title addAttributes:@{NSForegroundColorAttributeName: theme[@"secondary"]}
                               range:NSMakeRange(subtitleLocation, title.length - subtitleLocation)];
            }
        }
        [button setAttributedTitle:title forState:UIControlStateNormal];
    }
}

@end

@implementation WorkspaceWorkspacesToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UIStackView *_rowsStack;
    NSMutableArray<UIButton *> *_trackedButtons;
    NSMutableDictionary<NSString *, UIImageView *> *_previewImageViewsByIdentifier;
    NSMutableDictionary<NSString *, UILabel *> *_titleLabelsByIdentifier;
    NSMutableDictionary<NSString *, UILabel *> *_detailLabelsByIdentifier;
    NSArray<NSDictionary<NSString *, id> *> *_sceneDescriptors;
}

- (UIImage *)scenePreviewImageForDescriptor:(NSDictionary<NSString *, id> *)descriptor size:(CGSize)size {
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIColor *backgroundTop = [theme[@"backgroundTop"] colorWithAlphaComponent:0.94];
    UIColor *backgroundBottom = [theme[@"backgroundBottom"] colorWithAlphaComponent:0.98];
    UIColor *frameColor = [theme[@"card"] colorWithAlphaComponent:0.96];
    UIColor *titleBarColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.98];
    UIColor *accentColor = theme[@"accent"];
    UIColor *accentAlt = theme[@"accentAlt"];
    BOOL current = [descriptor[@"isCurrent"] boolValue];
    NSString *role = descriptor[@"role"] ?: @"Unknown";

    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext *rendererContext) {
        CGContextRef context = rendererContext.CGContext;
        CGRect rect = CGRectMake(0, 0, size.width, size.height);
        ISHWorkspaceThemeDrawLinearGradient(context, rect, backgroundTop, backgroundBottom);

        CGRect mainFrame = CGRectInset(rect, 4, 4);
        UIBezierPath *outerPath = [UIBezierPath bezierPathWithRoundedRect:mainFrame cornerRadius:9];
        [frameColor setFill];
        [outerPath fill];

        CGRect titleBar = CGRectMake(CGRectGetMinX(mainFrame),
                                     CGRectGetMinY(mainFrame),
                                     CGRectGetWidth(mainFrame),
                                     12);
        UIBezierPath *titlePath = [UIBezierPath bezierPathWithRoundedRect:titleBar cornerRadius:9];
        [titleBarColor setFill];
        [titlePath fill];

        CGRect dotRect = CGRectMake(CGRectGetMinX(titleBar) + 5, CGRectGetMidY(titleBar) - 1.75, 3.5, 3.5);
        UIBezierPath *dotPath = [UIBezierPath bezierPathWithOvalInRect:dotRect];
        [(current ? accentColor : accentAlt) setFill];
        [dotPath fill];

        if ([role isEqualToString:@"Workspace"]) {
            NSArray<NSValue *> *cards = @[
                [NSValue valueWithCGRect:CGRectMake(CGRectGetMinX(mainFrame) + 6, CGRectGetMinY(mainFrame) + 18, 18, 11)],
                [NSValue valueWithCGRect:CGRectMake(CGRectGetMinX(mainFrame) + 9, CGRectGetMinY(mainFrame) + 33, 23, 14)],
                [NSValue valueWithCGRect:CGRectMake(CGRectGetMinX(mainFrame) + 28, CGRectGetMinY(mainFrame) + 22, 20, 17)],
            ];
            for (NSUInteger index = 0; index < cards.count; index++) {
                UIColor *fill = index == 1
                    ? [accentColor colorWithAlphaComponent:0.24]
                    : [accentAlt colorWithAlphaComponent:0.18];
                UIBezierPath *path = [UIBezierPath bezierPathWithRoundedRect:cards[index].CGRectValue cornerRadius:4];
                [fill setFill];
                [path fill];
            }
        } else {
            CGRect terminalRect = CGRectMake(CGRectGetMinX(mainFrame) + 6,
                                             CGRectGetMinY(mainFrame) + 18,
                                             CGRectGetWidth(mainFrame) - 12,
                                             CGRectGetHeight(mainFrame) - 24);
            UIBezierPath *terminalPath = [UIBezierPath bezierPathWithRoundedRect:terminalRect cornerRadius:5];
            [[UIColor colorWithWhite:0.08 alpha:0.9] setFill];
            [terminalPath fill];
            NSDictionary<NSAttributedStringKey, id> *attributes = @{
                NSFontAttributeName: [UIFont monospacedSystemFontOfSize:5.5 weight:UIFontWeightMedium],
                NSForegroundColorAttributeName: [accentColor colorWithAlphaComponent:0.82],
            };
            [@"$" drawAtPoint:CGPointMake(CGRectGetMinX(terminalRect) + 4, CGRectGetMinY(terminalRect) + 4) withAttributes:attributes];
            [@">" drawAtPoint:CGPointMake(CGRectGetMinX(terminalRect) + 12, CGRectGetMinY(terminalRect) + 10) withAttributes:attributes];
            [@"_" drawAtPoint:CGPointMake(CGRectGetMinX(terminalRect) + 8, CGRectGetMaxY(terminalRect) - 10) withAttributes:attributes];
        }
    }];
}

- (UIButton *)workspaceSceneButtonWithDescriptor:(NSDictionary<NSString *, id> *)descriptor {
    NSString *identifier = descriptor[@"identifier"] ?: NSUUID.UUID.UUIDString;
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityIdentifier = identifier;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentFill;
    button.contentVerticalAlignment = UIControlContentVerticalAlignmentFill;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;
    [button.heightAnchor constraintEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 70.0 : 78.0)].active = YES;
    [button addTarget:self action:@selector(focusWorkspaceSceneButton:) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *contentRow = [UIStackView new];
    contentRow.translatesAutoresizingMaskIntoConstraints = NO;
    contentRow.axis = UILayoutConstraintAxisHorizontal;
    contentRow.alignment = UIStackViewAlignmentCenter;
    contentRow.spacing = 8;
    contentRow.userInteractionEnabled = NO;
    [button addSubview:contentRow];

    UIImageView *previewView = [UIImageView new];
    previewView.translatesAutoresizingMaskIntoConstraints = NO;
    previewView.contentMode = UIViewContentModeScaleAspectFill;
    previewView.clipsToBounds = YES;
    previewView.layer.cornerRadius = 9;
    previewView.layer.borderWidth = 1;
    [previewView.widthAnchor constraintEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 52.0 : 60.0)].active = YES;
    [previewView.heightAnchor constraintEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 38.0 : 44.0)].active = YES;
    [contentRow addArrangedSubview:previewView];

    UIStackView *textStack = [UIStackView new];
    textStack.axis = UILayoutConstraintAxisVertical;
    textStack.spacing = 2;
    textStack.alignment = UIStackViewAlignmentLeading;
    textStack.userInteractionEnabled = NO;
    UILabel *titleLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    titleLabel.numberOfLines = 1;
    titleLabel.text = descriptor[@"title"];
    UILabel *detailLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    detailLabel.numberOfLines = 2;
    detailLabel.text = descriptor[@"detail"];
    [textStack addArrangedSubview:titleLabel];
    [textStack addArrangedSubview:detailLabel];
    [contentRow addArrangedSubview:textStack];

    [NSLayoutConstraint activateConstraints:@[
        [contentRow.topAnchor constraintEqualToAnchor:button.topAnchor constant:8],
        [contentRow.leadingAnchor constraintEqualToAnchor:button.leadingAnchor constant:8],
        [contentRow.trailingAnchor constraintEqualToAnchor:button.trailingAnchor constant:-8],
        [contentRow.bottomAnchor constraintEqualToAnchor:button.bottomAnchor constant:-8],
    ]];

    _previewImageViewsByIdentifier[identifier] = previewView;
    _titleLabelsByIdentifier[identifier] = titleLabel;
    _detailLabelsByIdentifier[identifier] = detailLabel;
    [_trackedButtons addObject:button];
    return button;
}

- (void)rebuildSceneButtons {
    NSArray<UIView *> *existingRows = _rowsStack.arrangedSubviews.copy;
    for (UIView *view in existingRows) {
        [_rowsStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [_trackedButtons removeAllObjects];
    [_previewImageViewsByIdentifier removeAllObjects];
    [_titleLabelsByIdentifier removeAllObjects];
    [_detailLabelsByIdentifier removeAllObjects];

    if (_sceneDescriptors.count == 0) {
        UILabel *emptyLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
        emptyLabel.text = @"No other windows detected yet.";
        [_rowsStack addArrangedSubview:emptyLabel];
        return;
    }

    BOOL twoColumns = _sceneDescriptors.count > 2 && !ISHWorkspaceUsesPhoneLayout();
    if (twoColumns) {
        for (NSUInteger index = 0; index < _sceneDescriptors.count; index += 2) {
            UIStackView *row = [UIStackView new];
            row.axis = UILayoutConstraintAxisHorizontal;
            row.spacing = 6;
            row.distribution = UIStackViewDistributionFillEqually;
            [row addArrangedSubview:[self workspaceSceneButtonWithDescriptor:_sceneDescriptors[index]]];
            if (index + 1 < _sceneDescriptors.count) {
                [row addArrangedSubview:[self workspaceSceneButtonWithDescriptor:_sceneDescriptors[index + 1]]];
            } else {
                UIView *spacer = [UIView new];
                spacer.translatesAutoresizingMaskIntoConstraints = NO;
                [row addArrangedSubview:spacer];
            }
            [_rowsStack addArrangedSubview:row];
        }
    } else {
        for (NSDictionary<NSString *, id> *descriptor in _sceneDescriptors) {
            [_rowsStack addArrangedSubview:[self workspaceSceneButtonWithDescriptor:descriptor]];
        }
    }
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Workspaces";
    _trackedButtons = [NSMutableArray array];
    _previewImageViewsByIdentifier = [NSMutableDictionary dictionary];
    _titleLabelsByIdentifier = [NSMutableDictionary dictionary];
    _detailLabelsByIdentifier = [NSMutableDictionary dictionary];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 5;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"WINDOW SWITCHER";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:10],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:10],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-10],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-10],
    ]];

    UIView *listCard = [self workspaceThemeCardView];
    _rowsStack = [UIStackView new];
    _rowsStack.translatesAutoresizingMaskIntoConstraints = NO;
    _rowsStack.axis = UILayoutConstraintAxisVertical;
    _rowsStack.spacing = 6;
    [listCard addSubview:_rowsStack];
    [NSLayoutConstraint activateConstraints:@[
        [_rowsStack.topAnchor constraintEqualToAnchor:listCard.topAnchor constant:8],
        [_rowsStack.leadingAnchor constraintEqualToAnchor:listCard.leadingAnchor constant:8],
        [_rowsStack.trailingAnchor constraintEqualToAnchor:listCard.trailingAnchor constant:-8],
        [_rowsStack.bottomAnchor constraintEqualToAnchor:listCard.bottomAnchor constant:-8],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:listCard];
    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceScenesNotification:)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceScenesNotification:)
                                               name:TerminalRegistryDidChangeNotification
                                             object:nil];
    if (@available(iOS 13.0, *)) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceScenesNotification:)
                                                   name:UISceneDidActivateNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceScenesNotification:)
                                                   name:UISceneDidDisconnectNotification
                                                 object:nil];
    }

    [self refreshWorkspaceScenes];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshWorkspaceScenes];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
    _rowsStack.spacing = ISHWorkspaceDensityValue(4, 6);
}

- (void)refreshWorkspaceScenesNotification:(__unused NSNotification *)notification {
    [self refreshWorkspaceScenes];
}

- (void)refreshWorkspaceScenes {
    if (@available(iOS 13.0, *)) {
        _sceneDescriptors = ISHWorkspaceSceneDescriptors(self.workspaceHostViewController.view.window.windowScene);
        NSUInteger workspaceCount = 0;
        for (NSDictionary<NSString *, id> *descriptor in _sceneDescriptors) {
            if ([descriptor[@"role"] isEqualToString:@"Workspace"])
                workspaceCount += 1;
        }
        _summaryLabel.text = [NSString stringWithFormat:@"%lu windows  •  %lu workspaces",
                              (unsigned long) _sceneDescriptors.count,
                              (unsigned long) workspaceCount];
    } else {
        _sceneDescriptors = @[];
        _summaryLabel.text = @"Scene switching requires iOS 13 windows.";
    }
    [self rebuildSceneButtons];
    [self workspaceApplyTheme];
}

- (void)focusWorkspaceSceneButton:(UIButton *)sender {
    if (@available(iOS 13.0, *)) {
    } else {
        return;
    }
    [self.workspaceHostViewController focusSceneWithPersistentIdentifier:sender.accessibilityIdentifier];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _summaryLabel.textColor = theme[@"accentAlt"];
    for (NSDictionary<NSString *, id> *descriptor in _sceneDescriptors) {
        NSString *identifier = descriptor[@"identifier"];
        UIButton *button = nil;
        for (UIButton *candidate in _trackedButtons) {
            if ([candidate.accessibilityIdentifier isEqualToString:identifier]) {
                button = candidate;
                break;
            }
        }
        if (button == nil)
            continue;
        BOOL current = [descriptor[@"isCurrent"] boolValue];
        button.backgroundColor = current
            ? [theme[@"accent"] colorWithAlphaComponent:0.16]
            : [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
        button.layer.borderColor = (current ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        _previewImageViewsByIdentifier[identifier].image =
            [self scenePreviewImageForDescriptor:descriptor size:CGSizeMake(120, 88)];
        _previewImageViewsByIdentifier[identifier].layer.borderColor =
            (current ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        _titleLabelsByIdentifier[identifier].textColor = current ? theme[@"accent"] : theme[@"primary"];
        _detailLabelsByIdentifier[identifier].textColor = theme[@"secondary"];
    }
}

@end

@implementation WorkspaceClockToolViewController {
    UIView *_heroCard;
    UILabel *_timeLabel;
    UILabel *_dateLabel;
    UILabel *_zoneLabel;
    UIStackView *_stackView;
    NSTimer *_timer;
    NSDateFormatter *_timeFormatter;
    NSDateFormatter *_dateFormatter;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Clock";

    _timeFormatter = [NSDateFormatter new];
    _timeFormatter.timeStyle = NSDateFormatterMediumStyle;
    _timeFormatter.dateStyle = NSDateFormatterNoStyle;
    _dateFormatter = [NSDateFormatter new];
    _dateFormatter.dateStyle = NSDateFormatterFullStyle;
    _dateFormatter.timeStyle = NSDateFormatterNoStyle;

    _heroCard = [self workspaceThemeCardView];
    [self.toolContentView addSubview:_heroCard];

    _stackView = [UIStackView new];
    _stackView.axis = UILayoutConstraintAxisVertical;
    _stackView.spacing = 8;
    _stackView.translatesAutoresizingMaskIntoConstraints = NO;
    _stackView.alignment = UIStackViewAlignmentCenter;
    [_heroCard addSubview:_stackView];

    _timeLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleLargeTitle monospaced:YES];
    _timeLabel.numberOfLines = 1;
    _timeLabel.adjustsFontSizeToFitWidth = YES;
    _timeLabel.minimumScaleFactor = 0.5;

    _dateLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleTitle3 monospaced:NO];
    _dateLabel.numberOfLines = 0;
    _dateLabel.textAlignment = NSTextAlignmentCenter;
    _zoneLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _zoneLabel.textAlignment = NSTextAlignmentCenter;

    [_stackView addArrangedSubview:_timeLabel];
    [_stackView addArrangedSubview:_dateLabel];
    [_stackView addArrangedSubview:_zoneLabel];

    [NSLayoutConstraint activateConstraints:@[
        [_heroCard.centerXAnchor constraintEqualToAnchor:self.toolContentView.centerXAnchor],
        [_heroCard.centerYAnchor constraintEqualToAnchor:self.toolContentView.centerYAnchor],
        [_heroCard.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.toolContentView.leadingAnchor constant:10],
        [_heroCard.trailingAnchor constraintLessThanOrEqualToAnchor:self.toolContentView.trailingAnchor constant:-10],
        [_heroCard.widthAnchor constraintLessThanOrEqualToConstant:280],

        [_stackView.topAnchor constraintEqualToAnchor:_heroCard.topAnchor constant:8],
        [_stackView.leadingAnchor constraintEqualToAnchor:_heroCard.leadingAnchor constant:10],
        [_stackView.trailingAnchor constraintEqualToAnchor:_heroCard.trailingAnchor constant:-10],
        [_stackView.bottomAnchor constraintEqualToAnchor:_heroCard.bottomAnchor constant:-8],
    ]];

    [self refreshClock:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGFloat insetY = ISHWorkspaceDensityValue(4, 12);
    CGFloat insetX = ISHWorkspaceDensityValue(6, 16);
    CGRect bounds = UIEdgeInsetsInsetRect(self.toolContentView.bounds, UIEdgeInsetsMake(insetY, insetX, insetY, insetX));
    CGFloat width = MAX(1, CGRectGetWidth(bounds));
    CGFloat height = MAX(1, CGRectGetHeight(bounds));
    CGFloat timeFontSize = MIN(width * 0.22, height * 0.42);
    timeFontSize = MIN(MAX(timeFontSize, 18), 42);
    CGFloat dateFontSize = MIN(width * 0.055, height * 0.14);
    dateFontSize = MIN(MAX(dateFontSize, 9), 14);

    _timeLabel.font = [UIFont monospacedDigitSystemFontOfSize:timeFontSize weight:UIFontWeightBold];
    _dateLabel.font = [UIFont systemFontOfSize:dateFontSize weight:UIFontWeightSemibold];
    _zoneLabel.font = [UIFont systemFontOfSize:MAX(9, round(dateFontSize * 0.76)) weight:UIFontWeightMedium];
    _stackView.spacing = MAX(2, round(timeFontSize * 0.08));
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1
                                              target:self
                                            selector:@selector(refreshClock:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshClock:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)refreshClock:(id)sender {
    NSDate *now = NSDate.date;
    _timeLabel.text = [_timeFormatter stringFromDate:now];
    _dateLabel.text = [_dateFormatter stringFromDate:now];
    NSString *abbreviation = NSTimeZone.localTimeZone.abbreviation ?: @"Local";
    _zoneLabel.text = [NSString stringWithFormat:@"%@  •  %@", abbreviation, NSTimeZone.localTimeZone.name ?: @"Time Zone"];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _timeLabel.textColor = theme[@"accent"];
    _zoneLabel.textColor = theme[@"accentAlt"];
    _heroCard.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.88];
}

@end

@implementation WorkspaceInfoToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UIStackView *_topRow;
    UIStackView *_bottomRow;
    UILabel *_batteryValueLabel;
    UILabel *_rootValueLabel;
    UILabel *_storageValueLabel;
    UILabel *_startupValueLabel;
    NSTimer *_timer;
}

- (UIView *)infoMetricCardWithTitle:(NSString *)title valueLabel:(UILabel * __strong *)valueLabel {
    UIView *card = [self workspaceThemeCardView];
    CGFloat stackInset = ISHWorkspaceUsesPhoneLayout() ? 6.0 : 7.0;
    CGFloat minimumHeight = ISHWorkspaceUsesPhoneLayout() ? 50.0 : 58.0;

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 6;
    [card addSubview:stack];

    UILabel *titleLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    titleLabel.text = [title uppercaseString];
    titleLabel.font = [UIFont systemFontOfSize:8 weight:UIFontWeightSemibold];
    titleLabel.textAlignment = NSTextAlignmentCenter;

    UILabel *label = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleTitle3 monospaced:NO];
    label.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 2;
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.55;

    [stack addArrangedSubview:titleLabel];
    [stack addArrangedSubview:label];

    [NSLayoutConstraint activateConstraints:@[
        [card.heightAnchor constraintGreaterThanOrEqualToConstant:minimumHeight],
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:stackInset],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:stackInset],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-stackInset],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-stackInset],
    ]];

    if (valueLabel != NULL)
        *valueLabel = label;
    return card;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Info";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 6;
    [_scrollView addSubview:_contentStack];

    _topRow = [UIStackView new];
    _topRow.axis = UILayoutConstraintAxisHorizontal;
    _topRow.spacing = 6;
    _topRow.distribution = UIStackViewDistributionFillEqually;
    [_topRow addArrangedSubview:[self infoMetricCardWithTitle:@"Battery" valueLabel:&_batteryValueLabel]];
    [_topRow addArrangedSubview:[self infoMetricCardWithTitle:@"Root" valueLabel:&_rootValueLabel]];

    _bottomRow = [UIStackView new];
    _bottomRow.axis = UILayoutConstraintAxisHorizontal;
    _bottomRow.spacing = 6;
    _bottomRow.distribution = UIStackViewDistributionFillEqually;
    [_bottomRow addArrangedSubview:[self infoMetricCardWithTitle:@"Free Storage" valueLabel:&_storageValueLabel]];
    [_bottomRow addArrangedSubview:[self infoMetricCardWithTitle:@"Startup" valueLabel:&_startupValueLabel]];

    [_contentStack addArrangedSubview:_topRow];
    [_contentStack addArrangedSubview:_bottomRow];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshInfo:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    BOOL compactWidth = CGRectGetWidth(self.toolContentView.bounds) < 420;
    _topRow.axis = compactWidth ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    _bottomRow.axis = compactWidth ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    _topRow.distribution = compactWidth ? UIStackViewDistributionFill : UIStackViewDistributionFillEqually;
    _bottomRow.distribution = compactWidth ? UIStackViewDistributionFill : UIStackViewDistributionFillEqually;
    CGFloat spacing = ISHWorkspaceDensityValue(4, 8);
    _contentStack.spacing = spacing;
    _topRow.spacing = spacing;
    _bottomRow.spacing = spacing;
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                              target:self
                                            selector:@selector(refreshInfo:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshInfo:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
    UIDevice.currentDevice.batteryMonitoringEnabled = NO;
}

- (void)refreshInfo:(id)sender {
    if (UIDevice.currentDevice.batteryState == UIDeviceBatteryStateUnknown || UIDevice.currentDevice.batteryLevel < 0) {
        _batteryValueLabel.text = @"Unavailable";
    } else {
        NSString *stateDescription = @"On battery";
        switch (UIDevice.currentDevice.batteryState) {
            case UIDeviceBatteryStateCharging:
                stateDescription = @"Charging";
                break;
            case UIDeviceBatteryStateFull:
                stateDescription = @"Fully charged";
                break;
            case UIDeviceBatteryStateUnplugged:
                stateDescription = @"On battery";
                break;
            case UIDeviceBatteryStateUnknown:
                break;
        }
        NSInteger percent = (NSInteger) llround(UIDevice.currentDevice.batteryLevel * 100.0);
        _batteryValueLabel.text = [NSString stringWithFormat:@"%ld%%\n%@", (long) percent, stateDescription];
    }

    NSString *defaultRoot = Roots.instance.defaultRoot;
    _rootValueLabel.text = defaultRoot.length > 0 ? defaultRoot : @"Unavailable";

    NSDictionary<NSFileAttributeKey, id> *attributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = attributes[NSFileSystemFreeSize];
    if (freeSize != nil) {
        NSString *formattedSize = [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue
                                                                  countStyle:NSByteCountFormatterCountStyleFile];
        _storageValueLabel.text = formattedSize;
    } else {
        _storageValueLabel.text = @"Unavailable";
    }

    _startupValueLabel.text = ISHInitialWindowTitle();
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _batteryValueLabel.textColor = theme[@"accent"];
    _storageValueLabel.textColor = theme[@"accentAlt"];
}

@end

@implementation WorkspaceMonitorToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_heroSummaryLabel;
    UIProgressView *_cpuProgressView;
    UILabel *_cpuTitleLabel;
    UIProgressView *_memoryProgressView;
    UILabel *_memoryTitleLabel;
    UILabel *_uptimeValueLabel;
    UILabel *_batteryValueLabel;
    UILabel *_diskValueLabel;
    UILabel *_rootValueLabel;
    UILabel *_liveValueLabel;
    UILabel *_networkValueLabel;
    NSTimer *_timer;
    natural_t _previousCPUTicks[CPU_STATE_MAX];
    BOOL _hasPreviousCPUSample;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Monitor";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *heroCard = [self workspaceThemeCardView];
    UIStackView *heroStack = [UIStackView new];
    heroStack.translatesAutoresizingMaskIntoConstraints = NO;
    heroStack.axis = UILayoutConstraintAxisVertical;
    heroStack.spacing = 6;
    [heroCard addSubview:heroStack];
    UILabel *heroLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    heroLabel.text = @"LIVE RUNTIME";
    heroLabel.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _heroSummaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleTitle3 monospaced:NO];
    _heroSummaryLabel.textAlignment = NSTextAlignmentLeft;
    [heroStack addArrangedSubview:heroLabel];
    [heroStack addArrangedSubview:_heroSummaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [heroStack.topAnchor constraintEqualToAnchor:heroCard.topAnchor constant:12],
        [heroStack.leadingAnchor constraintEqualToAnchor:heroCard.leadingAnchor constant:12],
        [heroStack.trailingAnchor constraintEqualToAnchor:heroCard.trailingAnchor constant:-12],
        [heroStack.bottomAnchor constraintEqualToAnchor:heroCard.bottomAnchor constant:-12],
    ]];
    [_contentStack addArrangedSubview:heroCard];

    [_contentStack addArrangedSubview:[self monitorBarCardWithTitle:@"CPU"
                                                         titleLabel:&_cpuTitleLabel
                                                       progressView:&_cpuProgressView
                                                        detailLabel:NULL]];
    [_contentStack addArrangedSubview:[self monitorBarCardWithTitle:@"Memory"
                                                         titleLabel:&_memoryTitleLabel
                                                       progressView:&_memoryProgressView
                                                        detailLabel:NULL]];

    UIStackView *factsStack = [UIStackView new];
    factsStack.axis = UILayoutConstraintAxisVertical;
    factsStack.spacing = 4;
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Live" valueLabel:&_liveValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Uptime" valueLabel:&_uptimeValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Battery" valueLabel:&_batteryValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Storage" valueLabel:&_diskValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Root" valueLabel:&_rootValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Network" valueLabel:&_networkValueLabel]];
    [_contentStack addArrangedSubview:[self monitorCardWithContent:factsStack]];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshMonitor:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                              target:self
                                            selector:@selector(refreshMonitor:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshMonitor:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
    UIDevice.currentDevice.batteryMonitoringEnabled = NO;
}

- (UILabel *)monitorValueLabelWithMonospaced:(BOOL)monospaced {
    UILabel *label = monospaced
        ? [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:YES]
        : [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    label.numberOfLines = 1;
    UIFont *font = nil;
    if (monospaced) {
        if (@available(iOS 13.0, *)) {
            font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
        } else {
            font = [UIFont fontWithName:@"Menlo-Regular" size:12] ?: [UIFont systemFontOfSize:12];
        }
    } else {
        font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    }
    label.font = font;
    return label;
}

- (UIView *)monitorCardWithContent:(UIView *)contentView {
    UIView *card = [self workspaceThemeCardView];

    contentView.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contentView];
    [NSLayoutConstraint activateConstraints:@[
        [contentView.topAnchor constraintEqualToAnchor:card.topAnchor constant:6],
        [contentView.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:6],
        [contentView.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-6],
        [contentView.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-6],
    ]];
    return card;
}

- (UIView *)monitorBarCardWithTitle:(NSString *)title
                         titleLabel:(UILabel * __strong *)titleLabel
                       progressView:(UIProgressView * __strong *)progressView
                        detailLabel:(UILabel * __strong *)detailLabel {
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 4;

    UILabel *headingLabel = [self monitorValueLabelWithMonospaced:NO];
    headingLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    headingLabel.text = title;
    [stack addArrangedSubview:headingLabel];

    UIProgressView *bar = [self workspaceThemeProgressView];
    [stack addArrangedSubview:bar];

    if (titleLabel != NULL)
        *titleLabel = headingLabel;
    if (progressView != NULL)
        *progressView = bar;
    if (detailLabel != NULL)
        *detailLabel = nil;
    return [self monitorCardWithContent:stack];
}

- (UIView *)monitorKeyValueRowWithTitle:(NSString *)title valueLabel:(UILabel * __strong *)valueLabel {
    UIStackView *row = [UIStackView new];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 4;
    row.alignment = UIStackViewAlignmentFirstBaseline;

    UILabel *titleLabel = [self monitorValueLabelWithMonospaced:NO];
    titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    titleLabel.text = title;
    titleLabel.textColor = self.workspaceTheme[@"secondary"];

    UILabel *detail = [self monitorValueLabelWithMonospaced:YES];
    detail.textAlignment = NSTextAlignmentRight;

    [row addArrangedSubview:titleLabel];
    [row addArrangedSubview:detail];
    [titleLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [detail setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    if (valueLabel != NULL)
        *valueLabel = detail;
    return row;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

- (double)sampleSystemCPURatio {
    host_cpu_load_info_data_t cpuInfo;
    mach_msg_type_number_t cpuInfoCount = HOST_CPU_LOAD_INFO_COUNT;
    mach_port_t hostPort = mach_host_self();
    kern_return_t result = host_statistics(hostPort, HOST_CPU_LOAD_INFO, (host_info_t) &cpuInfo, &cpuInfoCount);
    mach_port_deallocate(mach_task_self(), hostPort);
    if (result != KERN_SUCCESS)
        return -1.0;

    uint64_t totalTicks = 0;
    uint64_t activeTicks = 0;
    for (NSUInteger index = 0; index < CPU_STATE_MAX; index++) {
        natural_t currentTicks = cpuInfo.cpu_ticks[index];
        natural_t previousTicks = _hasPreviousCPUSample ? _previousCPUTicks[index] : 0;
        uint64_t delta = _hasPreviousCPUSample ? (uint64_t) (currentTicks - previousTicks) : (uint64_t) currentTicks;
        totalTicks += delta;
        if (index != CPU_STATE_IDLE)
            activeTicks += delta;
        _previousCPUTicks[index] = currentTicks;
    }
    _hasPreviousCPUSample = YES;
    if (totalTicks == 0)
        return 0.0;
    return (double) activeTicks / (double) totalTicks;
}

- (void)refreshMonitor:(id)sender {
    double cpuRatio = [self sampleSystemCPURatio];

    uint64_t footprint = 0;
    uint64_t physical = 0;
    BOOL hasMemory = ISHWorkspaceMemoryUsage(&footprint, NULL, &physical);
    double memoryRatio = (hasMemory && physical > 0) ? ((double) footprint / (double) physical) : 0.0;

    NSString *storageString = [ISHWorkspaceStorageSummaryText() stringByReplacingOccurrencesOfString:@"Free storage: "
                                                                                          withString:@""];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    NSString *networkLine = ISHWorkspacePrimaryNetworkLine();

    NSUInteger sceneCount = 0;
    if (@available(iOS 13.0, *)) {
        sceneCount = UIApplication.sharedApplication.connectedScenes.count;
    }

    NSUInteger terminalCount = [Terminal activeTerminals].count;

    if (cpuRatio >= 0.0) {
        _cpuProgressView.progress = (float) cpuRatio;
        _cpuTitleLabel.text = [NSString stringWithFormat:@"CPU  %ld%%", (long) llround(cpuRatio * 100.0)];
    } else {
        _cpuProgressView.progress = 0.0f;
        _cpuTitleLabel.text = @"CPU  unavailable";
    }

    if (hasMemory) {
        _memoryProgressView.progress = (float) memoryRatio;
        _memoryTitleLabel.text = [NSString stringWithFormat:@"Memory  %ld%%", (long) llround(memoryRatio * 100.0)];
    } else {
        _memoryProgressView.progress = 0.0f;
        _memoryTitleLabel.text = @"Memory  unavailable";
    }

    _uptimeValueLabel.text = ISHWorkspaceDurationString(NSProcessInfo.processInfo.systemUptime);
    _batteryValueLabel.text = ISHWorkspaceBatterySummaryText();
    _diskValueLabel.text = storageString;
    _rootValueLabel.text = defaultRoot.length > 0 ? defaultRoot : @"unavailable";
    _liveValueLabel.text = [NSString stringWithFormat:@"%lu scenes   %lu roots   %lu terminals",
                            (unsigned long) sceneCount,
                            (unsigned long) Roots.instance.roots.count,
                            (unsigned long) terminalCount];
    _heroSummaryLabel.text = [NSString stringWithFormat:@"%lu scenes  •  %lu roots  •  %lu terminals",
                              (unsigned long) sceneCount,
                              (unsigned long) Roots.instance.roots.count,
                              (unsigned long) terminalCount];
    _networkValueLabel.text = networkLine;
}

@end

@implementation WorkspaceNetworksToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UITextView *_textView;
    NSTimer *_timer;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Networks";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 6;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"CONNECTIVITY";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:12],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:12],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-12],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-12],
    ]];

    UIView *detailsCard = [self workspaceThemeCardView];
    _textView = [self workspaceThemeTextView];
    [detailsCard addSubview:_textView];
    [NSLayoutConstraint activateConstraints:@[
        [detailsCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 92.0 : 110.0)],
        [_textView.topAnchor constraintEqualToAnchor:detailsCard.topAnchor constant:8],
        [_textView.leadingAnchor constraintEqualToAnchor:detailsCard.leadingAnchor constant:8],
        [_textView.trailingAnchor constraintEqualToAnchor:detailsCard.trailingAnchor constant:-8],
        [_textView.bottomAnchor constraintEqualToAnchor:detailsCard.bottomAnchor constant:-8],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:detailsCard];
    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshNetworks:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                              target:self
                                            selector:@selector(refreshNetworks:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshNetworks:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)refreshNetworks:(id)sender {
    NSString *summary = ISHWorkspaceNetworkSummaryText();
    NSArray<NSString *> *lines = [summary componentsSeparatedByString:@"\n"];
    _summaryLabel.text = lines.firstObject ?: @"Network unavailable";
    if (lines.count > 1) {
        _textView.text = [[lines subarrayWithRange:NSMakeRange(1, lines.count - 1)] componentsJoinedByString:@"\n"];
    } else {
        _textView.text = @"No active interfaces to display.";
    }
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    _summaryLabel.textColor = self.workspaceTheme[@"accentAlt"];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

@end

@implementation WorkspaceStatusToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_heroValueLabel;
    UITextView *_runtimeTextView;
    UITextView *_networkTextView;
    UITextView *_eventsTextView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"System Status";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *heroCard = [self workspaceThemeCardView];
    UIStackView *heroStack = [UIStackView new];
    heroStack.translatesAutoresizingMaskIntoConstraints = NO;
    heroStack.axis = UILayoutConstraintAxisVertical;
    heroStack.spacing = 6;
    [heroCard addSubview:heroStack];
    UILabel *heroTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    heroTitle.text = @"SYSTEM STATUS";
    heroTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _heroValueLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _heroValueLabel.numberOfLines = 0;
    [heroStack addArrangedSubview:heroTitle];
    [heroStack addArrangedSubview:_heroValueLabel];
    [NSLayoutConstraint activateConstraints:@[
        [heroStack.topAnchor constraintEqualToAnchor:heroCard.topAnchor constant:12],
        [heroStack.leadingAnchor constraintEqualToAnchor:heroCard.leadingAnchor constant:12],
        [heroStack.trailingAnchor constraintEqualToAnchor:heroCard.trailingAnchor constant:-12],
        [heroStack.bottomAnchor constraintEqualToAnchor:heroCard.bottomAnchor constant:-12],
    ]];

    UIView *runtimeCard = [self workspaceThemeCardView];
    _runtimeTextView = [self workspaceThemeTextView];
    [runtimeCard addSubview:_runtimeTextView];
    [NSLayoutConstraint activateConstraints:@[
        [runtimeCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 76.0 : 94.0)],
        [_runtimeTextView.topAnchor constraintEqualToAnchor:runtimeCard.topAnchor constant:8],
        [_runtimeTextView.leadingAnchor constraintEqualToAnchor:runtimeCard.leadingAnchor constant:8],
        [_runtimeTextView.trailingAnchor constraintEqualToAnchor:runtimeCard.trailingAnchor constant:-8],
        [_runtimeTextView.bottomAnchor constraintEqualToAnchor:runtimeCard.bottomAnchor constant:-8],
    ]];

    UIView *networkCard = [self workspaceThemeCardView];
    _networkTextView = [self workspaceThemeTextView];
    [networkCard addSubview:_networkTextView];
    [NSLayoutConstraint activateConstraints:@[
        [networkCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 68.0 : 82.0)],
        [_networkTextView.topAnchor constraintEqualToAnchor:networkCard.topAnchor constant:8],
        [_networkTextView.leadingAnchor constraintEqualToAnchor:networkCard.leadingAnchor constant:8],
        [_networkTextView.trailingAnchor constraintEqualToAnchor:networkCard.trailingAnchor constant:-8],
        [_networkTextView.bottomAnchor constraintEqualToAnchor:networkCard.bottomAnchor constant:-8],
    ]];

    UIView *eventsCard = [self workspaceThemeCardView];
    _eventsTextView = [self workspaceThemeTextView];
    [eventsCard addSubview:_eventsTextView];
    [NSLayoutConstraint activateConstraints:@[
        [eventsCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 74.0 : 90.0)],
        [_eventsTextView.topAnchor constraintEqualToAnchor:eventsCard.topAnchor constant:8],
        [_eventsTextView.leadingAnchor constraintEqualToAnchor:eventsCard.leadingAnchor constant:8],
        [_eventsTextView.trailingAnchor constraintEqualToAnchor:eventsCard.trailingAnchor constant:-8],
        [_eventsTextView.bottomAnchor constraintEqualToAnchor:eventsCard.bottomAnchor constant:-8],
    ]];

    [_contentStack addArrangedSubview:heroCard];
    [_contentStack addArrangedSubview:runtimeCard];
    [_contentStack addArrangedSubview:networkCard];
    [_contentStack addArrangedSubview:eventsCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshStatus:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshStatus:nil];
}

- (void)refreshStatus:(id)sender {
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    NSString *build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    _heroValueLabel.text = [NSString stringWithFormat:@"iSH-AOK %@ (%@)\n%@ • iOS %@",
                            version,
                            build,
                            UIDevice.currentDevice.model ?: @"Unknown device",
                            UIDevice.currentDevice.systemVersion ?: @"?"];

    NSMutableArray<NSString *> *runtimeLines = [NSMutableArray array];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    [runtimeLines addObject:[NSString stringWithFormat:@"Root: %@",
                             defaultRoot.length > 0 ? defaultRoot : @"unavailable"]];
    [runtimeLines addObject:ISHWorkspaceStorageSummaryText()];
    [runtimeLines addObject:[NSString stringWithFormat:@"Startup: %@", ISHInitialWindowTitle()]];
    [runtimeLines addObject:[NSString stringWithFormat:@"Installed roots: %lu",
                             (unsigned long) Roots.instance.roots.count]];
    [runtimeLines addObject:[NSString stringWithFormat:@"Active terminals: %lu",
                             (unsigned long) Terminal.activeTerminals.count]];
    if (@available(iOS 13.0, *)) {
        [runtimeLines addObject:[NSString stringWithFormat:@"Open scenes: %lu",
                                 (unsigned long) UIApplication.sharedApplication.connectedScenes.count]];
    }
    _runtimeTextView.text = [runtimeLines componentsJoinedByString:@"\n"];
    _networkTextView.text = ISHWorkspaceNetworkSummaryText();

    NSArray<NSDictionary<NSString *, id> *> *breadcrumbs = [ISHDiagnosticsStore recentBreadcrumbsWithLimit:5];
    if (breadcrumbs.count == 0) {
        _eventsTextView.text = @"Recent events:\nNo recent diagnostics breadcrumbs.";
        return;
    }
    NSMutableArray<NSString *> *eventLines = [NSMutableArray arrayWithObject:@"Recent events:"];
    for (NSDictionary<NSString *, id> *entry in breadcrumbs) {
        NSString *event = entry[@"event"] ?: @"event";
        NSString *timestamp = entry[@"timestamp"] ?: @"";
        [eventLines addObject:[NSString stringWithFormat:@"%@  %@", timestamp, event]];
    }
    _eventsTextView.text = [eventLines componentsJoinedByString:@"\n"];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    _heroValueLabel.textColor = self.workspaceTheme[@"accentAlt"];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

@end
