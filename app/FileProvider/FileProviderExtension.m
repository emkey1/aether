//
//  FileProviderExtension.m
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import "FileProviderExtension.h"
#import "FileProviderItem.h"
#import "FileProviderEnumerator.h"
#import "NSError+ISHErrno.h"
#import "../AppGroup.h"
#include "fs/fake-db.h"

static NSNumber *ISHFileProviderDurationMilliseconds(NSTimeInterval start) {
    return @((NSInteger) ((NSDate.date.timeIntervalSinceReferenceDate - start) * 1000.0));
}

static NSString *const kISHFileProviderDiagnosticsDirectory = @"Diagnostics";
static NSString *const kISHFileProviderDiagnosticsBreadcrumbsFile = @"breadcrumbs.json";

static dispatch_queue_t ISHFileProviderBreadcrumbQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("app.ish.iSH-AOK.FileProvider.Diagnostics", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

static NSURL *ISHFileProviderDiagnosticsDirectoryURL(void) {
    NSURL *baseURL = ContainerURL();
    if (baseURL == nil) {
        baseURL = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                      inDomains:NSUserDomainMask].firstObject;
    }
    if (baseURL == nil)
        return nil;
    return [baseURL URLByAppendingPathComponent:kISHFileProviderDiagnosticsDirectory isDirectory:YES];
}

static NSURL *ISHFileProviderBreadcrumbsURL(void) {
    NSURL *directoryURL = ISHFileProviderDiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return nil;
    return [directoryURL URLByAppendingPathComponent:kISHFileProviderDiagnosticsBreadcrumbsFile isDirectory:NO];
}

static NSString *ISHFileProviderISO8601StringFromDate(NSDate *date) {
    if (date == nil)
        return nil;
    static NSISO8601DateFormatter *formatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        formatter = [NSISO8601DateFormatter new];
        formatter.formatOptions = NSISO8601DateFormatWithInternetDateTime;
    });
    return [formatter stringFromDate:date];
}

void ISHFileProviderRecordBreadcrumb(NSString *event, NSDictionary<NSString *, id> *details) {
    if (event.length == 0)
        return;
    dispatch_async(ISHFileProviderBreadcrumbQueue(), ^{
        NSURL *directoryURL = ISHFileProviderDiagnosticsDirectoryURL();
        NSURL *breadcrumbsURL = ISHFileProviderBreadcrumbsURL();
        if (directoryURL == nil || breadcrumbsURL == nil)
            return;

        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];
        NSData *existingData = [NSData dataWithContentsOfURL:breadcrumbsURL];
        NSMutableArray<NSDictionary<NSString *, id> *> *breadcrumbs = [NSMutableArray array];
        if (existingData.length > 0) {
            id existingObject = [NSJSONSerialization JSONObjectWithData:existingData options:NSJSONReadingMutableContainers error:nil];
            if ([existingObject isKindOfClass:[NSArray class]])
                [breadcrumbs addObjectsFromArray:existingObject];
        }

        NSMutableDictionary<NSString *, id> *entry = [NSMutableDictionary dictionary];
        entry[@"timestamp"] = ISHFileProviderISO8601StringFromDate([NSDate date]) ?: @"";
        entry[@"event"] = event;
        if (details.count != 0)
            entry[@"details"] = details;
        [breadcrumbs addObject:entry];

        const NSUInteger maxBreadcrumbs = 200;
        if (breadcrumbs.count > maxBreadcrumbs) {
            NSRange overflow = NSMakeRange(0, breadcrumbs.count - maxBreadcrumbs);
            [breadcrumbs removeObjectsInRange:overflow];
        }

        NSData *jsonData = [NSJSONSerialization dataWithJSONObject:breadcrumbs options:NSJSONWritingPrettyPrinted error:nil];
        if (jsonData != nil)
            [jsonData writeToURL:breadcrumbsURL options:NSDataWritingAtomic error:nil];
    });
}

