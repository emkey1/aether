//
//  AboutViewController.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "Diagnostics.h"
#import "UserPreferences.h"
#import "iOSFS.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"
#import "UIViewController+Extras.h"

@interface DiagnosticsViewController : UIViewController
@end

@interface AboutViewController ()
@property (weak, nonatomic) IBOutlet UITableViewCell *capsLockMappingCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *themeCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *initialWindowCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *diagnosticsCell;
@property (weak, nonatomic) IBOutlet UISwitch *disableDimmingSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableMulticoreSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableExtraLockingSwitch;
@property (weak, nonatomic) IBOutlet UITextField *launchCommandField;
@property (weak, nonatomic) IBOutlet UITextField *bootCommandField;

@property (weak, nonatomic) IBOutlet UITableViewCell *sendFeedback;
@property (weak, nonatomic) IBOutlet UITableViewCell *openGithub;
@property (weak, nonatomic) IBOutlet UITableViewCell *openDiscord;

@property (weak, nonatomic) IBOutlet UITableViewCell *upgradeApkCell;
@property (weak, nonatomic) IBOutlet UILabel *upgradeApkLabel;
@property (weak, nonatomic) IBOutlet UIView *upgradeApkBadge;
@property (weak, nonatomic) IBOutlet UITableViewCell *exportContainerCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *resetMountsCell;

@property (weak, nonatomic) IBOutlet UILabel *versionLabel;

@end

@implementation DiagnosticsViewController {
    UITextView *_textView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Diagnostics";
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
        _textView.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    } else {
        _textView.backgroundColor = UIColor.whiteColor;
        _textView.textColor = UIColor.blackColor;
        _textView.font = [UIFont fontWithName:@"Menlo-Regular" size:12] ?: [UIFont systemFontOfSize:12];
    }
    [self.view addSubview:_textView];

    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAction
                                                      target:self
                                                      action:@selector(exportDiagnostics:)],
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                      target:self
                                                      action:@selector(refreshDiagnostics:)],
    ];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshDiagnostics:)
                                               name:ISHDiagnosticsStoreDidUpdateNotification
                                             object:nil];
    [self refreshDiagnostics:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDiagnostics:nil];
}

- (void)refreshDiagnostics:(id)sender {
    _textView.text = [ISHDiagnosticsStore diagnosticsReport];
    [_textView setContentOffset:CGPointZero animated:NO];
}

- (void)exportDiagnostics:(id)sender {
    NSError *error = nil;
    NSURL *bundleURL = [ISHDiagnosticsStore prepareExportBundle:&error];
    if (bundleURL == nil) {
        [self presentError:error title:@"Export failed"];
        return;
    }

    UIActivityViewController *activityViewController =
        [[UIActivityViewController alloc] initWithActivityItems:@[bundleURL] applicationActivities:nil];
    UIPopoverPresentationController *popover = activityViewController.popoverPresentationController;
    if (popover != nil) {
        popover.barButtonItem = sender;
    }
    [self presentViewController:activityViewController animated:YES completion:nil];
}

@end

@implementation AboutViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    [self _updateUI];
    if (self.recoveryMode) {
        self.includeDebugPanel = YES;
        self.navigationItem.title = @"Recovery Mode";
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Exit"
                                                                                  style:UIBarButtonItemStyleDone
                                                                                 target:self
                                                                                 action:@selector(exitRecovery:)];
    }
    _versionLabel.text = [NSString stringWithFormat:@"iSH-AOK %@ (Build %@)",
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"]];

    [UserPreferences.shared observe:@[@"capsLockMapping", @"fontSize", @"launchCommand", @"bootCommand", @"shouldLockSleepNanoseconds"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateUI];
        });
    }];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(_updateUI:) name:FsUpdatedNotification object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self _updateUI];
}

- (IBAction)dismiss:(id)sender {
    [self dismissViewControllerAnimated:self completion:nil];
}

- (void)exitRecovery:(id)sender {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"recovery"];
    exit(0);
}

- (void)showDiagnostics:(id)sender {
    DiagnosticsViewController *viewController = [DiagnosticsViewController new];
    [self.navigationController pushViewController:viewController animated:YES];
}

- (void)_updateUI:(NSNotification *)notification {
    [self _updateUI];
}

