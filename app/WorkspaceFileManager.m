//
//  WorkspaceFileManager.m
//  iSH-AOK
//

#import "WorkspaceFileManager.h"
#import "GuestFileBridge.h"

static const CGFloat kFileManagerSidebarWidth = 180.0;
static const CGFloat kFileManagerSidebarCollapseThreshold = 520.0;
static const CGFloat kFileManagerToolbarHeight = 44.0;
static const CGFloat kFileManagerStatusBarHeight = 28.0;
static const CGFloat kFileManagerDividerThickness = 1.0 / 3.0;  // hairline; UIView doesn't need device-scale awareness here

static NSString *const kFileManagerSortModeDefaultsKey = @"WorkspaceFileManagerSortMode";
static NSString *const kFileManagerShowHiddenDefaultsKey = @"WorkspaceFileManagerShowHidden";

static NSString *const kFileManagerCellReuseID = @"filemanager.row";
static NSString *const kFileManagerSidebarCellReuseID = @"filemanager.sidebar";

typedef NS_ENUM(NSInteger, WorkspaceFileManagerSortMode) {
    WorkspaceFileManagerSortName = 0,
    WorkspaceFileManagerSortSize = 1,
    WorkspaceFileManagerSortDate = 2,
    WorkspaceFileManagerSortKind = 3,
};

// Fixed, non-editable sidebar rows for v1 -- the plan's "user-editable Favorites"
// is a later-phase nicety; this covers the seed locations that matter today.
static NSArray<NSDictionary *> *ISHFileManagerSidebarRows(void) {
    return @[
        @{@"title": @"Home", @"path": @"/root", @"symbol": @"house"},
        @{@"title": @"Persist", @"path": @"/AOK/persist", @"symbol": @"externaldrive"},
        @{@"title": @"Temporary", @"path": @"/tmp", @"symbol": @"clock"},
        @{@"title": @"Root", @"path": @"/", @"symbol": @"internaldrive"},
    ];
}

@interface WorkspaceFileManagerToolViewController () <UITableViewDataSource, UITableViewDelegate>
@end

@implementation WorkspaceFileManagerToolViewController {
    UIView *_sidebarContainerView;
    UITableView *_sidebarTableView;
    NSLayoutConstraint *_sidebarWidthConstraint;
    BOOL _sidebarHidden;

    UIView *_dividerView;

    UIView *_toolbarView;
    UIButton *_backButton;
    UIButton *_forwardButton;
    UIButton *_upButton;
    UILabel *_pathLabel;
    UIButton *_moreButton;

    UITableView *_tableView;
    UIView *_statusBarView;
    UILabel *_statusLabel;

    NSString *_currentPath;
    NSMutableArray<NSString *> *_backHistory;
    NSMutableArray<NSString *> *_forwardHistory;

    NSArray<ISHGuestFileItem *> *_allItems;   // last listing, unfiltered/unsorted
    NSArray<ISHGuestFileItem *> *_items;      // filtered + sorted, what the table shows
    NSError *_loadError;
    BOOL _loading;
    NSInteger _loadGeneration;                // discards stale async listings

    WorkspaceFileManagerSortMode _sortMode;
    BOOL _showHidden;
}

#pragma mark Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"File Manager";

    _backHistory = [NSMutableArray array];
    _forwardHistory = [NSMutableArray array];
    _items = @[];
    _allItems = @[];

    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    _sortMode = (WorkspaceFileManagerSortMode)[defaults integerForKey:kFileManagerSortModeDefaultsKey];
    _showHidden = [defaults boolForKey:kFileManagerShowHiddenDefaultsKey];

    [self buildSidebar];
    [self buildDivider];
    [self buildToolbar];
    [self buildTableView];
    [self buildStatusBar];
    [self activateRegionConstraints];

    [self refreshMoreMenu];
    _currentPath = @"/";
    [self updateToolbarState];
    [self reload];

    // Start in /AOK/persist when it exists (matches MotePad's default-directory
    // convention) without blocking the first paint on the check.
    __weak typeof(self) weakSelf = self;
    [[ISHGuestFileBridge sharedBridge] statAtGuestPath:@"/AOK/persist" completion:^(ISHGuestFileItem *item, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        BOOL userAlreadyNavigated = strongSelf->_backHistory.count > 0 || ![strongSelf->_currentPath isEqualToString:@"/"];
        if (userAlreadyNavigated) return;
        if (item != nil && item.kind == ISHGuestFileKindDirectory)
            [strongSelf navigateToPath:@"/AOK/persist" pushHistory:NO];
    }];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    BOOL hideSidebar = self.view.bounds.size.width < kFileManagerSidebarCollapseThreshold;
    if (hideSidebar == _sidebarHidden)
        return;
    _sidebarHidden = hideSidebar;
    _sidebarContainerView.hidden = hideSidebar;
    _sidebarWidthConstraint.constant = hideSidebar ? 0 : kFileManagerSidebarWidth;
}

