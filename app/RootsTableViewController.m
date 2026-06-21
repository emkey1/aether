//
//  RootsTableViewController.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import "AppDelegate.h"
#import "Roots.h"
#import "RootsTableViewController.h"
#import "ProgressReportViewController.h"
#import "UIApplication+OpenURL.h"
#import "UIViewController+Extras.h"
#import "NSObject+SaneKVO.h"
#import "UserPreferences.h"
#import "WorkspaceViewController.h"

@interface RootsTableViewController ()
// Archives found in the shared /AOK/persist/roots directory, shown as the
// "Root Cached Filesystems" section. Cached so the table data source is stable
// within a reload; refreshed on appear and when roots change.
@property (nonatomic, copy) NSArray<NSURL *> *cachedRootArchives;
@end

@interface RootDetailViewController : UITableViewController <UIDocumentPickerDelegate, UITextFieldDelegate>

@property (nonatomic) NSString *rootName;
@property (nonatomic) NSURL *exportURL;

@property (weak, nonatomic) IBOutlet UITextField *nameField;
@property (weak, nonatomic) IBOutlet UILabel *deleteLabel;
@property (weak, nonatomic) IBOutlet UITableViewCell *deleteCell;

@end

@implementation RootsTableViewController

- (BOOL)_bundledChoiceRequiresAMD64Bringup:(NSDictionary<NSString *, NSString *> *)choice {
    return [choice[@"guestABI"] isEqualToString:@"amd64"];
}

- (NSString *)_bundledChoiceSubtitle:(NSDictionary<NSString *, NSString *> *)choice {
    if ([self _bundledChoiceRequiresAMD64Bringup:choice]) {
        return @"x86_64 (amd64) guest rootfs.";
    }
    return @"i386 guest rootfs.";
}

- (void)_beginBundledImportChoice:(NSDictionary<NSString *, NSString *> *)choice {
    [self startBundledImportChoice:choice];
}

- (void)_confirmBundledImportChoiceIfNeeded:(NSDictionary<NSString *, NSString *> *)choice {
    // x86_64/amd64 roots import directly now, like i386 — no experimental confirmation prompt.
    [self _beginBundledImportChoice:choice];
}

- (void)_completeRootSelectionWithName:(NSString *)rootName {
    if (rootName.length == 0)
        return;
    Roots.instance.defaultRoot = rootName;
    if (self.rootSelectionHandler != nil)
        self.rootSelectionHandler(rootName);
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)bundledChoices {
    return Roots.instance.bundledRootChoices;
}

- (BOOL)showsBundledChoicesSection {
    return self.bundledChoices.count != 0;
}

- (BOOL)showsInstalledRootsSection {
    return Roots.instance.roots.count != 0;
}

- (BOOL)showsCachedRootsSection {
    return self.cachedRootArchives.count != 0;
}

- (void)reloadCachedRootArchives {
    self.cachedRootArchives = Roots.instance.cachedRootArchiveURLs;
}

// Sections appear in this fixed order, each shown only when non-empty:
// Installed Filesystems, Root Cached Filesystems, Bundled Filesystems.
- (NSInteger)installedRootsSectionIndex {
    return self.showsInstalledRootsSection ? 0 : NSNotFound;
}
- (NSInteger)cachedRootsSectionIndex {
    if (!self.showsCachedRootsSection)
        return NSNotFound;
    return self.showsInstalledRootsSection ? 1 : 0;
}
- (NSInteger)bundledChoicesSectionIndex {
    if (!self.showsBundledChoicesSection)
        return NSNotFound;
    NSInteger index = 0;
    if (self.showsInstalledRootsSection)
        index++;
    if (self.showsCachedRootsSection)
        index++;
    return index;
}

- (BOOL)sectionShowsInstalledRoots:(NSInteger)section {
    return self.showsInstalledRootsSection && section == self.installedRootsSectionIndex;
}

- (BOOL)sectionShowsCachedRoots:(NSInteger)section {
    return self.showsCachedRootsSection && section == self.cachedRootsSectionIndex;
}

- (BOOL)sectionShowsBundledChoices:(NSInteger)section {
    return self.showsBundledChoicesSection && section == self.bundledChoicesSectionIndex;
}