static NSMutableDictionary<NSString *, id> *ISHFileProviderDetails(NSString * _Nullable domainIdentifier) {
    NSMutableDictionary<NSString *, id> *details = [NSMutableDictionary dictionary];
    if (domainIdentifier.length != 0)
        details[@"domain"] = domainIdentifier;
    return details;
}

static NSString *ISHFileProviderCleanupDefaultsKey(NSString * _Nullable domainIdentifier) {
    NSString *suffix = domainIdentifier.length != 0 ? domainIdentifier : @"default";
    return [NSString stringWithFormat:@"ISHFileProviderLastCleanup.%@", suffix];
}

static int ISHFileProviderAcquireRootLock(NSString *domainIdentifier, BOOL exclusive) {
    NSError *error = nil;
    int fd = ISHAppGroupAcquireNamedLock(@"root", domainIdentifier, exclusive, &error);
    if (fd < 0) {
        NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(domainIdentifier);
        details[@"mode"] = exclusive ? @"exclusive" : @"shared";
        details[@"error"] = error.localizedDescription ?: @"unknown";
        ISHFileProviderRecordBreadcrumb(@"fileprovider.rootlock.failed", details);
    }
    return fd;
}

@interface ISHFileProviderMount ()

@property (nonatomic) struct fakefs_mount storage;
@property (nonatomic, readwrite) NSString *domainIdentifier;
@property (nonatomic, readwrite) NSURL *rootURL;
@property (nonatomic, readwrite) NSRecursiveLock *ioLock;

@end

@implementation ISHFileProviderMount

- (nullable instancetype)initWithDomainIdentifier:(NSString *)domainIdentifier error:(NSError **)error {
    self = [super init];
    if (self == nil)
        return nil;

    _storage.root_fd = -1;
    NSURL *container = ContainerURL();
    NSURL *fs_dir = [[container URLByAppendingPathComponent:@"roots"]
                     URLByAppendingPathComponent:domainIdentifier];
    _domainIdentifier = [domainIdentifier copy];
    _rootURL = [fs_dir URLByAppendingPathComponent:@"data"];
    _ioLock = [[NSRecursiveLock alloc] init];
    _storage.source = strdup(_rootURL.fileSystemRepresentation);
    _storage.root_fd = open(_storage.source, O_RDONLY | O_DIRECTORY);
    if (_storage.root_fd < 0) {
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        free((void *) _storage.source);
        _storage.source = NULL;
        return nil;
    }

    int err = fake_db_init(&_storage.db,
                           [fs_dir URLByAppendingPathComponent:@"meta.db"].fileSystemRepresentation,
                           _storage.root_fd);
    if (err < 0) {
        NSLog(@"error opening root: %d", err);
        close(_storage.root_fd);
        _storage.root_fd = -1;
        free((void *) _storage.source);
        _storage.source = NULL;
        if (error != nil)
            *error = [NSError errorWithISHErrno:err itemIdentifier:NSFileProviderRootContainerItemIdentifier];
        return nil;
    }

    return self;
}

- (struct fakefs_mount *)mount {
    return &_storage;
}

- (void)dealloc {
    if (_storage.source != NULL) {
        free((void *) _storage.source);
        _storage.source = NULL;
    }
    if (_storage.root_fd >= 0) {
        close(_storage.root_fd);
        _storage.root_fd = -1;
    }
    if (_storage.db.db != NULL)
        fake_db_deinit(&_storage.db);
}

@end

@interface FileProviderExtension () {
}
@property (nonatomic) ISHFileProviderMount *mountOwner;
@end

@implementation FileProviderExtension

- (struct fakefs_mount *)mount {
    NSAssert(self.mountOwner != nil, @"");
    return self.mountOwner.mount;
}

