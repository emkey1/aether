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
#include <net/if.h>

@class ISHWorkspaceContainedWindowView;

@interface WorkspaceViewController ()

@property (nonatomic, copy) NSString *initialToolIdentifier;
@property (nonatomic) BOOL didOpenInitialTool;
@property (nonatomic, strong) UIView *desktopSurfaceView;
@property (nonatomic, strong) NSMutableArray<UIView *> *desktopWindows;
@property (nonatomic) NSInteger desktopWindowCascadeIndex;
@property (nonatomic, weak) ISHWorkspaceContainedWindowView *dashboardWindow;
@property (nonatomic) CGSize dashboardExpandedSize;
@property (nonatomic) BOOL dashboardIsCompact;
@property (nonatomic, strong) UILabel *clockLabel;
@property (nonatomic, strong) UILabel *batteryLabel;
@property (nonatomic, strong) UILabel *rootLabel;
@property (nonatomic, strong) UILabel *storageLabel;
@property (nonatomic, strong) UILabel *startupPreferenceLabel;
@property (nonatomic, strong) UILabel *windowSummaryLabel;
@property (nonatomic, strong) UILabel *systemSummaryLabel;
@property (nonatomic, strong) UILabel *networkSummaryLabel;
@property (nonatomic, strong) UILabel *diagnosticsSummaryLabel;
@property (nonatomic, strong) UIStackView *sceneWindowsStack;
@property (nonatomic, strong) UIStackView *activeTerminalsStack;
@property (nonatomic, strong) UILabel *breadcrumbsLabel;
@property (nonatomic, strong) UILabel *summaryLabel;
@property (nonatomic, strong) UIStackView *bodyStack;
@property (nonatomic, strong) UIStackView *leadingColumnStack;
@property (nonatomic, strong) UIStackView *trailingColumnStack;
@property (nonatomic, strong) UIView *statusCard;
@property (nonatomic, strong) UIView *actionsCard;
@property (nonatomic, strong) UIView *toolsCard;
@property (nonatomic, strong) UIView *windowCard;
@property (nonatomic, strong) UIView *systemCard;
@property (nonatomic, strong) UIView *networkCard;
@property (nonatomic, strong) UIView *terminalsCard;
@property (nonatomic, strong) UIView *eventsCard;
@property (nonatomic, strong) NSDateFormatter *timeFormatter;
@property (nonatomic, strong) NSTimer *clockTimer;

- (UISceneSession *)sceneSessionHostingTerminalUUID:(NSUUID *)terminalUUID API_AVAILABLE(ios(13.0));
- (BOOL)focusSceneSession:(UISceneSession *)sceneSession title:(NSString *)title API_AVAILABLE(ios(13.0));

@end

NSString *const ISHInitialWindowWorkspaceValue = @"workspace";
static NSString *const ISHWorkspaceToolClockIdentifier = @"clock";
static NSString *const ISHWorkspaceToolStatusIdentifier = @"status";
static NSString *const ISHWorkspaceToolFilesystemsIdentifier = @"filesystems";
static NSString *const ISHWorkspaceToolSettingsIdentifier = @"settings";
static NSString *const ISHWorkspaceToolDiagnosticsIdentifier = @"diagnostics";
static NSString *const ISHWorkspaceSavedLayoutDefaultsKey = @"ISHWorkspaceSavedLayout";
static NSString *const ISHWorkspaceSavedLayoutKindDashboard = @"dashboard";
static NSString *const ISHWorkspaceSavedLayoutKindTool = @"tool";
static NSString *const ISHWorkspaceSavedLayoutKindTerminal = @"terminal";

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
@property (nonatomic, strong) UIView *resizeHandleView;
@property (nonatomic, copy, nullable) dispatch_block_t closeHandler;
@property (nonatomic, copy, nullable) dispatch_block_t utilityHandler;
@property (nonatomic, weak) TerminalViewController *hostedTerminalViewController;
@property (nonatomic, copy) NSString *workspaceToolIdentifier;
@property (nonatomic) CGSize minimumSize;
@property (nonatomic) CGSize maximumSize;

- (instancetype)initWithTitle:(NSString *)title showsCloseButton:(BOOL)showsCloseButton;
- (void)setUtilityButtonTitle:(nullable NSString *)title handler:(nullable dispatch_block_t)handler;

@end

@implementation ISHWorkspaceContainedWindowView