#pragma mark View construction

- (void)buildSidebar {
    _sidebarContainerView = [UIView new];
    _sidebarContainerView.translatesAutoresizingMaskIntoConstraints = NO;
    _sidebarContainerView.clipsToBounds = YES;
    [self.toolContentView addSubview:_sidebarContainerView];

    _sidebarTableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _sidebarTableView.translatesAutoresizingMaskIntoConstraints = NO;
    _sidebarTableView.dataSource = self;
    _sidebarTableView.delegate = self;
    _sidebarTableView.rowHeight = 36.0;
    [_sidebarContainerView addSubview:_sidebarTableView];

    [NSLayoutConstraint activateConstraints:@[
        [_sidebarTableView.topAnchor constraintEqualToAnchor:_sidebarContainerView.topAnchor],
        [_sidebarTableView.leadingAnchor constraintEqualToAnchor:_sidebarContainerView.leadingAnchor],
        [_sidebarTableView.trailingAnchor constraintEqualToAnchor:_sidebarContainerView.trailingAnchor],
        [_sidebarTableView.bottomAnchor constraintEqualToAnchor:_sidebarContainerView.bottomAnchor],
    ]];
}

- (void)buildDivider {
    _dividerView = [UIView new];
    _dividerView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_dividerView];
}

- (void)buildToolbar {
    _toolbarView = [UIView new];
    _toolbarView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_toolbarView];

    _backButton = [self toolbarIconButtonNamed:@"chevron.left" action:@selector(navigateBack)];
    _forwardButton = [self toolbarIconButtonNamed:@"chevron.right" action:@selector(navigateForward)];
    _upButton = [self toolbarIconButtonNamed:@"arrow.up" action:@selector(navigateUp)];
    _moreButton = [self toolbarIconButtonNamed:@"ellipsis.circle" action:nil];
    _moreButton.showsMenuAsPrimaryAction = YES;

    _pathLabel = [UILabel new];
    _pathLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _pathLabel.font = [UIFont systemFontOfSize:13.0 weight:UIFontWeightMedium];
    _pathLabel.lineBreakMode = NSLineBreakByTruncatingHead;
    _pathLabel.textAlignment = NSTextAlignmentCenter;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_backButton, _forwardButton, _upButton, _pathLabel, _moreButton]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisHorizontal;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 10.0;
    [_toolbarView addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:_toolbarView.leadingAnchor constant:10],
        [stack.trailingAnchor constraintEqualToAnchor:_toolbarView.trailingAnchor constant:-10],
        [stack.centerYAnchor constraintEqualToAnchor:_toolbarView.centerYAnchor],
    ]];
}

- (UIButton *)toolbarIconButtonNamed:(NSString *)symbolName action:(nullable SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setImage:[UIImage systemImageNamed:symbolName] forState:UIControlStateNormal];
    if (action != NULL)
        [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [button.widthAnchor constraintGreaterThanOrEqualToConstant:28].active = YES;
    return button;
}

- (void)buildTableView {
    _tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _tableView.translatesAutoresizingMaskIntoConstraints = NO;
    _tableView.dataSource = self;
    _tableView.delegate = self;
    _tableView.backgroundColor = UIColor.clearColor;
    [self.toolContentView addSubview:_tableView];
}

- (void)buildStatusBar {
    _statusBarView = [UIView new];
    _statusBarView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_statusBarView];

    _statusLabel = [UILabel new];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.font = [UIFont systemFontOfSize:11.0];
    [_statusBarView addSubview:_statusLabel];

    [NSLayoutConstraint activateConstraints:@[
        [_statusLabel.leadingAnchor constraintEqualToAnchor:_statusBarView.leadingAnchor constant:10],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_statusBarView.centerYAnchor],
    ]];
}