- (nullable ISHFileProviderMount *)getMountOwnerWithError:(NSError **)error {
    NSString *domainIdentifier = self.domain.identifier ?: @"";
    int rootLockFd = ISHFileProviderAcquireRootLock(domainIdentifier, NO);
    ISHFileProviderMount *mountOwner = nil;
    BOOL shouldCleanup = NO;
    NSString *cleanupKey = ISHFileProviderCleanupDefaultsKey(domainIdentifier);
    @try {
        @synchronized (self) {
            mountOwner = self.mountOwner;
            if (mountOwner == nil) {
                if (self.domain == nil) {
                    if (error != nil)
                        *error = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNotAuthenticated userInfo:nil];
                    return nil;
                }
                ISHFileProviderRecordBreadcrumb(@"fileprovider.mount.create.begin",
                                                ISHFileProviderDetails(domainIdentifier));
                NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
                mountOwner = [[ISHFileProviderMount alloc] initWithDomainIdentifier:self.domain.identifier error:error];
                if (mountOwner == nil) {
                    NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(domainIdentifier);
                    details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
                    if (error != nil && *error != nil)
                        details[@"error"] = (*error).localizedDescription ?: @"unknown";
                    ISHFileProviderRecordBreadcrumb(@"fileprovider.mount.create.failed", details);
                    return nil;
                }
                self.mountOwner = mountOwner;
                NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(domainIdentifier);
                details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
                ISHFileProviderRecordBreadcrumb(@"fileprovider.mount.create.end", details);
            }
        }

        // Keep the synchronized section and shared root lock small; cleanup can
        // walk the filesystem and will take its own exclusive root lock.
        NSDate *lastCleanup = [NSUserDefaults.standardUserDefaults objectForKey:cleanupKey];
        lastCleanup = lastCleanup ? lastCleanup : NSDate.distantPast;
        shouldCleanup = [NSDate.date timeIntervalSinceDate:lastCleanup] > 60 * 60 /* 1 hour */;
    } @finally {
        ISHAppGroupReleaseLock(rootLockFd);
    }

    if (shouldCleanup) {
        [self cleanupStorage];
        [NSUserDefaults.standardUserDefaults setObject:NSDate.date forKey:cleanupKey];
    }

    return mountOwner;
}

- (NSURL *)storageURL {
    NSURL *storage = NSFileProviderManager.defaultManager.documentStorageURL;
    if (self.domain != nil)
        storage = [storage URLByAppendingPathComponent:self.domain.pathRelativeToDocumentStorage isDirectory:YES];
    return storage;
}

- (nullable NSFileProviderItem)itemForIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError * _Nullable *)error {
    NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
    ISHFileProviderMount *mountOwner = [self getMountOwnerWithError:error];
    if (mountOwner == nil) {
        NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(self.domain.identifier);
        details[@"identifier"] = identifier ?: @"";
        details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
        if (error != nil && *error != nil)
            details[@"error"] = (*error).localizedDescription ?: @"unknown";
        ISHFileProviderRecordBreadcrumb(@"fileprovider.item.lookup.failed", details);
        return nil;
    }
    int rootLockFd = ISHFileProviderAcquireRootLock(self.domain.identifier ?: @"", NO);
    @try {
        NSLog(@"item for id %@", identifier);
        NSError *err;
        FileProviderItem *item = [[FileProviderItem alloc] initWithIdentifier:identifier mountOwner:mountOwner error:&err];
        if (item == nil) {
            if (error != nil)
                *error = err;
            NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(self.domain.identifier);
            details[@"identifier"] = identifier ?: @"";
            details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
            details[@"error"] = err.localizedDescription ?: @"unknown";
            ISHFileProviderRecordBreadcrumb(@"fileprovider.item.lookup.failed", details);
            return nil;
        }
        NSNumber *duration = ISHFileProviderDurationMilliseconds(start);
        if (duration.integerValue >= 200) {
            NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(self.domain.identifier);
            details[@"identifier"] = identifier ?: @"";
            details[@"duration_ms"] = duration;
            ISHFileProviderRecordBreadcrumb(@"fileprovider.item.lookup.slow", details);
        }
        return item;
    } @finally {
        ISHAppGroupReleaseLock(rootLockFd);
    }
}