- (NSIndexPath *)selectedIndexPathForSender:(id)sender {
    if ([sender isKindOfClass:UITableViewCell.class]) {
        return [self.tableView indexPathForCell:sender];
    }
    if ([sender isKindOfClass:UIGestureRecognizer.class]) {
        UIView *view = ((UIGestureRecognizer *) sender).view;
        if ([view isKindOfClass:UITableViewCell.class])
            return [self.tableView indexPathForCell:(UITableViewCell *) view];
    }
    return self.tableView.indexPathForSelectedRow;
}

- (void)finishInitialSelectionIfNeededFromEmptyState:(BOOL)wasInitialSelection {
    if (!wasInitialSelection || Roots.instance.needsInitialRootSelection)
        return;
    [NSNotificationCenter.defaultCenter postNotificationName:RootsDidFinishInitialSelectionNotification object:nil];
}

- (void)startBundledImportChoice:(NSDictionary<NSString *, NSString *> *)choice {
    NSString *identifier = choice[@"identifier"];
    NSString *displayName = choice[@"displayName"];
    BOOL wasInitialSelection = Roots.instance.needsInitialRootSelection;
    NSString *initialWindow = choice[@"initialWindow"];

    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Importing %@", displayName];
    [self presentViewController:progressVC animated:YES completion:nil];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        BOOL success = [Roots.instance importBundledRootChoice:identifier error:&error progressReporter:progressVC];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success) {
                    if (error != nil)
                        [self presentError:error title:@"Import failed"];
                    return;
                }
                NSString *currentInitialWindow =
                    [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
                if (!self.choosesRootOnSelection &&
                    wasInitialSelection &&
                    ![currentInitialWindow isEqualToString:ISHInitialWindowWorkspaceValue] &&
                    ([initialWindow isEqualToString:@"terminal"] ||
                     [initialWindow isEqualToString:@"session-shell"])) {
                    [NSUserDefaults.standardUserDefaults setObject:initialWindow
                                                            forKey:kPreferenceInitialWindowKey];
                }
                if (self.choosesRootOnSelection) {
                    [self _completeRootSelectionWithName:Roots.instance.roots.lastObject];
                    return;
                }
                [self finishInitialSelectionIfNeededFromEmptyState:wasInitialSelection];
            }];
        });
    });
}

- (void)presentImportOptionsFromSender:(id)sender {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Import Filesystem"
                                            message:@"Choose a bundled filesystem or import a root archive from Files."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSDictionary<NSString *, NSString *> *choice in self.bundledChoices) {
        NSString *displayName = choice[@"displayName"];
        [alert addAction:[UIAlertAction actionWithTitle:displayName
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self _confirmBundledImportChoiceIfNeeded:choice];
        }]];
    }

    [alert addAction:[UIAlertAction actionWithTitle:@"Browse Files…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                                  initWithDocumentTypes:@[@"public.tar-archive", @"org.gnu.gnu-zip-archive", @"public.bzip2-archive"]
                                                  inMode:UIDocumentPickerModeImport];
        [self presentViewController:picker animated:YES completion:nil];
        if (@available(iOS 13, *)) {
            picker.shouldShowFileExtensions = YES;
        }
        picker.delegate = self;
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil) {
        if ([sender isKindOfClass:UIBarButtonItem.class]) {
            popover.barButtonItem = sender;
        } else if ([sender isKindOfClass:UIView.class]) {
            popover.sourceView = sender;
            popover.sourceRect = ((UIView *) sender).bounds;
        } else {
            popover.sourceView = self.view;
            popover.sourceRect = CGRectMake(CGRectGetMidX(self.view.bounds), CGRectGetMidY(self.view.bounds), 1, 1);
        }
    }

    [self presentViewController:alert animated:YES completion:nil];
}