// Explicit region anchors rather than a fill-distribution UIStackView: UITableView
// has no reliable intrinsic content size, and a stack view's "give the ambiguous
// child the remaining space" behavior is exactly the kind of thing that's easy to
// get subtly wrong without being able to see it render. Every region here is
// pinned by hand instead, matching how every other applet in this file pins its
// outer scroll/content views.
- (void)activateRegionConstraints {
    UIView *content = self.toolContentView;
    _sidebarWidthConstraint = [_sidebarContainerView.widthAnchor constraintEqualToConstant:kFileManagerSidebarWidth];
    [NSLayoutConstraint activateConstraints:@[
        [_sidebarContainerView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_sidebarContainerView.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_sidebarContainerView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        _sidebarWidthConstraint,

        [_dividerView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_dividerView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [_dividerView.leadingAnchor constraintEqualToAnchor:_sidebarContainerView.trailingAnchor],
        [_dividerView.widthAnchor constraintEqualToConstant:kFileManagerDividerThickness],

        [_toolbarView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_toolbarView.leadingAnchor constraintEqualToAnchor:_dividerView.trailingAnchor],
        [_toolbarView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_toolbarView.heightAnchor constraintEqualToConstant:kFileManagerToolbarHeight],

        [_tableView.topAnchor constraintEqualToAnchor:_toolbarView.bottomAnchor],
        [_tableView.leadingAnchor constraintEqualToAnchor:_dividerView.trailingAnchor],
        [_tableView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_tableView.bottomAnchor constraintEqualToAnchor:_statusBarView.topAnchor],

        [_statusBarView.leadingAnchor constraintEqualToAnchor:_dividerView.trailingAnchor],
        [_statusBarView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_statusBarView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [_statusBarView.heightAnchor constraintEqualToConstant:kFileManagerStatusBarHeight],
    ]];
}

#pragma mark Theme

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _pathLabel.textColor = theme[@"primary"];
    _statusLabel.textColor = theme[@"secondary"];
    _dividerView.backgroundColor = theme[@"stroke"];
    _tableView.separatorColor = theme[@"stroke"];
    _sidebarTableView.separatorColor = theme[@"stroke"];
    _sidebarTableView.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.4];
    for (UIButton *button in @[_backButton, _forwardButton, _upButton, _moreButton])
        button.tintColor = theme[@"accent"];
    [_tableView reloadData];
    [_sidebarTableView reloadData];
}

#pragma mark Navigation

- (void)navigateToPath:(NSString *)path pushHistory:(BOOL)pushHistory {
    if (pushHistory && _currentPath != nil && ![_currentPath isEqualToString:path]) {
        [_backHistory addObject:_currentPath];
        [_forwardHistory removeAllObjects];
    }
    _currentPath = path;
    [self updateToolbarState];
    [self reload];
}

- (void)navigateBack {
    if (_backHistory.count == 0) return;
    NSString *target = _backHistory.lastObject;
    [_backHistory removeLastObject];
    [_forwardHistory addObject:_currentPath];
    _currentPath = target;
    [self updateToolbarState];
    [self reload];
}

- (void)navigateForward {
    if (_forwardHistory.count == 0) return;
    NSString *target = _forwardHistory.lastObject;
    [_forwardHistory removeLastObject];
    [_backHistory addObject:_currentPath];
    _currentPath = target;
    [self updateToolbarState];
    [self reload];
}

- (void)navigateUp {
    if ([_currentPath isEqualToString:@"/"]) return;
    NSString *parent = _currentPath.stringByDeletingLastPathComponent;
    if (parent.length == 0) parent = @"/";
    [self navigateToPath:parent pushHistory:YES];
}

- (void)updateToolbarState {
    _backButton.enabled = _backHistory.count > 0;
    _forwardButton.enabled = _forwardHistory.count > 0;
    _upButton.enabled = ![_currentPath isEqualToString:@"/"];
    _pathLabel.text = _currentPath;
}

#pragma mark WorkspaceFileOpenable

// Opening a path in the File Manager means "reveal its containing folder" --
// the same meaning "open" has for any directory-aware applet. No adopters call
// this yet; it exists so a future "reveal in Finder" action has somewhere to go.
- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    NSString *directory = guestPath.stringByDeletingLastPathComponent;
    if (directory.length == 0) directory = @"/";
    [self navigateToPath:directory pushHistory:YES];
}

#pragma mark Loading

- (void)reload {
    NSInteger generation = ++_loadGeneration;
    [self setLoading:YES];
    __weak typeof(self) weakSelf = self;
    [[ISHGuestFileBridge sharedBridge] listDirectoryAtGuestPath:_currentPath
                                                      completion:^(NSArray<ISHGuestFileItem *> *items, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;  // a newer navigation superseded this listing
        [strongSelf setLoading:NO];
        strongSelf->_loadError = error;
        strongSelf->_allItems = items ?: @[];
        [strongSelf applyFilterAndSort];
    }];
}

- (void)setLoading:(BOOL)loading {
    _loading = loading;
    _tableView.userInteractionEnabled = !loading;
    if (loading) {
        UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        [spinner startAnimating];
        _tableView.backgroundView = spinner;
    } else {
        [self updateEmptyState];
    }
}