- (nullable NSURL *)URLForItemWithPersistentIdentifier:(NSFileProviderItemIdentifier)identifier {
    if ([identifier isEqualToString:NSFileProviderRootContainerItemIdentifier])
        return self.storageURL;
    FileProviderItem *item = [self itemForIdentifier:identifier error:nil];
    if (item == nil)
        return nil;
    NSURL *url = [self.storageURL URLByAppendingPathComponent:identifier isDirectory:YES];
    url = [url URLByAppendingPathComponent:item.path.lastPathComponent isDirectory:NO];
    NSLog(@"url for id %@ = %@", identifier, url);
    return url;
}

- (nullable NSFileProviderItemIdentifier)persistentIdentifierForItemAtURL:(NSURL *)url {
    if ([url.URLByDeletingLastPathComponent isEqual:NSFileProviderManager.defaultManager.documentStorageURL]) {
        NSAssert([self.domain.identifier isEqualToString:url.lastPathComponent], @"url isn't the same as our domain");
        return NSFileProviderRootContainerItemIdentifier;
    }
    NSString *identifier = url.pathComponents[url.pathComponents.count - 2];
    if (identifier.longLongValue == 0 && !ISHFileProviderIsVirtualIdentifier(identifier))
        return nil; // something must be screwed I guess
    NSLog(@"id for url %@ = %@", url, identifier);
    return identifier;
}

- (BOOL)enhanceSanityOfURL:(NSURL *)url error:(NSError **)error {
    NSURL *dir = url.URLByDeletingLastPathComponent;
    NSFileManager *manager = NSFileManager.defaultManager;
    BOOL isDir;
    if ([manager fileExistsAtPath:dir.path isDirectory:&isDir] && !isDir)
        [manager removeItemAtURL:dir error:nil];
    return [manager createDirectoryAtURL:dir
             withIntermediateDirectories:YES
                              attributes:nil
                                   error:error];
}

- (void)providePlaceholderAtURL:(NSURL *)url completionHandler:(void (^)(NSError * _Nullable error))completionHandler {
    NSError *err;
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:&err];
    if (item == nil) {
        completionHandler(err);
        return;
    }
    if (![self enhanceSanityOfURL:url error:&err]) {
        completionHandler(err);
        return;
    }
    if (![NSFileProviderManager writePlaceholderAtURL:[NSFileProviderManager placeholderURLForURL:url]
                                         withMetadata:item
                                                error:&err]) {
        completionHandler(err);
        return;
    }
    completionHandler(nil);
}

- (void)startProvidingItemAtURL:(NSURL *)url completionHandler:(void (^)(NSError *))completionHandler {
    // Should ensure that the actual file is in the position returned by URLForItemWithIdentifier:, then call the completion handler
    NSError *err;
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:&err];
    if (item == nil) {
        completionHandler(err);
        return;
    }
    if (![self enhanceSanityOfURL:url error:&err]) {
        completionHandler(err);
        return;
    }
    [item loadToURL:url];
    completionHandler(nil);
}

- (void)itemChangedAtURL:(NSURL *)url {
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:nil];
    if (item == nil)
        return;
    [item saveFromURL:url];
}

#pragma mark - Action helpers

// FIXME: not dry enough
// These helpers assume the mount has already been created by itemForIdentifier:error:
- (BOOL)doCreateDirectoryAt:(NSString *)path inode:(ino_t *)inode error:(NSError **)error {
    struct fakefs_mount *mount = self.mountOwner.mount;
    NSURL *url = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]] URLByAppendingPathComponent:path];
    if (![NSFileManager.defaultManager createDirectoryAtURL:url
                                withIntermediateDirectories:NO
                                                 attributes:@{NSFilePosixPermissions: @0777}
                                                      error:error]) {
        return NO;
    }
    db_begin_write(&mount->db);
    struct ish_stat stat;
    NSString *parentPath = [path substringToIndex:[path rangeOfString:@"/" options:NSBackwardsSearch].location];
    if (!path_read_stat(&mount->db, parentPath.fileSystemRepresentation, &stat, NULL)) {
        db_rollback(&mount->db);
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        if (error != nil)
            *error = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil];
        return NO;
    }
    stat.mode = (stat.mode & ~S_IFMT) | S_IFDIR;
    path_create(&mount->db, path.fileSystemRepresentation, &stat);
    if (inode != NULL)
        *inode = path_get_inode(&mount->db, path.fileSystemRepresentation);
    db_commit(&mount->db);
    return YES;
}

