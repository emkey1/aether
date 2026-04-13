//
//  Roots.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <FileProvider/FileProvider.h>
#import "Diagnostics.h"
#import "Roots.h"
#import "AppGroup.h"
#import "NSObject+SaneKVO.h"
#include <archive.h>
#include <archive_entry.h>
#include "tools/fakefs.h"
#ifdef __APPLE__
#include <errno.h>
#include <sys/resource.h>
#define IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY 1
#define IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE 1
#endif

static NSURL *RootsDir(void) {
    static NSURL *rootsDir;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        rootsDir = [ContainerURL() URLByAppendingPathComponent:@"roots"];
        NSFileManager *manager = [NSFileManager defaultManager];
        [manager createDirectoryAtURL:rootsDir
          withIntermediateDirectories:YES
                           attributes:@{}
                                error:nil];
    });
    return rootsDir;
}

static NSString *kDefaultRoot = @"Default Root";
static NSString *const kBundledRootIdentifierKey = @"identifier";
static NSString *const kBundledRootDisplayNameKey = @"displayName";
static NSString *const kBundledRootArchiveNameKey = @"archiveName";
static NSString *const kBundledRootImportNameKey = @"importName";
static NSString *const kBundledRootInitialWindowKey = @"initialWindow";
static NSString *const kBundledRootGuestABIKey = @"guestABI";
static NSString *const kRootsErrorDomain = @"iSH.Roots";
static NSString *const kRootMetadataFileName = @"ish-root.plist";
static NSString *const kRootMetadataGuestABIKey = @"guestABI";

NSNotificationName const RootsDidFinishInitialSelectionNotification = @"RootsDidFinishInitialSelectionNotification";

static NSArray<NSDictionary<NSString *, NSString *> *> *BundledRootChoices(void) {
    static NSArray<NSDictionary<NSString *, NSString *> *> *choices;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        choices = @[
            @{
                kBundledRootIdentifierKey: @"alpine3233",
                kBundledRootDisplayNameKey: @"Alpine3.23.3",
                kBundledRootArchiveNameKey: @"alpine-minirootfs-3.23.3-x86",
                kBundledRootImportNameKey: @"Alpine3.23.3",
                kBundledRootInitialWindowKey: @"session-shell",
                kBundledRootGuestABIKey: @"i386",
            },
            @{
                kBundledRootIdentifierKey: @"alpine3233x8664",
                kBundledRootDisplayNameKey: @"Alpine3.23.3(x86_64)",
                kBundledRootArchiveNameKey: @"alpine-minirootfs-3.23.3-x86_64",
                kBundledRootImportNameKey: @"Alpine3.23.3(x86_64)",
                kBundledRootInitialWindowKey: @"session-shell",
                kBundledRootGuestABIKey: @"amd64",
            },
        ];
    });
    return choices;
}

static NSURL *RootMetadataURL(NSURL *rootURL) {
    return [rootURL URLByAppendingPathComponent:kRootMetadataFileName];
}

static NSDictionary<NSString *, id> *ReadRootMetadata(NSURL *rootURL) {
    NSDictionary<NSString *, id> *metadata =
        [NSDictionary dictionaryWithContentsOfURL:RootMetadataURL(rootURL)];
    if (![metadata isKindOfClass:NSDictionary.class])
        return nil;
    return metadata;
}

static void WriteRootMetadata(NSURL *rootURL, NSDictionary<NSString *, id> *metadata) {
    if (metadata.count == 0)
        return;
    NSError *error = nil;
    if (![metadata writeToURL:RootMetadataURL(rootURL) error:&error]) {
        NSLog(@"could not write root metadata for %@: %@", rootURL.lastPathComponent, error);
    }
}