- (void)applyFilterAndSort {
    NSArray<ISHGuestFileItem *> *filtered = _allItems;
    if (!_showHidden) {
        filtered = [filtered filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:
            ^BOOL(ISHGuestFileItem *item, NSDictionary *bindings) {
                return ![item.name hasPrefix:@"."];
            }]];
    }
    _items = [self sortedItems:filtered];
    [_tableView reloadData];
    [self updateEmptyState];
    [self updateStatusLabel];
}

- (NSArray<ISHGuestFileItem *> *)sortedItems:(NSArray<ISHGuestFileItem *> *)items {
    NSComparator comparator;
    switch (_sortMode) {
        case WorkspaceFileManagerSortSize:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                if (a.size != b.size) return a.size < b.size ? NSOrderedDescending : NSOrderedAscending;
                return [a.name caseInsensitiveCompare:b.name];
            };
            break;
        case WorkspaceFileManagerSortDate:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                NSDate *ad = a.modificationDate ?: NSDate.distantPast;
                NSDate *bd = b.modificationDate ?: NSDate.distantPast;
                NSComparisonResult result = [bd compare:ad];
                return result != NSOrderedSame ? result : [a.name caseInsensitiveCompare:b.name];
            };
            break;
        case WorkspaceFileManagerSortKind:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                if (a.kind != b.kind) return a.kind < b.kind ? NSOrderedAscending : NSOrderedDescending;
                return [a.name caseInsensitiveCompare:b.name];
            };
            break;
        case WorkspaceFileManagerSortName:
        default:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                return [a.name caseInsensitiveCompare:b.name];
            };
            break;
    }
    return [items sortedArrayUsingComparator:comparator];
}

- (void)updateEmptyState {
    if (_loading) return;
    if (_items.count > 0) {
        _tableView.backgroundView = nil;
        return;
    }
    UILabel *label = [UILabel new];
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.textColor = self.workspaceTheme[@"secondary"] ?: UIColor.secondaryLabelColor;
    label.font = [UIFont systemFontOfSize:15.0];
    if (_loadError != nil)
        label.text = _loadError.localizedDescription.length ? _loadError.localizedDescription : @"Couldn’t load this folder.";
    else if (![ISHGuestFileBridge.sharedBridge isGuestAvailable])
        label.text = @"The guest filesystem isn’t ready yet.";
    else
        label.text = @"This folder is empty.";
    _tableView.backgroundView = label;
}

- (void)updateStatusLabel {
    _statusLabel.text = _items.count == 1 ? @"1 item" : [NSString stringWithFormat:@"%lu items", (unsigned long)_items.count];
}

#pragma mark UITableViewDataSource / UITableViewDelegate

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (tableView == _sidebarTableView) ? (NSInteger)ISHFileManagerSidebarRows().count : (NSInteger)_items.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (tableView == _sidebarTableView)
        return [self sidebarCellForRowAtIndexPath:indexPath inTableView:tableView];
    return [self fileCellForRowAtIndexPath:indexPath inTableView:tableView];
}

- (UITableViewCell *)sidebarCellForRowAtIndexPath:(NSIndexPath *)indexPath inTableView:(UITableView *)tableView {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kFileManagerSidebarCellReuseID];
    if (cell == nil)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:kFileManagerSidebarCellReuseID];
    NSDictionary *row = ISHFileManagerSidebarRows()[(NSUInteger)indexPath.row];
    cell.textLabel.text = row[@"title"];
    cell.textLabel.font = [UIFont systemFontOfSize:13.0];
    cell.textLabel.textColor = self.workspaceTheme[@"primary"];
    cell.imageView.image = [UIImage systemImageNamed:row[@"symbol"]];
    cell.imageView.tintColor = self.workspaceTheme[@"accent"];
    cell.backgroundColor = UIColor.clearColor;
    cell.accessoryType = UITableViewCellAccessoryNone;
    return cell;
}