- (instancetype)initWithTitle:(NSString *)title showsCloseButton:(BOOL)showsCloseButton {
    self = [super initWithFrame:CGRectZero];
    if (self == nil)
        return nil;

    self.autoresizingMask = UIViewAutoresizingNone;
    self.backgroundColor = UIColor.clearColor;
    self.layer.cornerRadius = 22;
    self.layer.masksToBounds = NO;
    self.layer.shadowColor = UIColor.blackColor.CGColor;
    self.layer.shadowOpacity = 0.18;
    self.layer.shadowRadius = 28;
    self.layer.shadowOffset = CGSizeMake(0, 16);
    self.draggable = YES;
    self.resizable = NO;
    self.minimumSize = CGSizeMake(280, 180);
    self.maximumSize = CGSizeZero;

    UIView *panelView = [UIView new];
    panelView.translatesAutoresizingMaskIntoConstraints = NO;
    panelView.layer.cornerRadius = 22;
    panelView.layer.masksToBounds = YES;
    if (@available(iOS 13.0, *)) {
        panelView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        panelView.backgroundColor = UIColor.whiteColor;
    }
    [self addSubview:panelView];

    self.titleBarView = [UIView new];
    self.titleBarView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.titleBarView.backgroundColor = [UIColor.secondarySystemBackgroundColor colorWithAlphaComponent:0.96];
    } else {
        self.titleBarView.backgroundColor = [UIColor colorWithWhite:0.94 alpha:1.0];
    }
    [panelView addSubview:self.titleBarView];

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
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
    self.closeButton.titleLabel.font = [UIFont systemFontOfSize:22 weight:UIFontWeightSemibold];
    self.closeButton.hidden = !showsCloseButton;
    self.closeButton.alpha = showsCloseButton ? 1.0 : 0.0;
    self.closeButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.82];
    self.closeButton.layer.cornerRadius = 16;
    [self.closeButton addTarget:self action:@selector(closePressed:) forControlEvents:UIControlEventTouchUpInside];
    [self.titleBarView addSubview:self.closeButton];

    self.utilityButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.utilityButton.translatesAutoresizingMaskIntoConstraints = NO;
    self.utilityButton.hidden = YES;
    self.utilityButton.alpha = 0.0;
    self.utilityButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.82];
    self.utilityButton.layer.cornerRadius = 16;
    self.utilityButton.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    [self.utilityButton addTarget:self action:@selector(utilityPressed:) forControlEvents:UIControlEventTouchUpInside];
    [self.titleBarView addSubview:self.utilityButton];

    self.contentContainerView = [UIView new];
    self.contentContainerView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.contentContainerView.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.contentContainerView.backgroundColor = UIColor.whiteColor;
    }
    [panelView addSubview:self.contentContainerView];

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
    [panelView addSubview:self.resizeHandleView];

    [NSLayoutConstraint activateConstraints:@[
        [panelView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [panelView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [panelView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [panelView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],

        [self.titleBarView.topAnchor constraintEqualToAnchor:panelView.topAnchor],
        [self.titleBarView.leadingAnchor constraintEqualToAnchor:panelView.leadingAnchor],
        [self.titleBarView.trailingAnchor constraintEqualToAnchor:panelView.trailingAnchor],
        [self.titleBarView.heightAnchor constraintEqualToConstant:46],

        [self.closeButton.leadingAnchor constraintEqualToAnchor:self.titleBarView.leadingAnchor constant:12],
        [self.closeButton.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],
        [self.closeButton.widthAnchor constraintEqualToConstant:32],
        [self.closeButton.heightAnchor constraintEqualToConstant:32],

        [self.utilityButton.trailingAnchor constraintEqualToAnchor:self.titleBarView.trailingAnchor constant:-12],
        [self.utilityButton.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],
        [self.utilityButton.widthAnchor constraintEqualToConstant:44],
        [self.utilityButton.heightAnchor constraintEqualToConstant:32],

        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.titleBarView.leadingAnchor constant:56],
        [self.titleLabel.trailingAnchor constraintEqualToAnchor:self.titleBarView.trailingAnchor constant:-64],
        [self.titleLabel.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],

        [self.contentContainerView.topAnchor constraintEqualToAnchor:self.titleBarView.bottomAnchor],
        [self.contentContainerView.leadingAnchor constraintEqualToAnchor:panelView.leadingAnchor],
        [self.contentContainerView.trailingAnchor constraintEqualToAnchor:panelView.trailingAnchor],
        [self.contentContainerView.bottomAnchor constraintEqualToAnchor:panelView.bottomAnchor],

        [self.resizeHandleView.widthAnchor constraintEqualToConstant:20],
        [self.resizeHandleView.heightAnchor constraintEqualToConstant:20],
        [self.resizeHandleView.trailingAnchor constraintEqualToAnchor:panelView.trailingAnchor constant:-12],
        [self.resizeHandleView.bottomAnchor constraintEqualToAnchor:panelView.bottomAnchor constant:-12],
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

- (void)layoutSubviews {
    [super layoutSubviews];
    UIBezierPath *shadowPath = [UIBezierPath bezierPathWithRoundedRect:self.bounds cornerRadius:22];
    self.layer.shadowPath = shadowPath.CGPath;
}

- (void)bringWindowToFront {
    [self.superview bringSubviewToFront:self];
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

- (void)handlePan:(UIPanGestureRecognizer *)recognizer {
    if (!self.draggable || self.superview == nil)
        return;

    if (recognizer.state == UIGestureRecognizerStateBegan) {
        [self bringWindowToFront];
    }

    CGPoint translation = [recognizer translationInView:self.superview];
    CGPoint updatedCenter = CGPointMake(self.center.x + translation.x, self.center.y + translation.y);
    CGFloat halfWidth = CGRectGetWidth(self.bounds) * 0.5;
    CGFloat halfHeight = CGRectGetHeight(self.bounds) * 0.5;
    CGRect bounds = self.superview.bounds;
    updatedCenter.x = MIN(MAX(updatedCenter.x, halfWidth), CGRectGetWidth(bounds) - halfWidth);
    updatedCenter.y = MIN(MAX(updatedCenter.y, halfHeight), CGRectGetHeight(bounds) - halfHeight);
    self.center = updatedCenter;
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
    CGFloat maxWidth = CGRectGetWidth(self.superview.bounds) - CGRectGetMinX(frame);
    CGFloat maxHeight = CGRectGetHeight(self.superview.bounds) - CGRectGetMinY(frame);
    CGSize minimumSize = self.minimumSize;
    CGFloat targetWidth = MAX(minimumSize.width, CGRectGetWidth(frame) + translation.x);
    CGFloat targetHeight = MAX(minimumSize.height, CGRectGetHeight(frame) + translation.y);
    if (self.maximumSize.width > 0) {
        targetWidth = MIN(targetWidth, self.maximumSize.width);
    }
    if (self.maximumSize.height > 0) {
        targetHeight = MIN(targetHeight, self.maximumSize.height);
    }
    frame.size.width = MIN(targetWidth, maxWidth);
    frame.size.height = MIN(targetHeight, maxHeight);
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
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return CGSizeMake(256, 176);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return CGSizeMake(720, 560);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return CGSizeMake(760, 700);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return CGSizeMake(760, 720);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
        return CGSizeMake(760, 760);
    return CGSizeMake(720, 640);
}

static CGSize ISHWorkspacePreferredTerminalContentSize(void) {
    return CGSizeMake(900, 620);
}

static CGSize ISHWorkspaceCompactDashboardSize(void) {
    return CGSizeMake(440, 320);
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
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return @"System Status";
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
    if ([initialWindow isEqualToString:@"session-shell"])
        return @"Session Shell (pts/0)";
    return @"System Console (/dev/console)";
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

static NSString *ISHWorkspaceNetworkSummaryText(void) {
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0 || interfaces == NULL)
        return @"Network: unavailable";

    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSUInteger activeInterfaces = 0;
    BOOL includedLoopback = NO;
    char addressBuffer[INET6_ADDRSTRLEN] = {0};

    for (struct ifaddrs *cursor = interfaces; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_addr == NULL || cursor->ifa_name == NULL)
            continue;
        sa_family_t family = cursor->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6)
            continue;
        if ((cursor->ifa_flags & IFF_UP) == 0)
            continue;

        BOOL isLoopback = (cursor->ifa_flags & IFF_LOOPBACK) != 0;
        if (isLoopback && includedLoopback)
            continue;

        const void *source = family == AF_INET
            ? (const void *) &((const struct sockaddr_in *) cursor->ifa_addr)->sin_addr
            : (const void *) &((const struct sockaddr_in6 *) cursor->ifa_addr)->sin6_addr;
        if (inet_ntop(family, source, addressBuffer, sizeof(addressBuffer)) == NULL)
            continue;

        NSString *interfaceName = [NSString stringWithUTF8String:cursor->ifa_name];
        NSString *address = [NSString stringWithUTF8String:addressBuffer];
        if (isLoopback) {
            includedLoopback = YES;
            [lines addObject:[NSString stringWithFormat:@"Loopback: %@ (%@)", interfaceName, address]];
            continue;
        }

        activeInterfaces += 1;
        NSString *familyName = family == AF_INET6 ? @"IPv6" : @"IPv4";
        [lines addObject:[NSString stringWithFormat:@"%@: %@  %@", interfaceName, familyName, address]];
        if (activeInterfaces >= 3)
            break;
    }

    freeifaddrs(interfaces);

    if (lines.count == 0)
        return @"Network: no active interfaces";

    return [NSString stringWithFormat:@"Active interfaces: %lu\n%@",
                                      (unsigned long) activeInterfaces,
                                      [lines componentsJoinedByString:@"\n"]];
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

@interface WorkspaceClockToolViewController : UIViewController
@end

@interface WorkspaceStatusToolViewController : UIViewController
@end

static UIViewController *ISHCreateWorkspaceToolViewController(NSString *toolIdentifier) {
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return [WorkspaceClockToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return [WorkspaceStatusToolViewController new];
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
    if ([viewController isKindOfClass:WorkspaceStatusToolViewController.class])
        return ISHWorkspaceToolStatusIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"DiagnosticsViewController")])
        return ISHWorkspaceToolDiagnosticsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"AboutViewController")])
        return ISHWorkspaceToolSettingsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"RootsTableViewController")])
        return ISHWorkspaceToolFilesystemsIdentifier;
    return nil;
}