static BOOL RootURLLooksValid(NSURL *url) {
    if (url == nil)
        return NO;

    BOOL isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:url.path isDirectory:&isDirectory] || !isDirectory)
        return NO;

    NSURL *dataURL = [url URLByAppendingPathComponent:@"data"];
    NSURL *metaURL = [url URLByAppendingPathComponent:@"meta.db"];
    BOOL isDataDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:dataURL.path isDirectory:&isDataDirectory] || !isDataDirectory)
        return NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:metaURL.path])
        return NO;

    return YES;
}

static NSString *FormatByteCount(long long value) {
    return [NSByteCountFormatter stringFromByteCount:value countStyle:NSByteCountFormatterCountStyleFile];
}

static NSError *RootsStorageError(NSString *description, NSString *recoverySuggestion) {
    NSMutableDictionary *userInfo = [NSMutableDictionary dictionaryWithObject:description
                                                                       forKey:NSLocalizedDescriptionKey];
    if (recoverySuggestion != nil)
        userInfo[NSLocalizedRecoverySuggestionErrorKey] = recoverySuggestion;
    return [NSError errorWithDomain:kRootsErrorDomain code:NSFileWriteOutOfSpaceError userInfo:userInfo];
}

static NSError *FakefsImportNSError(struct fakefsify_error fs_err) {
    NSString *description = nil;
    NSString *recoverySuggestion = nil;
    NSString *domain = NSPOSIXErrorDomain;
    if (fs_err.type == ERR_SQLITE)
        domain = @"SQLite";

    if (fs_err.type == ERR_POSIX && fs_err.code == ENOSPC) {
        description = @"Not enough free space to extract the filesystem.";
        recoverySuggestion = @"Free up storage space and try again.";
    } else if (fs_err.type == ERR_ARCHIVE) {
        description = @"The filesystem archive could not be read.";
    } else if (fs_err.type == ERR_SQLITE) {
        description = @"The filesystem metadata database could not be created.";
    } else {
        description = [NSString stringWithUTF8String:fs_err.message];
    }

    NSMutableDictionary *userInfo = [NSMutableDictionary dictionaryWithObject:description
                                                                       forKey:NSLocalizedDescriptionKey];
    if (recoverySuggestion != nil)
        userInfo[NSLocalizedRecoverySuggestionErrorKey] = recoverySuggestion;
    if (fs_err.message != NULL && fs_err.message[0] != '\0') {
        userInfo[NSLocalizedFailureReasonErrorKey] = [NSString stringWithFormat:@"%s (line %d)",
                                                      fs_err.message, fs_err.line];
    }
    return [NSError errorWithDomain:domain code:fs_err.code userInfo:userInfo];
}

static BOOL EstimateArchiveExtractionRequirement(NSURL *archiveURL, long long *requiredBytes, NSError **error) {
    struct archive *archive = archive_read_new();
    if (archive == NULL) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"libarchive"
                                         code:ENOMEM
                                     userInfo:@{NSLocalizedDescriptionKey: @"Could not allocate archive reader."}];
        }
        return NO;
    }
    archive_read_support_filter_gzip(archive);
    archive_read_support_filter_bzip2(archive);
    archive_read_support_format_tar(archive);
    if (archive_read_open_filename(archive, archiveURL.fileSystemRepresentation, 65536) != ARCHIVE_OK) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"libarchive"
                                         code:archive_errno(archive)
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    @"The filesystem archive could not be read."}];
        }
        archive_read_free(archive);
        return NO;
    }

    long long payloadBytes = 0;
    long long entryCount = 0;
    struct archive_entry *entry = NULL;
    int err = ARCHIVE_OK;
    while ((err = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
        entryCount++;
        la_int64_t entrySize = archive_entry_size(entry);
        if (entrySize > 0)
            payloadBytes += entrySize;
        archive_read_data_skip(archive);
    }
    if (err != ARCHIVE_EOF) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"libarchive"
                                         code:archive_errno(archive)
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    @"The filesystem archive could not be read."}];
        }
        archive_read_free(archive);
        return NO;
    }
    archive_read_free(archive);

    long long metadataOverhead = MAX(entryCount * 2048LL, 8LL * 1024 * 1024);
    long long safetyMargin = 64LL * 1024 * 1024;
    *requiredBytes = payloadBytes + metadataOverhead + safetyMargin;
    return YES;
}