- (void)updateEmptyState {
    if (Roots.instance.roots.count != 0 || self.cachedRootArchives.count != 0 || self.bundledChoices.count != 0) {
        self.tableView.backgroundView = nil;
        self.tableView.scrollEnabled = YES;
        self.navigationItem.rightBarButtonItem.enabled = YES;
        return;
    }

    UILabel *label = [[UILabel alloc] init];
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    if (@available(iOS 13.0, *)) {
        label.textColor = UIColor.secondaryLabelColor;
    } else {
        label.textColor = UIColor.grayColor;
    }
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    if (Roots.instance.initialBundledRootImportInProgress) {
        label.text = @"Extracting the bundled filesystem.\nThis can take a moment on first launch.";
    } else if (Roots.instance.initialBundledRootImportError != nil) {
        label.text = [NSString stringWithFormat:@"%@\n\nTap Import to add a filesystem manually after freeing space.",
                      Roots.instance.initialBundledRootImportError.localizedDescription];
    } else {
        label.text = @"No filesystems are available.\nTap Import to add a root filesystem.";
    }
    label.translatesAutoresizingMaskIntoConstraints = NO;

    UIView *container = [[UIView alloc] initWithFrame:self.tableView.bounds];
    UIActivityIndicatorView *spinner = nil;
    if (Roots.instance.initialBundledRootImportInProgress) {
        if (@available(iOS 13, *)) {
            spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        } else {
            spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleGray];
        }
        spinner.translatesAutoresizingMaskIntoConstraints = NO;
        [spinner startAnimating];
        [container addSubview:spinner];
    }
    [container addSubview:label];

    NSMutableArray<NSLayoutConstraint *> *constraints = [NSMutableArray array];
    if (spinner != nil) {
        [constraints addObject:[spinner.centerXAnchor constraintEqualToAnchor:container.centerXAnchor]];
        [constraints addObject:[spinner.bottomAnchor constraintEqualToAnchor:label.topAnchor constant:-16]];
    }
    [constraints addObject:[label.centerXAnchor constraintEqualToAnchor:container.centerXAnchor]];
    [constraints addObject:[label.centerYAnchor constraintEqualToAnchor:container.centerYAnchor constant:spinner != nil ? 18 : 0]];
    [constraints addObject:[label.leadingAnchor constraintGreaterThanOrEqualToAnchor:container.leadingAnchor constant:24]];
    [constraints addObject:[label.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor constant:-24]];
    [NSLayoutConstraint activateConstraints:constraints];
    self.tableView.backgroundView = container;
    self.tableView.scrollEnabled = !Roots.instance.initialBundledRootImportInProgress;
    self.navigationItem.rightBarButtonItem.enabled = !Roots.instance.initialBundledRootImportInProgress;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self reloadCachedRootArchives];
    [Roots.instance observe:@[@"roots", @"defaultRoot", @"initialBundledRootImportInProgress", @"initialBundledRootImportError"]
                    options:0 owner:self usingBlock:^(typeof(self) self) {
        [self reloadCachedRootArchives];
        [self updateEmptyState];
        [self.tableView reloadData];
    }];
    [self updateEmptyState];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    // Re-scan /AOK/persist/roots: archives may have been dropped in from the guest
    // (wget/scp/Files) since this screen was last shown.
    [self reloadCachedRootArchives];
    [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    NSInteger sections = 0;
    if (self.showsInstalledRootsSection)
        sections++;
    if (self.showsCachedRootsSection)
        sections++;
    if (self.showsBundledChoicesSection)
        sections++;
    return MAX(sections, 1);
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if ([self sectionShowsInstalledRoots:section])
        return Roots.instance.roots.count;
    if ([self sectionShowsCachedRoots:section])
        return self.cachedRootArchives.count;
    if ([self sectionShowsBundledChoices:section])
        return self.bundledChoices.count;
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if ([self sectionShowsInstalledRoots:section]) {
        if (self.showsBundledChoicesSection || self.showsCachedRootsSection)
            return @"Installed Filesystems";
        return self.choosesRootOnSelection ? @"Choose a Filesystem" : nil;
    }
    if ([self sectionShowsCachedRoots:section]) {
        return @"Root Cached Filesystems (/AOK/persist/roots)";
    }
    if ([self sectionShowsBundledChoices:section]) {
        if (self.showsInstalledRootsSection || self.showsCachedRootsSection)
            return @"Bundled Filesystems";
        return @"Choose a Filesystem";
    }
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if ([self sectionShowsInstalledRoots:section] && self.choosesRootOnSelection) {
        return @"Tap a filesystem to make it active and continue booting.";
    }
    if ([self sectionShowsCachedRoots:section]) {
        return @"Archives in /AOK/persist/roots (shared across all filesystems). Tap one to install it as a new filesystem.";
    }
    if ([self sectionShowsBundledChoices:section]) {
        if (!self.showsInstalledRootsSection)
            return @"Choose one of the bundled filesystems below, or tap Import to browse for another archive.";
        return @"These bundled filesystems can be imported again at any time.";
    }
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if ([self sectionShowsBundledChoices:indexPath.section]) {
        UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"BundledRootChoice"];
        if (cell == nil)
            cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"BundledRootChoice"];
        NSDictionary<NSString *, NSString *> *choice = self.bundledChoices[indexPath.row];
        cell.textLabel.text = choice[@"displayName"];
        cell.detailTextLabel.text = [self _bundledChoiceSubtitle:choice];
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        return cell;
    }

    if ([self sectionShowsCachedRoots:indexPath.section]) {
        UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"CachedRootArchive"];
        if (cell == nil)
            cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"CachedRootArchive"];
        NSURL *archive = self.cachedRootArchives[indexPath.row];
        cell.textLabel.text = archive.lastPathComponent;
        NSNumber *size = nil;
        [archive getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        cell.detailTextLabel.text = size != nil
            ? [NSByteCountFormatter stringFromByteCount:size.longLongValue countStyle:NSByteCountFormatterCountStyleFile]
            : nil;
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        return cell;
    }

    NSString *ident = @"Root";
    BOOL isDefaultRoot = [Roots.instance.roots[indexPath.row] isEqual:Roots.instance.defaultRoot];
    if (isDefaultRoot)
        ident = @"Default Root";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:ident forIndexPath:indexPath];
    cell.textLabel.text = Roots.instance.roots[indexPath.row];
    if (isDefaultRoot) {
        cell.accessibilityTraits |= UIAccessibilityTraitSelected;
    } else {
        cell.accessibilityTraits &= ~UIAccessibilityTraitSelected;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if ([self sectionShowsBundledChoices:indexPath.section]) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        [self _confirmBundledImportChoiceIfNeeded:self.bundledChoices[indexPath.row]];
        return;
    }
    if ([self sectionShowsCachedRoots:indexPath.section]) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        if (indexPath.row < (NSInteger) self.cachedRootArchives.count)
            [self importArchiveAtURL:self.cachedRootArchives[indexPath.row] securityScoped:NO];
        return;
    }
    if (self.choosesRootOnSelection && [self sectionShowsInstalledRoots:indexPath.section]) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        [self _completeRootSelectionWithName:Roots.instance.roots[indexPath.row]];
        return;
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    NSIndexPath *indexPath = [self selectedIndexPathForSender:sender];
    if (indexPath == nil || ![self sectionShowsInstalledRoots:indexPath.section])
        return;
    RootDetailViewController *vc = segue.destinationViewController;
    vc.rootName = Roots.instance.roots[indexPath.row];
}