- (void)_updateUI {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    self.disableDimmingSwitch.on = UserPreferences.shared.shouldDisableDimming;
    self.enableMulticoreSwitch.on = UserPreferences.shared.shouldEnableMulticore;
    self.enableExtraLockingSwitch.on = UserPreferences.shared.shouldEnableExtraLocking;
    self.initialWindowCell.detailTextLabel.text = [self _initialWindowTitle];
    self.launchCommandField.text = [UserPreferences.shared.launchCommand componentsJoinedByString:@" "];
    self.bootCommandField.text = [UserPreferences.shared.bootCommand componentsJoinedByString:@" "];

    self.upgradeApkCell.userInteractionEnabled = FsNeedsRepositoryUpdate();
    self.upgradeApkLabel.enabled = FsNeedsRepositoryUpdate();
    self.upgradeApkBadge.hidden = !FsNeedsRepositoryUpdate();
    self.upgradeApkCell.accessibilityValue = FsNeedsRepositoryUpdate() ? @"Update available" : nil;
    [self.tableView reloadData];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    if (cell == self.sendFeedback) {
        [UIApplication openURL:@"mailto:ish_aok_emkey1@icloud.com?subject=Feedback%20for%20iSH"];
    } else if (cell == self.diagnosticsCell) {
        [self showDiagnostics:cell];
    } else if (cell == self.initialWindowCell) {
        [self _showInitialWindowPickerFromCell:cell];
    } else if (cell == self.openGithub) {
        [UIApplication openURL:@"https://github.com/emkey1/ish-AOK"];
    } else if (cell == self.openDiscord) {
        [UIApplication openURL:@"https://discord.com/channels/776432683302649866/776432683302649870"];
    } else if (cell == self.exportContainerCell) {
        // copy the files to the app container so they can be extracted from iTunes file sharing
        NSURL *container = ContainerURL();
        NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
        [NSFileManager.defaultManager removeItemAtURL:[documents URLByAppendingPathComponent:@"roots copy"] error:nil];
        [NSFileManager.defaultManager copyItemAtURL:[container URLByAppendingPathComponent:@"roots"]
                                              toURL:[documents URLByAppendingPathComponent:@"roots copy"]
                                              error:nil];
    } else if (cell == self.resetMountsCell) {
        iosfs_clear_all_bookmarks();
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (NSString *)_initialWindowPreferenceValue {
    NSString *value = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([value isEqualToString:@"session-shell"])
        return value;
    return @"terminal";
}

- (NSString *)_initialWindowTitle {
    if ([[self _initialWindowPreferenceValue] isEqualToString:@"session-shell"])
        return @"Session Shell (pts/0)";
    return @"System Console (/dev/console)";
}

- (void)_setInitialWindowPreferenceValue:(NSString *)value {
    [NSUserDefaults.standardUserDefaults setObject:value forKey:kPreferenceInitialWindowKey];
    [self _updateUI];
}

- (void)_showInitialWindowPickerFromCell:(UITableViewCell *)cell {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Initial Window"
                                            message:@"Choose which terminal window opens first for new app launches."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    NSString *currentValue = [self _initialWindowPreferenceValue];
    NSString *terminalTitle = [currentValue isEqualToString:@"terminal"]
        ? @"System Console (/dev/console)  Current"
        : @"System Console (/dev/console)";
    NSString *sessionTitle = [currentValue isEqualToString:@"session-shell"]
        ? @"Session Shell (pts/0)  Current"
        : @"Session Shell (pts/0)";

    [alert addAction:[UIAlertAction actionWithTitle:terminalTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"terminal"];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:sessionTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"session-shell"];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = cell;
        popover.sourceRect = cell.bounds;
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 1) { // filesystems / upgrade
        if (!FsIsManaged()) {
            return @"The current filesystem is not managed by iSH.";
        } else if (!FsNeedsRepositoryUpdate()) {
            return [NSString stringWithFormat:@"The current filesystem is using %s, which is the latest version.", NEWEST_APK_VERSION];
        } else {
            return [NSString stringWithFormat:@"An upgrade to %s is available.", NEWEST_APK_VERSION];
        }
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    NSInteger sections = [super numberOfSectionsInTableView:tableView];
    if (!self.includeDebugPanel)
        sections--;
    return sections;
}

- (IBAction)disableDimmingChanged:(id)sender {
    UserPreferences.shared.shouldDisableDimming = self.disableDimmingSwitch.on;
}

- (IBAction)enableMulticoreChanged:(id)sender {
    UserPreferences.shared.shouldEnableMulticore = self.enableMulticoreSwitch.on;
}

- (IBAction)enableExtraLockingChanged:(id)sender {
    UserPreferences.shared.shouldEnableExtraLocking = self.enableExtraLockingSwitch.on;
}

//- (IBAction)shouldLockSleepNanoseconds:(id)sender {
//    UserPreferences.shared.shouldLockSleepNanoseconds = self.shouldLockSleepNanosecondsSwitch.on;
//}

- (IBAction)textBoxSubmit:(id)sender {
    [sender resignFirstResponder];
}

- (IBAction)launchCommandChanged:(id)sender {
    UserPreferences.shared.launchCommand = [self.launchCommandField.text componentsSeparatedByString:@" "];
}

- (IBAction)bootCommandChanged:(id)sender {
    UserPreferences.shared.bootCommand = [self.bootCommandField.text componentsSeparatedByString:@" "];
}

@end