@implementation WorkspaceViewController

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

- (UIButton *)workspaceCompactActionButtonWithTitle:(NSString *)title selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (UIStackView *)workspaceToolLauncherRowWithTitle:(NSString *)title
                                          subtitle:(NSString *)subtitle
                                    toolIdentifier:(NSString *)toolIdentifier {
    UIStackView *row = [UIStackView new];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 10;
    row.alignment = UIStackViewAlignmentCenter;

    UIStackView *labelStack = [UIStackView new];
    labelStack.axis = UILayoutConstraintAxisVertical;
    labelStack.spacing = 2;

    UILabel *titleLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    titleLabel.text = title;
    UILabel *subtitleLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    if (@available(iOS 13.0, *)) {
        subtitleLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        subtitleLabel.textColor = UIColor.darkGrayColor;
    }
    subtitleLabel.text = subtitle;
    [labelStack addArrangedSubview:titleLabel];
    [labelStack addArrangedSubview:subtitleLabel];

    UIButton *hereButton = [self workspaceCompactActionButtonWithTitle:@"Here"
                                                              selector:@selector(openWorkspaceToolHereFromButton:)];
    hereButton.accessibilityIdentifier = toolIdentifier;
    UIButton *windowButton = [self workspaceCompactActionButtonWithTitle:@"Window"
                                                                selector:@selector(openWorkspaceToolWindowFromButton:)];
    windowButton.accessibilityIdentifier = toolIdentifier;

    [row addArrangedSubview:labelStack];
    [row addArrangedSubview:hereButton];
    [row addArrangedSubview:windowButton];
    [labelStack setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [labelStack setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [hereButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [windowButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return row;
}

- (UIView *)workspaceCardWithContentStack:(UIStackView **)contentStackOut {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.layer.cornerRadius = 18;
    card.layer.masksToBounds = NO;
    card.layer.shadowColor = UIColor.blackColor.CGColor;
    card.layer.shadowOpacity = 0.08;
    card.layer.shadowRadius = 18;
    card.layer.shadowOffset = CGSizeMake(0, 8);
    if (@available(iOS 13.0, *)) {
        card.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        card.backgroundColor = [UIColor colorWithWhite:0.96 alpha:1.0];
    }

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = 14;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contentStack];

    [NSLayoutConstraint activateConstraints:@[
        [contentStack.topAnchor constraintEqualToAnchor:card.topAnchor constant:18],
        [contentStack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18],
        [contentStack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18],
        [contentStack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-18],
    ]];

    if (contentStackOut != NULL)
        *contentStackOut = contentStack;
    return card;
}

- (CGRect)desktopUsableBounds {
    UIEdgeInsets insets = self.view.safeAreaInsets;
    CGRect bounds = self.desktopSurfaceView.bounds;
    return UIEdgeInsetsInsetRect(bounds, UIEdgeInsetsMake(insets.top, 0, 0, 0));
}

- (CGRect)desktopFrameForWindowWithPreferredSize:(CGSize)preferredSize {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat width = MIN(preferredSize.width, CGRectGetWidth(usableBounds));
    CGFloat height = MIN(preferredSize.height, CGRectGetHeight(usableBounds));
    CGFloat offset = (CGFloat) (self.desktopWindowCascadeIndex % 6) * 28.0;
    CGFloat originX = CGRectGetMinX(usableBounds) + MAX(0, (CGRectGetWidth(usableBounds) - width) * 0.5) + offset;
    CGFloat originY = CGRectGetMinY(usableBounds) + MAX(0, (CGRectGetHeight(usableBounds) - height) * 0.16) + offset;
    originX = MIN(originX, CGRectGetMaxX(usableBounds) - width);
    originY = MIN(originY, CGRectGetMaxY(usableBounds) - height);
    self.desktopWindowCascadeIndex += 1;
    return CGRectIntegral(CGRectMake(originX, originY, width, height));
}

- (void)clampDesktopWindowToVisibleBounds:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.superview == nil || CGRectIsEmpty(windowView.bounds))
        return;

    CGRect usableBounds = [self desktopUsableBounds];
    CGRect frame = windowView.frame;
    if (CGRectGetWidth(frame) > CGRectGetWidth(usableBounds))
        frame.size.width = CGRectGetWidth(usableBounds);
    if (CGRectGetHeight(frame) > CGRectGetHeight(usableBounds))
        frame.size.height = CGRectGetHeight(usableBounds);
    frame.origin.x = MIN(MAX(frame.origin.x, CGRectGetMinX(usableBounds)), CGRectGetMaxX(usableBounds) - CGRectGetWidth(frame));
    frame.origin.y = MIN(MAX(frame.origin.y, CGRectGetMinY(usableBounds)), CGRectGetMaxY(usableBounds) - CGRectGetHeight(frame));
    windowView.frame = CGRectIntegral(frame);
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
    if (CGRectGetMaxX(frame) > CGRectGetMaxX(usableBounds)) {
        frame.origin.x = MAX(CGRectGetMinX(usableBounds), CGRectGetMaxX(usableBounds) - width);
    }
    if (CGRectGetMaxY(frame) > CGRectGetMaxY(usableBounds)) {
        frame.origin.y = MAX(CGRectGetMinY(usableBounds), CGRectGetMaxY(usableBounds) - height);
    }
    frame = CGRectIntegral(frame);
    windowView.preferredSize = frame.size;
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
    return windowView;
}

- (void)attachViewController:(UIViewController *)viewController toDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
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

- (NSDictionary<NSString *, id> *)savedLayoutDescriptorForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    NSDictionary<NSString *, NSNumber *> *frameDescriptor = [self normalizedFrameDescriptorForFrame:windowView.frame];
    if (frameDescriptor == nil)
        return nil;

    if (windowView == self.dashboardWindow) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindDashboard,
            @"frame": frameDescriptor,
            @"compact": @(self.dashboardIsCompact),
            @"expandedSize": ISHWorkspaceSizeDescriptor(self.dashboardExpandedSize),
        };
    }

    if (windowView.hostedTerminalViewController != nil) {
        NSUUID *terminalUUID = [self persistentTerminalUUIDForWindow:windowView];
        if (terminalUUID == nil)
            return nil;
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindTerminal,
            @"frame": frameDescriptor,
            @"terminalUUID": terminalUUID.UUIDString,
        };
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
        if (windowView == self.dashboardWindow)
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
}