- (BOOL)shouldPerformSegueWithIdentifier:(NSString *)identifier sender:(id)sender {
    NSIndexPath *indexPath = [self selectedIndexPathForSender:sender];
    if (indexPath != nil && [self sectionShowsBundledChoices:indexPath.section])
        return NO;
    if (indexPath != nil && [self sectionShowsCachedRoots:indexPath.section])
        return NO;
    if (self.choosesRootOnSelection && indexPath != nil && [self sectionShowsInstalledRoots:indexPath.section])
        return NO;
    return [super shouldPerformSegueWithIdentifier:identifier sender:sender];
}

- (IBAction)importFilesystem:(id)sender {
    [self presentImportOptionsFromSender:self.navigationItem.rightBarButtonItem ?: sender];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSAssert(urls.count == 1, @"somehow picked multiple documents");
    // Document-picker URLs are security-scoped; cached /root/roots archives live
    // in our own container and aren't.
    [self importArchiveAtURL:urls.firstObject securityScoped:YES];
}

- (void)importArchiveAtURL:(NSURL *)url securityScoped:(BOOL)securityScoped {
    NSString *fileName = url.lastPathComponent.stringByDeletingPathExtension;
    if ([fileName hasSuffix:@".tar"])
        fileName = fileName.stringByDeletingPathExtension;
    unsigned i = 2;
    NSString *name = fileName;
    while ([Roots.instance.roots containsObject:name]) {
        name = [NSString stringWithFormat:@"%@ %u", fileName, i++];
    }

    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Importing %@", name];
    [self presentViewController:progressVC animated:YES completion:nil];
    BOOL wasInitialSelection = Roots.instance.needsInitialRootSelection;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error;
        if (securityScoped)
            [url startAccessingSecurityScopedResource];
        BOOL success = [Roots.instance importRootFromArchive:url name:name error:&error progressReporter:progressVC];
        if (securityScoped)
            [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success) {
                    if (error != nil)
                        [self presentError:error title:@"Import failed"];
                    return;
                }
                if (self.choosesRootOnSelection) {
                    [self _completeRootSelectionWithName:name];
                    return;
                }
                [self finishInitialSelectionIfNeededFromEmptyState:wasInitialSelection];
            }];
        });
    });
}