- (UITableViewCell *)fileCellForRowAtIndexPath:(NSIndexPath *)indexPath inTableView:(UITableView *)tableView {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kFileManagerCellReuseID];
    if (cell == nil)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:kFileManagerCellReuseID];
    if ((NSUInteger)indexPath.row >= _items.count)
        return cell;

    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    BOOL openable = [self itemIsOpenable:item];

    cell.textLabel.text = item.name;
    cell.textLabel.textColor = openable ? self.workspaceTheme[@"primary"] : self.workspaceTheme[@"secondary"];
    cell.detailTextLabel.text = [self subtitleForItem:item];
    cell.detailTextLabel.textColor = self.workspaceTheme[@"secondary"];
    cell.imageView.image = [self iconForItem:item];
    cell.imageView.tintColor = self.workspaceTheme[@"accent"];
    cell.accessoryType = (item.kind == ISHGuestFileKindDirectory) ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
    cell.selectionStyle = openable ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (tableView == _sidebarTableView) {
        NSDictionary *row = ISHFileManagerSidebarRows()[(NSUInteger)indexPath.row];
        [self navigateToPath:row[@"path"] pushHistory:YES];
        return;
    }
    if ((NSUInteger)indexPath.row >= _items.count) return;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    if (![self itemIsOpenable:item]) return;
    if (item.kind == ISHGuestFileKindDirectory) {
        [self navigateToPath:item.guestPath pushHistory:YES];
        return;
    }
    [self openFileItem:item];
}

- (nullable UIContextMenuConfiguration *)tableView:(UITableView *)tableView
             contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath
                                                  point:(CGPoint)point {
    if (tableView == _sidebarTableView) return nil;
    if ((NSUInteger)indexPath.row >= _items.count) return nil;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil
        actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggestedActions) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return nil;
        return [strongSelf contextMenuForItem:item];
    }];
}

- (UIMenu *)contextMenuForItem:(ISHGuestFileItem *)item {
    NSMutableArray<UIMenuElement *> *actions = [NSMutableArray array];
    __weak typeof(self) weakSelf = self;

    if ([self itemIsOpenable:item]) {
        [actions addObject:[UIAction actionWithTitle:@"Open" image:[UIImage systemImageNamed:@"arrow.up.forward.square"]
                                           identifier:nil handler:^(UIAction *action) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil) return;
            if (item.kind == ISHGuestFileKindDirectory) [strongSelf navigateToPath:item.guestPath pushHistory:YES];
            else [strongSelf openFileItem:item];
        }]];
    }
    if (item.kind == ISHGuestFileKindRegular) {
        [actions addObject:[UIAction actionWithTitle:@"Duplicate" image:[UIImage systemImageNamed:@"plus.square.on.square"]
                                           identifier:nil handler:^(UIAction *action) {
            [weakSelf duplicateItem:item];
        }]];
    }
    [actions addObject:[UIAction actionWithTitle:@"Rename…" image:[UIImage systemImageNamed:@"pencil"]
                                       identifier:nil handler:^(UIAction *action) {
        [weakSelf promptRenameItem:item];
    }]];
    [actions addObject:[UIAction actionWithTitle:@"Get Info" image:[UIImage systemImageNamed:@"info.circle"]
                                       identifier:nil handler:^(UIAction *action) {
        [weakSelf presentInfoForItem:item];
    }]];
    UIAction *deleteAction = [UIAction actionWithTitle:@"Delete" image:[UIImage systemImageNamed:@"trash"]
                                             identifier:nil handler:^(UIAction *action) {
        [weakSelf confirmDeleteItem:item];
    }];
    deleteAction.attributes = UIMenuElementAttributesDestructive;
    [actions addObject:deleteAction];
    return [UIMenu menuWithTitle:@"" children:actions];
}

- (nullable UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
     trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (tableView == _sidebarTableView) return nil;
    if ((NSUInteger)indexPath.row >= _items.count) return nil;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    __weak typeof(self) weakSelf = self;
    UIContextualAction *delete = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
                                                                          title:@"Delete"
                                                                        handler:^(UIContextualAction *action, UIView *sourceView, void (^completionHandler)(BOOL)) {
        [weakSelf confirmDeleteItem:item];
        completionHandler(YES);  // dismiss the swipe row now; the confirm alert handles the actual delete
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[delete]];
}

#pragma mark Row presentation helpers

- (BOOL)itemIsOpenable:(ISHGuestFileItem *)item {
    return item.kind == ISHGuestFileKindDirectory || item.kind == ISHGuestFileKindRegular;
}

- (nullable UIImage *)iconForItem:(ISHGuestFileItem *)item {
    NSString *symbolName = (item.kind == ISHGuestFileKindDirectory) ? @"folder"
                          : (item.kind == ISHGuestFileKindRegular) ? @"doc.text"
                          : @"questionmark.square";
    return [UIImage systemImageNamed:symbolName];
}

- (nullable NSString *)subtitleForItem:(ISHGuestFileItem *)item {
    if (item.isBrokenSymlink)
        return [NSString stringWithFormat:@"Broken alias → %@", item.symlinkTarget];
    if (item.kind == ISHGuestFileKindDirectory)
        return item.isSymlink ? [NSString stringWithFormat:@"Alias → %@", item.symlinkTarget] : nil;
    NSString *size = [self formattedSize:item.size];
    NSString *date = item.modificationDate ? [self formattedDate:item.modificationDate] : nil;
    return date ? [NSString stringWithFormat:@"%@ · %@", size, date] : size;
}