- (ISHWorkspaceContainedWindowView *)restoreDesktopTerminalWindowWithUUID:(NSUUID *)terminalUUID {
    if (terminalUUID == nil)
        return nil;

    if (@available(iOS 13.0, *)) {
        UISceneSession *existingSession = [self sceneSessionHostingTerminalUUID:terminalUUID];
        UISceneSession *currentSession = self.view.window.windowScene.session;
        if (existingSession != nil && existingSession != currentSession)
            return nil;
    }

    ISHWorkspaceContainedWindowView *containedWindow = [self desktopWindowHostingTerminalUUID:terminalUUID];
    if (containedWindow != nil)
        return containedWindow;

    Terminal *terminal = [Terminal terminalWithUUID:terminalUUID];
    if (terminal != nil && terminal.webView.superview != nil)
        return nil;

    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil)
        return nil;

    NSString *title = terminal != nil ? ISHWorkspaceTerminalDisplayName(terminal) : @"Session Shell";
    ISHWorkspaceContainedWindowView *windowView =
        [self openDesktopTerminalWindowWithTitle:title terminalViewController:terminalViewController];
    [terminalViewController reconnectSessionFromTerminalUUID:terminalUUID];
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
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier]) {
        windowView.resizable = YES;
        windowView.minimumSize = CGSizeMake(200, 140);
    }
    [self attachViewController:viewController toDesktopWindow:windowView];
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
    NSMutableArray<NSDictionary<NSString *, id> *> *windowDescriptors = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *descriptor in layout) {
        NSString *kind = descriptor[@"kind"];
        if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindDashboard]) {
            dashboardDescriptor = descriptor;
        } else {
            [windowDescriptors addObject:descriptor];
        }
    }

    [self closeAllRestorableDesktopWindows];
    if (dashboardDescriptor != nil)
        [self applySavedDashboardDescriptor:dashboardDescriptor];

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
            NSUUID *terminalUUID = [[NSUUID alloc] initWithUUIDString:descriptor[@"terminalUUID"]];
            if (terminalUUID == nil)
                continue;
            ISHWorkspaceContainedWindowView *windowView = [self restoreDesktopTerminalWindowWithUUID:terminalUUID];
            if (windowView != nil) {
                [self applySavedFrameDescriptor:frameDescriptor
                                       toWindow:windowView
                                   fallbackSize:ISHWorkspacePreferredTerminalContentSize()];
            }
        }
    }

    [self refreshWorkspaceStatus];
}