- (BOOL)doCreateFileAt:(NSString *)path importFrom:(NSURL *)importURL inode:(ino_t *)inode error:(NSError **)error {
    struct fakefs_mount *mount = self.mountOwner.mount;
    NSURL *url = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]] URLByAppendingPathComponent:path];
    if (![NSFileManager.defaultManager copyItemAtURL:importURL
                                               toURL:url
                                               error:error]) {
        return NO;
    }
    db_begin_write(&mount->db);
    struct ish_stat stat;
    NSString *parentPath = [path substringToIndex:[path rangeOfString:@"/" options:NSBackwardsSearch].location];
    if (!path_read_stat(&mount->db, parentPath.fileSystemRepresentation, &stat, NULL)) {
        db_rollback(&mount->db);
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        if (error != nil)
            *error = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil];
        return NO;
    }
    stat.mode = (stat.mode & ~S_IFMT & ~0111) | S_IFREG;
    path_create(&mount->db, path.fileSystemRepresentation, &stat);
    if (inode != NULL)
        *inode = path_get_inode(&mount->db, path.fileSystemRepresentation);
    db_commit(&mount->db);
    return YES;
}

- (NSString *)pathOfItemWithIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError **)error {
    FileProviderItem *parent = [self itemForIdentifier:identifier error:error];
    if (parent == nil)
        return nil;
    return parent.path;
}

#pragma mark - Actions

/* TODO: implement the actions for items here
 each of the actions follows the same pattern:
 - make a note of the change in the local model
 - schedule a server request as a background task to inform the server of the change
 - call the completion block with the modified item in its post-modification state
 */

- (void)createDirectoryWithName:(NSString *)directoryName inParentItemIdentifier:(NSFileProviderItemIdentifier)parentItemIdentifier completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *parentPath = [self pathOfItemWithIdentifier:parentItemIdentifier error:&error];
    if (parentPath == nil) {
        completionHandler(nil, error);
        return;
    }
    ino_t inode;
    int rootLockFd = ISHFileProviderAcquireRootLock(self.domain.identifier ?: @"", YES);
    [self.mountOwner.ioLock lock];
    BOOL worked = [self doCreateDirectoryAt:[parentPath stringByAppendingFormat:@"/%@", directoryName] inode:&inode error:&error];
    [self.mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked) {
        completionHandler(nil, error);
        return;
    }
    FileProviderItem *item = [self itemForIdentifier:[NSString stringWithFormat:@"%lu", (unsigned long) inode] error:&error];
    if (item == nil)
        completionHandler(nil, error);
    else
        completionHandler(item, nil);
}

- (void)importDocumentAtURL:(NSURL *)fileURL toParentItemIdentifier:(NSFileProviderItemIdentifier)parentItemIdentifier completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *parentPath = [self pathOfItemWithIdentifier:parentItemIdentifier error:&error];
    if (parentPath == nil) {
        completionHandler(nil, error);
        return;
    }
    
    [fileURL startAccessingSecurityScopedResource];
    BOOL isDir;
    assert([NSFileManager.defaultManager fileExistsAtPath:fileURL.path isDirectory:&isDir] && !isDir);
    ino_t inode;
    int rootLockFd = ISHFileProviderAcquireRootLock(self.domain.identifier ?: @"", YES);
    [self.mountOwner.ioLock lock];
    BOOL worked = [self doCreateFileAt:[parentPath stringByAppendingFormat:@"/%@", fileURL.lastPathComponent]
                            importFrom:fileURL
                                 inode:&inode
                                 error:&error];
    [self.mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    [fileURL stopAccessingSecurityScopedResource];
    if (!worked) {
        completionHandler(nil, error);
        return;
    }
    
    FileProviderItem *item = [self itemForIdentifier:[NSString stringWithFormat:@"%lu", (unsigned long) inode] error:&error];
    if (item == nil)
        completionHandler(nil, error);
    else
        completionHandler(item, nil);
}