- (NSString *)formattedSize:(unsigned long long)size {
    static NSByteCountFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [NSByteCountFormatter new];
        formatter.countStyle = NSByteCountFormatterCountStyleFile;
    });
    return [formatter stringFromByteCount:(long long)size];
}

- (NSString *)formattedDate:(NSDate *)date {
    static NSDateFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [NSDateFormatter new];
        formatter.dateStyle = NSDateFormatterShortStyle;
        formatter.timeStyle = NSDateFormatterShortStyle;
    });
    return [formatter stringFromDate:date];
}

- (NSString *)kindDisplayNameForItem:(ISHGuestFileItem *)item {
    switch (item.kind) {
        case ISHGuestFileKindDirectory: return @"Folder";
        case ISHGuestFileKindRegular: return @"File";
        case ISHGuestFileKindSymlink: return @"Broken Alias";
        case ISHGuestFileKindFIFO: return @"Named Pipe";
        case ISHGuestFileKindSocket: return @"Socket";
        case ISHGuestFileKindCharDevice: return @"Character Device";
        case ISHGuestFileKindBlockDevice: return @"Block Device";
        case ISHGuestFileKindOther: default: return @"Item";
    }
}

- (NSString *)formattedPosixMode:(mode_t)mode {
    char permissions[10];
    static const char kPermissionChars[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        permissions[i] = (mode & (1 << (8 - i))) ? kPermissionChars[i] : '-';
    permissions[9] = '\0';
    return [NSString stringWithFormat:@"%s (%o)", permissions, mode & 07777];
}

#pragma mark Opening files

// Extensions known to be plain text: skip the content sniff below and route
// straight to MotePad. Extensions known to be binary media: no viewer exists
// yet (images/video/audio land in phases 2-3), so these always report no
// route rather than guessing from content.
static NSSet<NSString *> *ISHFileManagerKnownTextExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[
            @"txt", @"md", @"markdown", @"log", @"conf", @"cfg", @"config", @"json", @"yaml", @"yml",
            @"xml", @"csv", @"tsv", @"sh", @"bash", @"zsh", @"c", @"h", @"cpp", @"cc", @"hpp", @"m", @"mm",
            @"py", @"rb", @"js", @"ts", @"jsx", @"tsx", @"go", @"rs", @"java", @"swift", @"ini", @"toml",
            @"gitignore", @"env", @"plist", @"properties", @"gradle", @"make", @"mk", @"cmake", @"rst",
            @"tex", @"diff", @"patch",
        ]];
    });
    return set;
}

static NSSet<NSString *> *ISHFileManagerKnownBinaryExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[
            @"png", @"jpg", @"jpeg", @"gif", @"bmp", @"tiff", @"tif", @"heic", @"heif", @"webp", @"ico", @"svg",
            @"mp4", @"mov", @"m4v", @"avi", @"mkv", @"webm",
            @"mp3", @"m4a", @"aac", @"flac", @"ogg", @"opus", @"wav",
            @"zip", @"tar", @"gz", @"bz2", @"xz", @"7z", @"rar",
            @"pdf", @"doc", @"docx", @"xls", @"xlsx", @"ppt", @"pptx",
            @"so", @"dylib", @"dll", @"exe", @"o", @"a", @"bin",
            @"db", @"sqlite", @"sqlite3",
            @"ttf", @"otf", @"woff", @"woff2",
            @"html", @"htm",
        ]];
    });
    return set;
}

// The tool identifier is hardcoded rather than referencing
// ISHWorkspaceToolMotePadIdentifier (that constant has internal linkage in
// WorkspaceViewController.m) -- these identifier strings are already a durable
// contract (they round-trip through NSUserDefaults for saved window layouts),
// so a literal here carries no more fragility than that existing constraint.
- (void)determineOpenRouteForItem:(ISHGuestFileItem *)item completion:(void (^)(NSString * _Nullable toolIdentifier))completion {
    NSString *ext = item.name.pathExtension.lowercaseString;
    if (ext.length > 0 && [ISHFileManagerKnownTextExtensions() containsObject:ext]) {
        completion(@"motepad");
        return;
    }
    if (ext.length > 0 && [ISHFileManagerKnownBinaryExtensions() containsObject:ext]) {
        completion(nil);  // no viewer for this type yet
        return;
    }
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:item.guestPath maxBytes:8192
                                               completion:^(NSData *data, NSError *error) {
        if (data == nil) { completion(nil); return; }
        completion([self dataLooksTextual:data] ? @"motepad" : nil);
    }];
}