- (void)focusDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil)
        return;
    [self.desktopSurfaceView bringSubviewToFront:windowView];
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
    windowView.minimumSize = CGSizeMake(520, 340);
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
    [self workspaceClearArrangedSubviewsFromStack:self.leadingColumnStack];
    [self workspaceClearArrangedSubviewsFromStack:self.trailingColumnStack];

    BOOL useTwoColumns = self.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular;
    self.bodyStack.axis = useTwoColumns ? UILayoutConstraintAxisHorizontal : UILayoutConstraintAxisVertical;
    self.bodyStack.spacing = 20;
    self.bodyStack.alignment = UIStackViewAlignmentFill;
    self.bodyStack.distribution = useTwoColumns ? UIStackViewDistributionFillEqually : UIStackViewDistributionFill;

    if (useTwoColumns) {
        [self.leadingColumnStack addArrangedSubview:self.actionsCard];
        [self.leadingColumnStack addArrangedSubview:self.toolsCard];
        [self.leadingColumnStack addArrangedSubview:self.eventsCard];
        [self.trailingColumnStack addArrangedSubview:self.windowCard];
        [self.trailingColumnStack addArrangedSubview:self.systemCard];
        [self.trailingColumnStack addArrangedSubview:self.networkCard];
        [self.trailingColumnStack addArrangedSubview:self.terminalsCard];
        [self.bodyStack addArrangedSubview:self.leadingColumnStack];
        [self.bodyStack addArrangedSubview:self.trailingColumnStack];
    } else {
        [self.bodyStack addArrangedSubview:self.actionsCard];
        [self.bodyStack addArrangedSubview:self.toolsCard];
        [self.bodyStack addArrangedSubview:self.windowCard];
        [self.bodyStack addArrangedSubview:self.systemCard];
        [self.bodyStack addArrangedSubview:self.networkCard];
        [self.bodyStack addArrangedSubview:self.terminalsCard];
        [self.bodyStack addArrangedSubview:self.eventsCard];
    }
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

    self.timeFormatter = [NSDateFormatter new];
    self.timeFormatter.dateStyle = NSDateFormatterMediumStyle;
    self.timeFormatter.timeStyle = NSDateFormatterMediumStyle;

    ISHWorkspaceContainedWindowView *dashboardWindow =
        [self createDesktopWindowWithTitle:@"Dashboard"
                             preferredSize:CGSizeMake(960, 760)
                          showsCloseButton:NO];
    self.dashboardWindow = dashboardWindow;
    self.dashboardExpandedSize = dashboardWindow.preferredSize;
    self.dashboardIsCompact = NO;
    dashboardWindow.draggable = YES;
    dashboardWindow.resizable = YES;
    dashboardWindow.minimumSize = CGSizeMake(420, 96);
    __weak typeof(self) weakSelf = self;
    [dashboardWindow setUtilityButtonTitle:@"Mini" handler:^{
        typeof(self) strongSelf = weakSelf;
        ISHWorkspaceContainedWindowView *strongDashboardWindow = strongSelf.dashboardWindow;
        if (strongSelf == nil || strongDashboardWindow == nil)
            return;
        if (strongSelf.dashboardIsCompact) {
            strongSelf.dashboardIsCompact = NO;
            [strongDashboardWindow setUtilityButtonTitle:@"Mini" handler:strongDashboardWindow.utilityHandler];
            [strongSelf resizeDesktopWindow:strongDashboardWindow
                                     toSize:strongSelf.dashboardExpandedSize
                                   animated:YES];
            return;
        }
        strongSelf.dashboardExpandedSize = strongDashboardWindow.bounds.size;
        strongSelf.dashboardIsCompact = YES;
        [strongDashboardWindow setUtilityButtonTitle:@"Full" handler:strongDashboardWindow.utilityHandler];
        [strongSelf resizeDesktopWindow:strongDashboardWindow
                                 toSize:ISHWorkspaceCompactDashboardSize()
                               animated:YES];
    }];

    UIScrollView *scrollView = [UIScrollView new];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [dashboardWindow.contentContainerView addSubview:scrollView];

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = 20;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [scrollView addSubview:contentStack];

    self.clockLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleTitle1 monospaced:YES];
    self.batteryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.rootLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.storageLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.startupPreferenceLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.windowSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.systemSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.networkSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.diagnosticsSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.sceneWindowsStack = [UIStackView new];
    self.sceneWindowsStack.axis = UILayoutConstraintAxisVertical;
    self.sceneWindowsStack.spacing = 10;
    self.sceneWindowsStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.breadcrumbsLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.summaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.summaryLabel.text = @"Widgets and layout stay native ARM. Terminal sessions remain guest-backed, and you can launch either console-focused or session-focused terminals from here.";

    self.activeTerminalsStack = [UIStackView new];
    self.activeTerminalsStack.axis = UILayoutConstraintAxisVertical;
    self.activeTerminalsStack.spacing = 10;
    self.activeTerminalsStack.translatesAutoresizingMaskIntoConstraints = NO;
    UIStackView *statusStack = nil;
    self.statusCard = [self workspaceCardWithContentStack:&statusStack];
    UILabel *headlineLabel = [self workspaceSectionTitle:@"Native workspace"];
    UILabel *statusSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    if (@available(iOS 13.0, *)) {
        statusSummaryLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        statusSummaryLabel.textColor = UIColor.darkGrayColor;
    }
    statusSummaryLabel.text = @"A native ARM dashboard for windows, terminals, and support surfaces.";
    [statusStack addArrangedSubview:headlineLabel];
    [statusStack addArrangedSubview:self.clockLabel];
    [statusStack addArrangedSubview:statusSummaryLabel];
    [statusStack addArrangedSubview:self.batteryLabel];
    [statusStack addArrangedSubview:self.rootLabel];
    [statusStack addArrangedSubview:self.storageLabel];
    [statusStack addArrangedSubview:self.startupPreferenceLabel];

    UIStackView *actionsStack = nil;
    self.actionsCard = [self workspaceCardWithContentStack:&actionsStack];
    [actionsStack addArrangedSubview:[self workspaceSectionTitle:@"Terminals"]];
    [actionsStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Open System Console Here"
                                                                selector:@selector(openSystemConsoleHere:)]];
    [actionsStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Open Session Shell Here"
                                                                selector:@selector(openSessionShellHere:)]];
    [actionsStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Open Preferred Terminal Here"
                                                                selector:@selector(openTerminalHere:)]];

    UIStackView *toolsStack = nil;
    self.toolsCard = [self workspaceCardWithContentStack:&toolsStack];
    [toolsStack addArrangedSubview:[self workspaceSectionTitle:@"Native apps"]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Clock"
                                                                  subtitle:@"Large clock view"
                                                            toolIdentifier:ISHWorkspaceToolClockIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"System Status"
                                                                  subtitle:@"Device, root, and session summary"
                                                            toolIdentifier:ISHWorkspaceToolStatusIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Filesystems"
                                                                  subtitle:@"Manage installed roots"
                                                            toolIdentifier:ISHWorkspaceToolFilesystemsIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Settings"
                                                                  subtitle:@"App configuration and preferences"
                                                            toolIdentifier:ISHWorkspaceToolSettingsIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Diagnostics"
                                                                  subtitle:@"Crash, MetricKit, and breadcrumb data"
                                                            toolIdentifier:ISHWorkspaceToolDiagnosticsIdentifier]];

    UIStackView *windowCardStack = nil;
    self.windowCard = [self workspaceCardWithContentStack:&windowCardStack];
    [windowCardStack addArrangedSubview:[self workspaceSectionTitle:@"Window overview"]];
    [windowCardStack addArrangedSubview:self.windowSummaryLabel];
    [windowCardStack addArrangedSubview:self.sceneWindowsStack];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Save Current Layout"
                                                                   selector:@selector(saveWorkspaceLayout:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Restore Saved Layout"
                                                                   selector:@selector(restoreWorkspaceLayout:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"New Terminal Window"
                                                                   selector:@selector(openNewTerminalWindow:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"New Workspace Window"
                                                                   selector:@selector(openNewWorkspaceWindow:)]];

    UIStackView *systemCardStack = nil;
    self.systemCard = [self workspaceCardWithContentStack:&systemCardStack];
    [systemCardStack addArrangedSubview:[self workspaceSectionTitle:@"System snapshot"]];
    [systemCardStack addArrangedSubview:self.systemSummaryLabel];
    [systemCardStack addArrangedSubview:self.diagnosticsSummaryLabel];

    UIStackView *networkCardStack = nil;
    self.networkCard = [self workspaceCardWithContentStack:&networkCardStack];
    [networkCardStack addArrangedSubview:[self workspaceSectionTitle:@"Network"]];
    [networkCardStack addArrangedSubview:self.networkSummaryLabel];

    UIStackView *terminalsCardStack = nil;
    self.terminalsCard = [self workspaceCardWithContentStack:&terminalsCardStack];
    [terminalsCardStack addArrangedSubview:[self workspaceSectionTitle:@"Active terminals"]];
    [terminalsCardStack addArrangedSubview:self.activeTerminalsStack];

    UIStackView *eventsCardStack = nil;
    self.eventsCard = [self workspaceCardWithContentStack:&eventsCardStack];
    [eventsCardStack addArrangedSubview:[self workspaceSectionTitle:@"Recent events"]];
    [eventsCardStack addArrangedSubview:self.breadcrumbsLabel];
    [eventsCardStack addArrangedSubview:self.summaryLabel];

    self.bodyStack = [UIStackView new];
    self.bodyStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.leadingColumnStack = [UIStackView new];
    self.leadingColumnStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.leadingColumnStack.axis = UILayoutConstraintAxisVertical;
    self.leadingColumnStack.spacing = 20;
    self.trailingColumnStack = [UIStackView new];
    self.trailingColumnStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.trailingColumnStack.axis = UILayoutConstraintAxisVertical;
    self.trailingColumnStack.spacing = 20;

    [contentStack addArrangedSubview:self.statusCard];
    [contentStack addArrangedSubview:self.bodyStack];
    [self rebuildWorkspaceColumns];

    UILayoutGuide *safeArea = dashboardWindow.contentContainerView.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scrollView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [scrollView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor],

        [contentStack.topAnchor constraintEqualToAnchor:scrollView.topAnchor constant:24],
        [contentStack.leadingAnchor constraintEqualToAnchor:scrollView.leadingAnchor constant:20],
        [contentStack.trailingAnchor constraintEqualToAnchor:scrollView.trailingAnchor constant:-20],
        [contentStack.bottomAnchor constraintEqualToAnchor:scrollView.bottomAnchor constant:-24],
        [contentStack.widthAnchor constraintEqualToAnchor:scrollView.widthAnchor constant:-40],
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
                                               name:ISHDiagnosticsStoreDidUpdateNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:NSUserDefaultsDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:UIDeviceBatteryLevelDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:UIDeviceBatteryStateDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:UIApplicationDidBecomeActiveNotification
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
    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        [self applyInitialFrameIfNeededToDesktopWindow:windowView];
        [self clampDesktopWindowToVisibleBounds:windowView];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    [self refreshWorkspaceStatus];
    [self startClock];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (self.didOpenInitialTool || self.initialToolIdentifier.length == 0)
        return;
    self.didOpenInitialTool = YES;
    [self openWorkspaceToolWithIdentifier:self.initialToolIdentifier];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [self.clockTimer invalidate];
    self.clockTimer = nil;
    UIDevice.currentDevice.batteryMonitoringEnabled = NO;
}