static BOOL RootsCheckAvailableSpaceForArchive(NSURL *archiveURL, NSURL *destinationDirectory, NSError **error) {
    long long requiredBytes = 0;
    NSError *estimateError = nil;
    if (!EstimateArchiveExtractionRequirement(archiveURL, &requiredBytes, &estimateError)) {
        if (error != NULL)
            *error = estimateError;
        return NO;
    }

    NSNumber *availableBytes = nil;
    NSError *resourceError = nil;
    if (@available(iOS 11.0, *)) {
        NSDictionary<NSURLResourceKey, id> *values = [destinationDirectory resourceValuesForKeys:@[
            NSURLVolumeAvailableCapacityForImportantUsageKey,
            NSURLVolumeAvailableCapacityKey,
        ] error:&resourceError];
        availableBytes = values[NSURLVolumeAvailableCapacityForImportantUsageKey];
        if (availableBytes == nil)
            availableBytes = values[NSURLVolumeAvailableCapacityKey];
    }
    if (availableBytes == nil) {
        NSDictionary<NSFileAttributeKey, id> *attributes =
            [NSFileManager.defaultManager attributesOfFileSystemForPath:destinationDirectory.path error:&resourceError];
        availableBytes = attributes[NSFileSystemFreeSize];
    }
    if (availableBytes == nil) {
        if (error != NULL)
            *error = resourceError;
        return NO;
    }

    if (availableBytes.longLongValue < requiredBytes) {
        if (error != NULL) {
            *error = RootsStorageError([NSString stringWithFormat:
                                        @"Not enough free space to extract the filesystem. About %@ is needed, but only %@ is available.",
                                        FormatByteCount(requiredBytes), FormatByteCount(availableBytes.longLongValue)],
                                       @"Free up storage space and try again.");
        }
        return NO;
    }
    return YES;
}

static void EnableCaseSensitiveFilesystemLookupsIfPossible(void) {
#ifdef __APPLE__
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        int err = setiopolicy_np(IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY,
                                 IOPOL_SCOPE_PROCESS,
                                 IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE);
        if (err != 0 && errno != EPERM) {
            NSLog(@"could not enable case-sensitive filesystem lookups: %s", strerror(errno));
        }
    });
#endif
}

@interface Roots ()
@property NSMutableOrderedSet<NSString *> *roots;
@property BOOL updatingDomains;
@property BOOL domainsNeedUpdate;
@property BOOL wantsVersionFile;
@property BOOL initialBundledRootImportInProgress;
@property (nullable) NSError *initialBundledRootImportError;
@end

@implementation Roots

- (instancetype)init {
    if (self = [super init]) {
        EnableCaseSensitiveFilesystemLookupsIfPossible();
        NSError *error = nil;
        NSArray<NSString *> *rootNames = [NSFileManager.defaultManager contentsOfDirectoryAtPath:RootsDir().path error:&error];
        NSAssert(error == nil, @"couldn't list roots: %@", error);
        NSMutableOrderedSet<NSString *> *roots = [NSMutableOrderedSet orderedSet];
        for (NSString *rootName in rootNames) {
            if (RootURLLooksValid([self rootUrl:rootName])) {
                [roots addObject:rootName];
            } else {
                NSLog(@"ignoring invalid root entry %@", rootName);
            }
        }
        self.roots = roots;
        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = self.roots[0];
            [self syncFileProviderDomains];
        }];
        [self syncFileProviderDomains];

        if ((!self.defaultRoot || ![self.roots containsObject:self.defaultRoot]) && self.roots.count)
            self.defaultRoot = self.roots.firstObject;
    }
    return self;
}