- (NSString *)pathFromURL:(NSURL *)url {
    struct fakefs_mount *mount = self.mountOwner.mount;
    NSURL *root = [NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]];
    assert([url.path hasPrefix:root.path]);
    NSString *path = [url.path substringFromIndex:root.path.length];
    assert([path hasPrefix:@"/"]);
    if ([path hasSuffix:@"/"])
        path = [path substringToIndex:path.length - 1];
    return path;
}

- (BOOL)doDelete:(NSString *)path itemIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError **)error {
    struct fakefs_mount *mount = self.mountOwner.mount;
    NSURL *url = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]] URLByAppendingPathComponent:path];
    NSDirectoryEnumerator<NSURL *> *enumerator = [NSFileManager.defaultManager enumeratorAtURL:url
                                                                    includingPropertiesForKeys:nil
                                                                                       options:NSDirectoryEnumerationSkipsSubdirectoryDescendants
                                                                                  errorHandler:nil];
    for (NSURL *suburl in enumerator) {
        if (![self doDelete:[self pathFromURL:suburl] itemIdentifier:identifier error:error])
            return NO;
    }
    db_begin_write(&mount->db);
    path_unlink(&mount->db, path.fileSystemRepresentation);
    int err = unlinkat(mount->root_fd, fix_path(path.fileSystemRepresentation), 0);
    if (err < 0)
        err = unlinkat(mount->root_fd, fix_path(path.fileSystemRepresentation), AT_REMOVEDIR);
    if (err < 0) {
        db_rollback(&mount->db);
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return NO;
    }
    db_commit(&mount->db);
    return YES;
}

- (void)deleteItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *path = [self pathOfItemWithIdentifier:itemIdentifier error:&error];
    if (path == nil) {
        completionHandler(error);
        return;
    }
    int rootLockFd = ISHFileProviderAcquireRootLock(self.domain.identifier ?: @"", YES);
    [self.mountOwner.ioLock lock];
    BOOL worked = [self doDelete:path itemIdentifier:itemIdentifier error:&error];
    [self.mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked)
        completionHandler(error);
    else
        completionHandler(nil);
}

- (BOOL)doRename:(NSString *)src to:(NSString *)dst itemIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError **)error {
    struct fakefs_mount *mount = self.mountOwner.mount;
    db_begin_write(&mount->db);
    path_rename(&mount->db, src.fileSystemRepresentation, dst.fileSystemRepresentation);
    int err = renameat(mount->root_fd, fix_path(src.fileSystemRepresentation), mount->root_fd, fix_path(dst.fileSystemRepresentation));
    if (err < 0) {
        db_rollback(&mount->db);
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return NO;
    }
    db_commit(&mount->db);
    return YES;
}

- (void)renameItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier toName:(NSString *)itemName completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    FileProviderItem *item = [self itemForIdentifier:itemIdentifier error:&error];
    if (item == nil) {
        completionHandler(nil, error);
        return;
    }
    NSString *dstPath = [item.path.stringByDeletingLastPathComponent stringByAppendingPathComponent:itemName];
    int rootLockFd = ISHFileProviderAcquireRootLock(self.domain.identifier ?: @"", YES);
    [self.mountOwner.ioLock lock];
    BOOL worked = [self doRename:item.path to:dstPath itemIdentifier:itemIdentifier error:&error];
    [self.mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked) {
        completionHandler(nil, error);
        return;
    }
    completionHandler(item, nil);
}