- (void)startClock {
    [self.clockTimer invalidate];
    self.clockTimer = [NSTimer scheduledTimerWithTimeInterval:1
                                                       target:self
                                                     selector:@selector(refreshWorkspaceStatus)
                                                     userInfo:nil
                                                      repeats:YES];
}

- (void)refreshWorkspaceStatus {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self refreshWorkspaceStatus];
        });
        return;
    }

    self.clockLabel.text = [self.timeFormatter stringFromDate:NSDate.date];
    if (UIDevice.currentDevice.batteryState == UIDeviceBatteryStateUnknown || UIDevice.currentDevice.batteryLevel < 0) {
        self.batteryLabel.text = @"Battery: unavailable";
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
        self.batteryLabel.text = [NSString stringWithFormat:@"Battery: %@ (%ld%%)", stateDescription, (long) percent];
    }
    NSString *defaultRoot = Roots.instance.defaultRoot;
    self.rootLabel.text = defaultRoot.length > 0
        ? [NSString stringWithFormat:@"Current root: %@", defaultRoot]
        : @"Current root: unavailable";

    NSDictionary<NSFileAttributeKey, id> *attributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = attributes[NSFileSystemFreeSize];
    if (freeSize != nil) {
        NSString *formattedSize = [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue
                                                                  countStyle:NSByteCountFormatterCountStyleFile];
        self.storageLabel.text = [NSString stringWithFormat:@"Free storage: %@", formattedSize];
    } else {
        self.storageLabel.text = @"Free storage: unavailable";
    }
    self.startupPreferenceLabel.text = [NSString stringWithFormat:@"Startup screen: %@", ISHInitialWindowTitle()];
    [self refreshWindowSummary];
    [self refreshSceneWindows];
    [self refreshSystemSummary];
    self.networkSummaryLabel.text = ISHWorkspaceNetworkSummaryText();

    NSArray<NSDictionary<NSString *, id> *> *breadcrumbs = [ISHDiagnosticsStore recentBreadcrumbsWithLimit:3];
    if (breadcrumbs.count == 0) {
        self.breadcrumbsLabel.text = @"Recent events: none";
    } else {
        NSMutableArray<NSString *> *lines = [NSMutableArray array];
        for (NSDictionary<NSString *, id> *entry in breadcrumbs) {
            NSString *event = entry[@"event"] ?: @"event";
            NSString *timestamp = entry[@"timestamp"] ?: @"";
            [lines addObject:[NSString stringWithFormat:@"%@  %@", timestamp, event]];
        }
        self.breadcrumbsLabel.text = [NSString stringWithFormat:@"Recent events:\n%@", [lines componentsJoinedByString:@"\n"]];
    }
    [self refreshActiveTerminals];
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