@end

@implementation RootDetailViewController

- (void)viewWillAppear:(BOOL)animated {
    self.nameField.text = self.rootName;
    [self update];
}

- (void)update {
    self.navigationItem.title = self.rootName;
    self.nameField.enabled = !self.isDefaultRoot;
    self.nameField.clearButtonMode = self.isDefaultRoot ? UITextFieldViewModeNever : UITextFieldViewModeAlways;
    self.nameField.accessibilityLabel = @"Filesystem Name";
    self.deleteLabel.enabled = !self.isDefaultRoot;
    self.deleteCell.selectionStyle = !self.isDefaultRoot ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    [self.tableView reloadData];
}

- (IBAction)nameChanged:(id)sender {
    NSString *newName = self.nameField.text;
    NSError *err;
    if (![Roots.instance renameRoot:self.rootName toName:newName error:&err]) {
        self.nameField.text = self.rootName;
        [self presentError:err title:@"Rename failed"];
        return;
    }
    self.rootName = newName;
    [self update];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    [textField resignFirstResponder];
    return NO;
}

- (BOOL)isDefaultRoot {
    return [self.rootName isEqualToString:Roots.instance.defaultRoot];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 2) { // delete
        if (self.isDefaultRoot)
            return @"This filesystem can't be deleted because it's currently mounted as the root.";
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == 0 && indexPath.row == 1)
        [self browseFiles];
    if (indexPath.section == 0 && indexPath.row == 2)
        [self exportFilesystem];
    if (indexPath.section == 1 && indexPath.row == 0)
        [self bootThis];
    if (indexPath.section == 2 && indexPath.row == 0)
        [self deleteFilesystem];
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)browseFiles {
    // The root now lives one level down, inside the single "iSH-AOK" domain.
    NSURL *url = [[NSFileProviderManager.defaultManager.documentStorageURL
                   URLByAppendingPathComponent:@"iSH-AOK"]
                  URLByAppendingPathComponent:self.rootName];
    NSURLComponents *components = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    components.scheme = @"shareddocuments";
    [UIApplication openURL:components.string];
}

- (void)exportFilesystem {
    self.exportURL = [[NSFileManager.defaultManager.temporaryDirectory
                       URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]]
                      URLByAppendingPathComponent:[NSString stringWithFormat:@"%@.tar.gz", self.rootName]];
    [NSFileManager.defaultManager createDirectoryAtURL:self.exportURL.URLByDeletingLastPathComponent
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Exporting %@", self.rootName];
    [self presentViewController:progressVC animated:YES completion:nil];

    // witness the callback hell
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *err;
        BOOL success = [Roots.instance exportRootNamed:self.rootName toArchive:self.exportURL error:&err progressReporter:progressVC];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success) {
                    if (err != nil)
                        [self presentError:err title:@"Export failed"];
                    return;
                }

                UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                                          initWithURL:self.exportURL
                                                          inMode:UIDocumentPickerModeExportToService];
                picker.delegate = self;
                if (@available(iOS 13, *)) {
                    picker.shouldShowFileExtensions = YES;
                }
                [self presentViewController:picker animated:YES completion:nil];
            }];
        });
    });
}

- (void)setExportURL:(NSURL *)exportURL {
    [NSFileManager.defaultManager removeItemAtURL:_exportURL.URLByDeletingLastPathComponent error:nil];
    _exportURL = exportURL;
}

- (void)bootThis {
    Roots.instance.defaultRoot = self.rootName;
    AppDelegate *appDelegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
    if ([appDelegate isKindOfClass:AppDelegate.class]) {
        [appDelegate exitApp];
    } else {
        exit(0);
    }
}

- (void)deleteFilesystem {
    if (self.isDefaultRoot)
        return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Really delete?"
                                                                   message:@"I can't be bothered to implement any undo or regret UI so this is irreversable."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        NSError *error;
        if (![Roots.instance destroyRootNamed:self.rootName error:&error]) {
            [self presentError:error title:@"Delete failed"];
        } else {
            [self.navigationController popViewControllerAnimated:YES];
        }
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)dealloc {
    self.exportURL = nil; // get it deleted
}

@end