- (NSString *)defaultRoot {
    return [NSUserDefaults.standardUserDefaults stringForKey:kDefaultRoot];
}
- (void)setDefaultRoot:(NSString *)defaultRoot {
    [NSUserDefaults.standardUserDefaults setObject:defaultRoot forKey:kDefaultRoot];
}

- (BOOL)needsInitialRootSelection {
    return self.roots.count == 0;
}

- (NSArray<NSDictionary<NSString *,NSString *> *> *)bundledRootChoices {
    return BundledRootChoices();
}

- (NSURL *)rootUrl:(NSString *)name {
    return [RootsDir() URLByAppendingPathComponent:name];
}

- (nullable NSString *)guestABIForRootNamed:(NSString *)name {
    NSDictionary<NSString *, id> *metadata = ReadRootMetadata([self rootUrl:name]);
    NSString *guestABI = metadata[kRootMetadataGuestABIKey];
    if ([guestABI isKindOfClass:NSString.class] && guestABI.length != 0)
        return guestABI;

    for (NSDictionary<NSString *, NSString *> *choice in BundledRootChoices()) {
        NSString *baseName = choice[kBundledRootImportNameKey];
        NSString *choiceGuestABI = choice[kBundledRootGuestABIKey];
        if (baseName.length == 0 || choiceGuestABI.length == 0)
            continue;
        if ([name isEqualToString:baseName] ||
                [name hasPrefix:[baseName stringByAppendingString:@" "]]) {
            return choiceGuestABI;
        }
    }
    return nil;
}

- (void)syncFileProviderDomains {
    if (self.updatingDomains) {
        self.domainsNeedUpdate = YES;
        return;
    }
    self.updatingDomains = YES;
    self.domainsNeedUpdate = NO;

    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        void (^onError)(NSError *error) = ^(NSError *error) {
            if (error != nil)
                NSLog(@"error adjusting domains: %@", error);
        };
        onError(error);
        NSMutableOrderedSet<NSString *> *missingRoots = [self.roots mutableCopy];
        for (NSFileProviderDomain *domain in domains) {
            if ([missingRoots containsObject:domain.identifier]) {
                [missingRoots removeObject:domain.identifier];
            } else {
                [NSFileManager.defaultManager removeItemAtURL:
                 [NSFileProviderManager.defaultManager.documentStorageURL
                  URLByAppendingPathComponent:domain.pathRelativeToDocumentStorage]
                                                        error:nil];
                [NSFileProviderManager removeDomain:domain completionHandler:onError];
            }
        }
        for (NSString *rootId in missingRoots) {
            [NSFileProviderManager addDomain:[[NSFileProviderDomain alloc] initWithIdentifier:rootId
                                                                                  displayName:rootId
                                                                pathRelativeToDocumentStorage:rootId]
                           completionHandler:onError];
        }
        if (self.domainsNeedUpdate)
            [self syncFileProviderDomains];
        self.updatingDomains = NO;
    }];
}

- (BOOL)accessInstanceVariablesDirectly {
    return YES;
}

- (BOOL)importBundledRootChoice:(NSString *)identifier error:(NSError **)error progressReporter:(id<ProgressReporter>)progress {
    NSDictionary<NSString *, NSString *> *selectedChoice = nil;
    for (NSDictionary<NSString *, NSString *> *choice in BundledRootChoices()) {
        if ([choice[kBundledRootIdentifierKey] isEqualToString:identifier]) {
            selectedChoice = choice;
            break;
        }
    }
    if (selectedChoice == nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"Unknown bundled root choice"
            }];
        }
        return NO;
    }

    NSURL *archive = [NSBundle.mainBundle URLForResource:selectedChoice[kBundledRootArchiveNameKey]
                                           withExtension:@"tar.gz"];
    if (archive == nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"Bundled root archive is missing"
            }];
        }
        return NO;
    }

    NSString *baseName = selectedChoice[kBundledRootImportNameKey];
    NSString *importName = baseName;
    unsigned suffix = 2;
    while ([self.roots containsObject:importName]) {
        importName = [NSString stringWithFormat:@"%@ %u", baseName, suffix++];
    }

    BOOL ok = [self importRootFromArchive:archive
                                     name:importName
                                    error:error
                         progressReporter:progress];
    if (ok) {
        NSString *guestABI = selectedChoice[kBundledRootGuestABIKey];
        if (guestABI.length != 0) {
            WriteRootMetadata([self rootUrl:importName], @{
                kRootMetadataGuestABIKey: guestABI,
            });
        }
        _wantsVersionFile = YES;
    }
    return ok;
}