- (BOOL)dataLooksTextual:(NSData *)data {
    const uint8_t *bytes = data.bytes;
    for (NSUInteger i = 0; i < data.length; i++) {
        if (bytes[i] == 0) return NO;
    }
    return YES;
}

- (void)openFileItem:(ISHGuestFileItem *)item {
    __weak typeof(self) weakSelf = self;
    [self determineOpenRouteForItem:item completion:^(NSString *toolIdentifier) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        if (toolIdentifier == nil) {
            [strongSelf presentSimpleAlertWithTitle:@"Can’t Open"
                                             message:[NSString stringWithFormat:@"There’s no app to open “%@” yet.", item.name]];
            return;
        }
        [strongSelf.workspaceHostViewController openWorkspaceToolWithIdentifier:toolIdentifier fileGuestPath:item.guestPath];
    }];
}

#pragma mark File operations

- (BOOL)validateItemName:(NSString *)name {
    if (name.length == 0) return NO;
    if ([name containsString:@"/"]) return NO;
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) return NO;
    return YES;
}

- (void)promptNewFolder {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"New Folder" message:nil
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = @"untitled folder";
        textField.placeholder = @"Name";
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Create" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        NSString *name = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (![strongSelf validateItemName:name]) {
            [strongSelf presentSimpleAlertWithTitle:@"Invalid Name" message:@"That name isn’t valid."];
            return;
        }
        NSString *path = [strongSelf->_currentPath stringByAppendingPathComponent:name];
        [ISHGuestFileBridge.sharedBridge createDirectoryAtGuestPath:path completion:^(BOOL ok, NSError *error) {
            typeof(self) strongSelf2 = weakSelf;
            if (strongSelf2 == nil) return;
            if (!ok) {
                [strongSelf2 presentSimpleAlertWithTitle:@"Couldn’t Create Folder" message:error.localizedDescription];
                return;
            }
            [strongSelf2 reload];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptRenameItem:(ISHGuestFileItem *)item {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename" message:nil
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = item.name;
        textField.placeholder = @"Name";
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        NSString *newName = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (![strongSelf validateItemName:newName] || [newName isEqualToString:item.name])
            return;
        NSString *destinationPath = [item.guestPath.stringByDeletingLastPathComponent stringByAppendingPathComponent:newName];
        [ISHGuestFileBridge.sharedBridge moveItemAtGuestPath:item.guestPath toGuestPath:destinationPath
                                                    completion:^(BOOL ok, NSError *error) {
            typeof(self) strongSelf2 = weakSelf;
            if (strongSelf2 == nil) return;
            if (!ok) {
                [strongSelf2 presentSimpleAlertWithTitle:@"Couldn’t Rename" message:error.localizedDescription];
                return;
            }
            [strongSelf2 reload];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)uniqueDuplicateNameForItem:(ISHGuestFileItem *)item {
    NSString *base = item.name.stringByDeletingPathExtension;
    NSString *ext = item.name.pathExtension;
    NSSet<NSString *> *existingNames = [NSSet setWithArray:[_allItems valueForKey:@"name"]];
    NSString *candidate = ext.length ? [[base stringByAppendingString:@" copy"] stringByAppendingPathExtension:ext]
                                      : [base stringByAppendingString:@" copy"];
    NSUInteger suffix = 2;
    while ([existingNames containsObject:candidate]) {
        NSString *stem = [NSString stringWithFormat:@"%@ copy %lu", base, (unsigned long)suffix];
        candidate = ext.length ? [stem stringByAppendingPathExtension:ext] : stem;
        suffix++;
    }
    return candidate;
}

- (void)duplicateItem:(ISHGuestFileItem *)item {
    if (item.kind != ISHGuestFileKindRegular) {
        [self presentSimpleAlertWithTitle:@"Can’t Duplicate" message:@"Duplicating folders isn’t supported yet."];
        return;
    }
    NSString *destinationPath = [_currentPath stringByAppendingPathComponent:[self uniqueDuplicateNameForItem:item]];
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge copyItemAtGuestPath:item.guestPath toGuestPath:destinationPath
                                                completion:^(BOOL ok, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        if (!ok) {
            [strongSelf presentSimpleAlertWithTitle:@"Couldn’t Duplicate" message:error.localizedDescription];
            return;
        }
        [strongSelf reload];
    }];
}

- (void)confirmDeleteItem:(ISHGuestFileItem *)item {
    BOOL isDirectory = item.kind == ISHGuestFileKindDirectory;
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:[NSString stringWithFormat:@"Delete “%@”?", item.name]
                          message:(isDirectory ? @"This will permanently delete the folder and everything inside it."
                                                : @"This will permanently delete the file.")
                   preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        [ISHGuestFileBridge.sharedBridge removeItemAtGuestPath:item.guestPath recursive:YES completion:^(BOOL ok, NSError *error) {
            typeof(self) strongSelf2 = weakSelf;
            if (strongSelf2 == nil) return;
            if (!ok) {
                [strongSelf2 presentSimpleAlertWithTitle:@"Couldn’t Delete" message:error.localizedDescription];
                return;
            }
            [strongSelf2 reload];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)presentInfoForItem:(ISHGuestFileItem *)item {
    NSMutableString *message = [NSMutableString string];
    [message appendFormat:@"Kind: %@\n", [self kindDisplayNameForItem:item]];
    if (item.kind != ISHGuestFileKindDirectory)
        [message appendFormat:@"Size: %@\n", [self formattedSize:item.size]];
    if (item.modificationDate != nil)
        [message appendFormat:@"Modified: %@\n", [self formattedDate:item.modificationDate]];
    [message appendFormat:@"Permissions: %@\n", [self formattedPosixMode:item.posixMode]];
    [message appendFormat:@"Owner: %u:%u\n", item.uid, item.gid];
    if (item.isSymlink)
        [message appendFormat:@"%@ → %@\n", (item.isBrokenSymlink ? @"Broken link" : @"Alias"), item.symlinkTarget];
    [message appendFormat:@"Path: %@", item.guestPath];
    [self presentSimpleAlertWithTitle:item.name message:message];
}

- (void)presentSimpleAlertWithTitle:(NSString *)title message:(nullable NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                     message:message.length ? message : nil
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark More menu (New Folder / Sort / Hidden files / Refresh)

- (void)refreshMoreMenu {
    _moreButton.menu = [self buildMoreMenu];
}

- (UIMenu *)buildMoreMenu {
    __weak typeof(self) weakSelf = self;

    UIAction *newFolder = [UIAction actionWithTitle:@"New Folder" image:[UIImage systemImageNamed:@"folder.badge.plus"]
                                          identifier:nil handler:^(UIAction *action) {
        [weakSelf promptNewFolder];
    }];

    NSArray<NSNumber *> *sortModes = @[@(WorkspaceFileManagerSortName), @(WorkspaceFileManagerSortSize),
                                        @(WorkspaceFileManagerSortDate), @(WorkspaceFileManagerSortKind)];
    NSArray<NSString *> *sortTitles = @[@"Name", @"Size", @"Date Modified", @"Kind"];
    NSMutableArray<UIAction *> *sortActions = [NSMutableArray array];
    for (NSUInteger i = 0; i < sortModes.count; i++) {
        WorkspaceFileManagerSortMode mode = (WorkspaceFileManagerSortMode)sortModes[i].integerValue;
        UIAction *sortAction = [UIAction actionWithTitle:sortTitles[i] image:nil identifier:nil handler:^(UIAction *action) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil) return;
            strongSelf->_sortMode = mode;
            [NSUserDefaults.standardUserDefaults setInteger:mode forKey:kFileManagerSortModeDefaultsKey];
            [strongSelf applyFilterAndSort];
            [strongSelf refreshMoreMenu];
        }];
        sortAction.state = (_sortMode == mode) ? UIMenuElementStateOn : UIMenuElementStateOff;
        [sortActions addObject:sortAction];
    }
    UIMenu *sortMenu = [UIMenu menuWithTitle:@"Sort By" image:[UIImage systemImageNamed:@"arrow.up.arrow.down"]
                                   identifier:nil options:0 children:sortActions];

    UIAction *hiddenToggle = [UIAction actionWithTitle:@"Show Hidden Files" image:nil identifier:nil
                                                handler:^(UIAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        strongSelf->_showHidden = !strongSelf->_showHidden;
        [NSUserDefaults.standardUserDefaults setBool:strongSelf->_showHidden forKey:kFileManagerShowHiddenDefaultsKey];
        [strongSelf applyFilterAndSort];
        [strongSelf refreshMoreMenu];
    }];
    hiddenToggle.state = _showHidden ? UIMenuElementStateOn : UIMenuElementStateOff;

    UIAction *refresh = [UIAction actionWithTitle:@"Refresh" image:[UIImage systemImageNamed:@"arrow.clockwise"]
                                        identifier:nil handler:^(UIAction *action) {
        [weakSelf reload];
    }];

    return [UIMenu menuWithTitle:@"" children:@[newFolder, sortMenu, hiddenToggle, refresh]];
}

@end