- (void)refreshSystemSummary {
    NSMutableArray<NSString *> *systemLines = [NSMutableArray array];
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    NSString *build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    [systemLines addObject:[NSString stringWithFormat:@"App: %@ (%@)", version, build]];
    [systemLines addObject:[NSString stringWithFormat:@"Device: %@ / iOS %@",
                            UIDevice.currentDevice.model ?: @"Unknown",
                            UIDevice.currentDevice.systemVersion ?: @"?"]];
    [systemLines addObject:[NSString stringWithFormat:@"Installed roots: %lu",
                            (unsigned long) Roots.instance.roots.count]];
    self.systemSummaryLabel.text = [systemLines componentsJoinedByString:@"\n"];

    NSArray<NSDictionary<NSString *, id> *> *payloads = [ISHDiagnosticsStore recentMetricKitPayloadsWithLimit:2];
    if (payloads.count == 0) {
        self.diagnosticsSummaryLabel.text = @"Diagnostics: no recent MetricKit payloads";
        return;
    }

    NSDictionary<NSString *, id> *latestPayload = payloads.firstObject;
    NSString *filename = latestPayload[@"filename"] ?: @"payload.json";
    NSString *receivedAt = latestPayload[@"receivedAt"] ?: @"recently";
    NSArray<NSDictionary<NSString *, id> *> *summaries = latestPayload[@"summaries"];
    NSString *topSummary = @"no summaries";
    if ([summaries isKindOfClass:NSArray.class] && summaries.count > 0) {
        NSDictionary<NSString *, id> *entry = summaries.firstObject;
        NSString *kind = entry[@"kind"] ?: @"diagnostic";
        NSString *signal = entry[@"signal"] ?: @"";
        topSummary = signal.length > 0 ? [NSString stringWithFormat:@"%@ / signal %@", kind, signal] : kind;
    }
    self.diagnosticsSummaryLabel.text =
        [NSString stringWithFormat:@"Diagnostics: %lu recent payload%@\nLatest: %@ (%@)\nTop summary: %@",
                                   (unsigned long) payloads.count,
                                   payloads.count == 1 ? @"" : @"s",
                                   filename,
                                   receivedAt,
                                   topSummary];
}

- (void)refreshActiveTerminals {
    for (UIView *subview in self.activeTerminalsStack.arrangedSubviews) {
        [self.activeTerminalsStack removeArrangedSubview:subview];
        [subview removeFromSuperview];
    }

    NSArray<Terminal *> *activeTerminals = [Terminal activeTerminals];
    if (activeTerminals.count == 0) {
        UILabel *emptyLabel = [UILabel new];
        emptyLabel.numberOfLines = 0;
        emptyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
        if (@available(iOS 13.0, *)) {
            emptyLabel.textColor = UIColor.secondaryLabelColor;
        } else {
            emptyLabel.textColor = UIColor.darkGrayColor;
        }
        emptyLabel.text = @"No terminals are active yet.";
        [self.activeTerminalsStack addArrangedSubview:emptyLabel];
        return;
    }

    for (Terminal *terminal in activeTerminals) {
        UIStackView *row = [UIStackView new];
        row.axis = UILayoutConstraintAxisHorizontal;
        row.spacing = 10;
        row.alignment = UIStackViewAlignmentCenter;

        UILabel *label = [UILabel new];
        label.numberOfLines = 0;
        label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        if (@available(iOS 13.0, *)) {
            label.textColor = UIColor.labelColor;
        } else {
            label.textColor = UIColor.blackColor;
        }
        label.text = ISHWorkspaceTerminalDisplayName(terminal);

        UIButton *hereButton = [self terminalActionButtonWithTitle:@"Here"
                                                          selector:@selector(openExistingTerminalHereFromButton:)
                                                      terminalUUID:terminal.uuid];
        UIButton *windowButton = [self terminalActionButtonWithTitle:@"Window"
                                                            selector:@selector(openExistingTerminalInNewWindowFromButton:)
                                                        terminalUUID:terminal.uuid];

        [row addArrangedSubview:label];
        [row addArrangedSubview:hereButton];
        [row addArrangedSubview:windowButton];
        [label setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [hereButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [windowButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

        [self.activeTerminalsStack addArrangedSubview:row];
    }
}

- (UIButton *)terminalActionButtonWithTitle:(NSString *)title
                                   selector:(SEL)selector
                               terminalUUID:(NSUUID *)terminalUUID {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    button.accessibilityIdentifier = terminalUUID.UUIDString;
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier {
    [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
}

- (void)openWorkspaceToolHereFromButton:(UIButton *)sender {
    NSString *toolIdentifier = sender.accessibilityIdentifier;
    if (toolIdentifier.length == 0)
        return;
    [self openWorkspaceToolWithIdentifier:toolIdentifier];
}

- (void)openWorkspaceToolWindowFromButton:(UIButton *)sender {
    NSString *toolIdentifier = sender.accessibilityIdentifier;
    if (toolIdentifier.length == 0) {
        [self presentSceneActivationError:nil title:@"Unable to open app window"];
        return;
    }
    [self requestSceneWithActivityType:ISHSceneActivityTypeWorkspace
                                 title:@"Unable to open app window"
                              userInfo:@{ISHSceneWorkspaceToolUserInfoKey: toolIdentifier}];
}

- (void)openDiagnostics:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolDiagnosticsIdentifier];
}

- (void)openClockTool:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolClockIdentifier];
}

- (void)openSystemStatusTool:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolStatusIdentifier];
}

- (void)openFilesystems:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolFilesystemsIdentifier];
}

- (void)openSettings:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolSettingsIdentifier];
}

- (void)openSystemConsoleHere:(id)sender {
    [self openTerminalHerePreferringConsole:YES];
}

- (void)openSessionShellHere:(id)sender {
    [self openTerminalHerePreferringConsole:NO];
}

- (void)openTerminalHere:(id)sender {
    [self openTerminalHerePreferringConsole:[self shouldPreferConsoleForPreferredLaunch]];
}

- (BOOL)shouldPreferConsoleForPreferredLaunch {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return ![initialWindow isEqualToString:@"session-shell"];
}

- (void)openTerminalHerePreferringConsole:(BOOL)preferConsole {
    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    NSString *title = preferConsole ? @"System Console" : @"Session Shell";
    [self openDesktopTerminalWindowWithTitle:title terminalViewController:terminalViewController];
    [terminalViewController startNewSession];
    if (preferConsole) {
        [terminalViewController showSystemConsoleForCurrentSession];
    } else {
        [terminalViewController showSessionShellForCurrentSession];
    }
}