void root_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    id <ProgressReporter> reporter = (__bridge id<ProgressReporter>) cookie;
    [reporter updateProgress:progress message:[NSString stringWithUTF8String:message]];
    if ([reporter shouldCancel])
        *should_cancel = true;
}

- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    EnableCaseSensitiveFilesystemLookupsIfPossible();
    NSAssert(![self.roots containsObject:name], @"root already exists: %@", name);
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    NSURL *temporaryDirectory = NSFileManager.defaultManager.temporaryDirectory;
    NSURL *tempDestination = [temporaryDirectory URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]];
    if (tempDestination == nil)
        return NO;
    NSError *spaceError = nil;
    if (!RootsCheckAvailableSpaceForArchive(archive, temporaryDirectory, &spaceError)) {
        if (error != NULL)
            *error = spaceError;
        [ISHDiagnosticsStore recordBreadcrumb:@"root.importPreflightFailed"
                                      details:@{@"name": name ?: @"",
                                                @"error": spaceError.localizedDescription ?: @"unknown"}];
        return NO;
    }
    if (!fakefs_import(archive.fileSystemRepresentation,
                       tempDestination.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        if (error != NULL) {
            *error = FakefsImportNSError(fs_err);
            if (fs_err.type == ERR_CANCELLED)
                *error = nil;
        }
        if (fs_err.type != ERR_CANCELLED) {
            NSError *reportedError = error != NULL ? *error : FakefsImportNSError(fs_err);
            [ISHDiagnosticsStore recordBreadcrumb:@"root.importFailed"
                                          details:@{@"name": name ?: @"",
                                                    @"error": reportedError.localizedDescription ?: @"unknown"}];
        }
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
    if (![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error]) {
        NSError *reportedError = error != NULL ? *error : nil;
        [ISHDiagnosticsStore recordBreadcrumb:@"root.importMoveFailed"
                                      details:@{@"name": name ?: @"",
                                                @"error": reportedError.localizedDescription ?: @"unknown"}];
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }

    void (^addRoot)(void) = ^{
        [[self mutableOrderedSetValueForKey:@"roots"] addObject:name];
    };
    if (!NSThread.isMainThread)
        dispatch_sync(dispatch_get_main_queue(), addRoot);
    else
        addRoot();
    return YES;
}

- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert([self.roots containsObject:name], @"trying to export a root that doesn't exist: %@", name);
    struct fakefsify_error fs_err;
    if (!fakefs_export([self rootUrl:name].fileSystemRepresentation,
                       archive.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        // TODO: dedup with above method
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:fs_err.message]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        return NO;
    }
    return YES;
}

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot delete the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error])
        return NO;
    [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name];
    return YES;
}

- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error {
    if (name.length == 0) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be empty"}];
        return NO;
    }
    if ([name containsString:@"/"]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't contain /"}];
        return NO;
    }
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be . or .."}];
        return NO;
    }
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot rename the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    
    if (![NSFileManager.defaultManager moveItemAtURL:[self rootUrl:name] toURL:[self rootUrl:newName] error:error])
        return NO;
    NSUInteger index = [self.roots indexOfObject:name];
    [[self mutableOrderedSetValueForKey:@"roots"] replaceObjectAtIndex:index withObject:newName];
    return YES;
}

+ (instancetype)instance {
    static Roots *instance;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        instance = [Roots new];
    });
    return instance;
}

@end