- (void)reparentItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier toParentItemWithIdentifier:(NSFileProviderItemIdentifier)parentItemIdentifier newName:(NSString *)newName completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    FileProviderItem *item = [self itemForIdentifier:itemIdentifier error:&error];
    if (item == nil) {
        completionHandler(nil, error);
        return;
    }
    FileProviderItem *parent = [self itemForIdentifier:parentItemIdentifier error:&error];
    if (parent == nil) {
        completionHandler(nil, error);
        return;
    }
    if (newName == nil)
        newName = item.path.lastPathComponent;
    int rootLockFd = ISHFileProviderAcquireRootLock(self.domain.identifier ?: @"", YES);
    [self.mountOwner.ioLock lock];
    BOOL worked = [self doRename:item.path to:[parent.path stringByAppendingPathComponent:newName] itemIdentifier:itemIdentifier error:&error];
    [self.mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked) {
        completionHandler(nil, error);
        return;
    }
    completionHandler(item, nil);
}

#pragma mark - Enumeration

- (nullable id<NSFileProviderEnumerator>)enumeratorForContainerItemIdentifier:(NSFileProviderItemIdentifier)containerItemIdentifier error:(NSError **)error {
    FileProviderItem *item = [self itemForIdentifier:containerItemIdentifier error:error];
    if (item == nil)
        return nil;
    return [[FileProviderEnumerator alloc] initWithItem:item];
}

#pragma mark - Storage deletion

// According to an engineer I talked to at WWDC, -stopProvidingItemAtURL: is never ever called, so that can't be used to free up disk space.
// Solution for now is to periodically look for and delete files in file provider storage where the original is missing.
// TODO: Delete files in file provider storage when the original file is deleted
// TODO: Create hardlinks into file provider storage instead of copies
//
- (void)cleanupStorage {
    ISHFileProviderMount *mountOwner = self.mountOwner;
    NSAssert(mountOwner != nil, @"Mount should exist by this point");

    NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
    NSFileManager *manager = NSFileManager.defaultManager;
    int rootLockFd = ISHFileProviderAcquireRootLock(self.domain.identifier ?: @"", YES);
    [mountOwner.ioLock lock];
    NSArray<NSURL *> *storageDirs = [manager contentsOfDirectoryAtURL:self.storageURL includingPropertiesForKeys:nil options:0 error:nil];
    NSUInteger removedCount = 0;
    for (NSURL *dir in storageDirs) {
        inode_t inode = dir.lastPathComponent.longLongValue;
        if (inode == 0)
            continue;

        BOOL exists = inode_exists(&mountOwner.mount->db, inode);

        if (!exists) {
            NSLog(@"removing dead inode %llu", inode);
            NSError *err;
            if (![manager removeItemAtURL:dir error:&err])
                NSLog(@"failed to remove dead inode: %@", err);
            else
                removedCount++;
        }
    }
    [mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    NSNumber *duration = ISHFileProviderDurationMilliseconds(start);
    if (removedCount != 0 || duration.integerValue >= 500) {
        NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(self.domain.identifier);
        details[@"duration_ms"] = duration;
        details[@"removed"] = @(removedCount);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.cleanup", details);
    }
}

// Dead code, leaving it here just in case
- (void)stopProvidingItemAtURL:(NSURL *)url {
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:nil];
    if (item == nil)
        return;
    [item saveFromURL:url];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    [NSFileProviderManager writePlaceholderAtURL:[NSFileProviderManager placeholderURLForURL:url]
                                    withMetadata:item
                                           error:nil];
}

@end

void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    [NSException raise:@"ish die" format:[NSString stringWithUTF8String:msg] arguments:args];
    abort();
    va_end(args);
}

void ish_printk(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    NSLogv([NSString stringWithUTF8String:msg], args);
    va_end(args);
}