- (void)openExistingTerminalHereFromButton:(UIButton *)sender {
    NSUUID *terminalUUID = [[NSUUID alloc] initWithUUIDString:sender.accessibilityIdentifier];
    if (terminalUUID == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    [self openExistingTerminalHereWithUUID:terminalUUID];
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

- (void)focusExistingSceneFromButton:(UIButton *)sender {
    if (@available(iOS 13.0, *)) {
        NSString *identifier = sender.accessibilityIdentifier;
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
        return;
    }
    [self presentSceneActivationError:nil title:@"Unable to focus window"];
}

- (void)openExistingTerminalInNewWindowFromButton:(UIButton *)sender {
    NSUUID *terminalUUID = [[NSUUID alloc] initWithUUIDString:sender.accessibilityIdentifier];
    if (terminalUUID == nil) {
        [self presentSceneActivationError:nil title:@"Unable to open terminal window"];
        return;
    }
    if (@available(iOS 13.0, *)) {
        UISceneSession *existingSession = [self sceneSessionHostingTerminalUUID:terminalUUID];
        if ([self focusSceneSession:existingSession title:@"Unable to open terminal window"])
            return;
    }
    ISHWorkspaceContainedWindowView *containedWindow = [self desktopWindowHostingTerminalUUID:terminalUUID];
    if (containedWindow != nil) {
        [self focusDesktopWindow:containedWindow];
        return;
    }
    Terminal *terminal = [Terminal terminalWithUUID:terminalUUID];
    if (terminal == nil) {
        [self presentSceneActivationError:nil title:@"Unable to open terminal window"];
        return;
    }
    if (terminal.webView.superview != nil) {
        [self presentSceneActivationError:nil title:@"Terminal already open in another window"];
        return;
    }
    [self requestSceneWithActivityType:ISHSceneActivityTypeTerminal
                                 title:@"Unable to open terminal window"
                              userInfo:@{ISHSceneTerminalUUIDUserInfoKey: terminalUUID.UUIDString}];
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
}

- (void)openNewTerminalWindow:(id)sender {
    [self requestSceneWithActivityType:ISHSceneActivityTypeTerminal
                               title:@"Unable to open terminal"
                            userInfo:nil];
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

@implementation WorkspaceClockToolViewController {
    UILabel *_timeLabel;
    UILabel *_dateLabel;
    UIStackView *_stackView;
    NSTimer *_timer;
    NSDateFormatter *_timeFormatter;
    NSDateFormatter *_dateFormatter;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Clock";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _timeFormatter = [NSDateFormatter new];
    _timeFormatter.timeStyle = NSDateFormatterMediumStyle;
    _timeFormatter.dateStyle = NSDateFormatterNoStyle;
    _dateFormatter = [NSDateFormatter new];
    _dateFormatter.dateStyle = NSDateFormatterFullStyle;
    _dateFormatter.timeStyle = NSDateFormatterNoStyle;

    _stackView = [UIStackView new];
    _stackView.axis = UILayoutConstraintAxisVertical;
    _stackView.spacing = 18;
    _stackView.translatesAutoresizingMaskIntoConstraints = NO;
    _stackView.alignment = UIStackViewAlignmentCenter;
    [self.view addSubview:_stackView];

    _timeLabel = [UILabel new];
    _timeLabel.numberOfLines = 1;
    _timeLabel.adjustsFontSizeToFitWidth = YES;
    _timeLabel.minimumScaleFactor = 0.5;
    _timeLabel.font = [UIFont monospacedDigitSystemFontOfSize:40 weight:UIFontWeightBold];
    if (@available(iOS 13.0, *)) {
        _timeLabel.textColor = UIColor.labelColor;
    } else {
        _timeLabel.textColor = UIColor.blackColor;
    }

    _dateLabel = [UILabel new];
    _dateLabel.numberOfLines = 0;
    _dateLabel.textAlignment = NSTextAlignmentCenter;
    _dateLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle3];
    if (@available(iOS 13.0, *)) {
        _dateLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        _dateLabel.textColor = UIColor.darkGrayColor;
    }

    [_stackView addArrangedSubview:_timeLabel];
    [_stackView addArrangedSubview:_dateLabel];

    [NSLayoutConstraint activateConstraints:@[
        [_stackView.centerXAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerXAnchor],
        [_stackView.centerYAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerYAnchor],
        [_stackView.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24],
        [_stackView.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24],
    ]];

    [self refreshClock:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGRect bounds = UIEdgeInsetsInsetRect(self.view.safeAreaLayoutGuide.layoutFrame, UIEdgeInsetsMake(12, 16, 12, 16));
    CGFloat width = MAX(1, CGRectGetWidth(bounds));
    CGFloat height = MAX(1, CGRectGetHeight(bounds));
    CGFloat timeFontSize = MIN(width * 0.22, height * 0.34);
    timeFontSize = MIN(MAX(timeFontSize, 24), 56);
    CGFloat dateFontSize = MIN(width * 0.08, height * 0.13);
    dateFontSize = MIN(MAX(dateFontSize, 12), 24);

    _timeLabel.font = [UIFont monospacedDigitSystemFontOfSize:timeFontSize weight:UIFontWeightBold];
    _dateLabel.font = [UIFont systemFontOfSize:dateFontSize weight:UIFontWeightRegular];
    _stackView.spacing = MAX(10, round(timeFontSize * 0.35));
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
}

@end

@implementation WorkspaceStatusToolViewController {
    UITextView *_textView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"System Status";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _textView = [[UITextView alloc] initWithFrame:self.view.bounds];
    _textView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _textView.editable = NO;
    _textView.alwaysBounceVertical = YES;
    if (@available(iOS 13.0, *)) {
        _textView.backgroundColor = UIColor.systemBackgroundColor;
        _textView.textColor = UIColor.labelColor;
        _textView.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    } else {
        _textView.backgroundColor = UIColor.whiteColor;
        _textView.textColor = UIColor.blackColor;
        _textView.font = [UIFont fontWithName:@"Menlo-Regular" size:13] ?: [UIFont systemFontOfSize:13];
    }
    [self.view addSubview:_textView];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                      target:self
                                                      action:@selector(refreshStatus:)];
    [self refreshStatus:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshStatus:nil];
}

- (void)refreshStatus:(id)sender {
    _textView.text = ISHWorkspaceSystemStatusText();
}

@end
