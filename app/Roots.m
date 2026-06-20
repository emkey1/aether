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
// A single file-provider domain hosts every installed root as a folder, instead
// of registering one domain per root (which spammed the Files sidebar). Must
// stay in sync with the displayName shown in Files and the deep-link path.
static NSString *const kISHFileProviderDomainIdentifier = @"iSH-AOK";
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
                // importName becomes a directory name and the whitespace-delimited
                // mount "source" string parsed by guest init scripts, so it must
                // pass RootNameIsValid (no shell/path metacharacters). The pretty
                // parenthesized form lives in displayName, which is all the UI shows.
                kBundledRootImportNameKey: @"Alpine3.23.3-x86_64",
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
    archive_read_support_filter_all(archive); // gzip, bzip2, xz, zstd, ... (match fakefs_import)
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
    while (true) {
        err = archive_read_next_header(archive, &entry);
        if (err == ARCHIVE_EOF)
            break;
        // ARCHIVE_WARN is non-fatal (e.g. a UTF-8 link path that can't also be
        // rendered in the process locale); keep estimating. Only hard errors abort.
        if (err != ARCHIVE_OK && err != ARCHIVE_WARN) {
            if (error != NULL) {
                *error = [NSError errorWithDomain:@"libarchive"
                                             code:archive_errno(archive)
                                         userInfo:@{NSLocalizedDescriptionKey:
                                                        @"The filesystem archive could not be read."}];
            }
            archive_read_free(archive);
            return NO;
        }
        entryCount++;
        la_int64_t entrySize = archive_entry_size(entry);
        if (entrySize > 0)
            payloadBytes += entrySize;
        archive_read_data_skip(archive);
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
@property BOOL fileProviderDomainSyncEnabled;
@property NSUInteger fileProviderDomainSyncGeneration;
@property BOOL wantsVersionFile;
@property BOOL initialBundledRootImportInProgress;
@property (nullable) NSError *initialBundledRootImportError;
@end

// A root's name becomes a directory name and, once mounted, the filesystem
// "source" string that guest init scripts parse with whitespace-delimited
// tools (df, mount, ...). A space or shell/path metacharacter there breaks
// stock scripts (e.g. mountall.sh's df parsing) or allows path traversal, so
// restrict names to a conservative, path- and shell-safe character set.
static BOOL RootNameIsValid(NSString *name, NSError **error) {
    NSString *reason = nil;
    if (name.length == 0) {
        reason = @"Filesystem name can't be empty";
    } else if ([name hasPrefix:@"."]) {
        reason = @"Filesystem name can't start with '.'";
    } else {
        static NSCharacterSet *disallowed;
        static dispatch_once_t onceToken;
        dispatch_once(&onceToken, ^{
            disallowed = [[NSCharacterSet characterSetWithCharactersInString:
                @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"] invertedSet];
        });
        if ([name rangeOfCharacterFromSet:disallowed].location != NSNotFound)
            reason = @"Filesystem name can only contain letters, numbers, '.', '-', and '_' (no spaces)";
    }
    if (reason != nil) {
        if (error != NULL)
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: reason}];
        return NO;
    }
    return YES;
}

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
        self.fileProviderDomainSyncEnabled = NO;
        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = self.roots[0];
            [self requestFileProviderDomainSync];
        }];
        [self requestFileProviderDomainSync];

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
                [name hasPrefix:[baseName stringByAppendingString:@"_"]]) {
            return choiceGuestABI;
        }
    }
    return nil;
}

- (NSArray<NSURL *> *)cachedRootArchiveURLs {
    // /AOK/persist is a single shared real-fs mount — the AppGroup container's
    // AOK/persist directory (see AOKPersistDirectoryURL / the do_mount in
    // AppDelegate). It is the same regardless of which root is booted and is NOT
    // inside any root's fakefs data dir (that's just an empty mount point), so we
    // scan the shared host directory directly.
    NSURL *container = ContainerURL();
    if (container == nil)
        return @[];
    NSURL *cacheDir = [[[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
                        URLByAppendingPathComponent:@"persist" isDirectory:YES]
                       URLByAppendingPathComponent:@"roots" isDirectory:YES];
    NSArray<NSURL *> *entries = [NSFileManager.defaultManager
        contentsOfDirectoryAtURL:cacheDir
      includingPropertiesForKeys:@[NSURLIsRegularFileKey, NSURLFileSizeKey]
                         options:NSDirectoryEnumerationSkipsHiddenFiles
                           error:nil];

    static NSArray<NSString *> *suffixes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        suffixes = @[@".tar", @".tar.gz", @".tgz", @".tar.bz2", @".tbz", @".tbz2",
                     @".tar.xz", @".txz", @".tar.zst", @".tzst", @".tar.lz", @".tar.lzma"];
    });

    NSMutableArray<NSURL *> *archives = [NSMutableArray array];
    for (NSURL *entry in entries) {
        NSNumber *isRegular = nil;
        [entry getResourceValue:&isRegular forKey:NSURLIsRegularFileKey error:nil];
        if (!isRegular.boolValue)
            continue;
        NSString *lower = entry.lastPathComponent.lowercaseString;
        for (NSString *suffix in suffixes) {
            if ([lower hasSuffix:suffix]) {
                [archives addObject:entry];
                break;
            }
        }
    }
    [archives sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];
    return archives;
}

- (void)syncFileProviderDomains {
    [self requestFileProviderDomainSync];
}

- (void)resumeDeferredFileProviderDomainSync {
    @synchronized (self) {
        if (self.fileProviderDomainSyncEnabled)
            return;
        self.fileProviderDomainSyncEnabled = YES;
        self.domainsNeedUpdate = YES;
    }
    [ISHDiagnosticsStore recordBreadcrumb:@"fileprovider.domainSync.resumed"];
    [self requestFileProviderDomainSync];
}

- (void)requestFileProviderDomainSync {
    NSArray<NSString *> *rootsSnapshot = nil;
    NSUInteger requestedGeneration = 0;
    @synchronized (self) {
        self.fileProviderDomainSyncGeneration++;
        requestedGeneration = self.fileProviderDomainSyncGeneration;
        if (!self.fileProviderDomainSyncEnabled) {
            self.domainsNeedUpdate = YES;
            [ISHDiagnosticsStore recordBreadcrumb:@"fileprovider.domainSync.deferred"
                                          details:@{@"roots": @(self.roots.count),
                                                    @"generation": @(requestedGeneration)}];
            return;
        }
        if (self.updatingDomains) {
            self.domainsNeedUpdate = YES;
            return;
        }
        self.updatingDomains = YES;
        self.domainsNeedUpdate = NO;
        rootsSnapshot = self.roots.array.copy;
    }

    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        void (^onError)(NSError *error) = ^(NSError *error) {
            if (error != nil)
                NSLog(@"error adjusting domains: %@", error);
        };
        onError(error);
        NSLog(@"syncing the iSH-AOK file provider domain for %lu root(s)", (unsigned long) rootsSnapshot.count);

        // Tell Files to re-list the roots (the domain root container) whenever the
        // set of installed roots changes.
        void (^signalRoots)(NSFileProviderDomain *) = ^(NSFileProviderDomain *domain) {
            if (domain == nil)
                return;
            NSFileProviderManager *manager = [NSFileProviderManager managerForDomain:domain];
            [manager signalEnumeratorForContainerItemIdentifier:NSFileProviderRootContainerItemIdentifier
                                              completionHandler:^(NSError *signalError) {
                if (signalError != nil)
                    NSLog(@"error signaling root enumerator: %@", signalError);
            }];
        };

        // Keep exactly one domain ("iSH-AOK"); remove everything else, including
        // the legacy one-domain-per-root entries from older builds.
        NSFileProviderDomain *ourDomain = nil;
        for (NSFileProviderDomain *domain in domains) {
            if (ourDomain == nil && [domain.identifier isEqualToString:kISHFileProviderDomainIdentifier]) {
                ourDomain = domain;
                continue;
            }
            [NSFileManager.defaultManager removeItemAtURL:
             [NSFileProviderManager.defaultManager.documentStorageURL
              URLByAppendingPathComponent:domain.pathRelativeToDocumentStorage]
                                                    error:nil];
            [NSFileProviderManager removeDomain:domain completionHandler:onError];
        }

        if (ourDomain == nil) {
            NSFileProviderDomain *domain = [[NSFileProviderDomain alloc]
                       initWithIdentifier:kISHFileProviderDomainIdentifier
                              displayName:kISHFileProviderDomainIdentifier
            pathRelativeToDocumentStorage:kISHFileProviderDomainIdentifier];
            [NSFileProviderManager addDomain:domain completionHandler:^(NSError *addError) {
                onError(addError);
                if (addError == nil)
                    signalRoots(domain);
            }];
        } else {
            signalRoots(ourDomain);
        }

        BOOL shouldResync = NO;
        @synchronized (self) {
            shouldResync = self.domainsNeedUpdate ||
                self.fileProviderDomainSyncGeneration != requestedGeneration;
            self.updatingDomains = NO;
            self.domainsNeedUpdate = NO;
        }
        if (shouldResync)
            [self requestFileProviderDomainSync];
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
        // Use '_' (not a space) so the deduped name also passes RootNameIsValid.
        importName = [NSString stringWithFormat:@"%@_%u", baseName, suffix++];
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
    if (!RootNameIsValid(name, error))
        return NO;
    NSAssert(![self.roots containsObject:name], @"root already exists: %@", name);
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    NSURL *temporaryDirectory = NSFileManager.defaultManager.temporaryDirectory;
    NSURL *tempDestination = [temporaryDirectory URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]];
    if (tempDestination == nil)
        return NO;
    // libarchive (used by the preflight below and by fakefs_import) needs a UTF-8
    // LC_CTYPE to read Debian/Devuan tarballs whose entries carry UTF-8 link paths.
    fakefs_ensure_utf8_locale();
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
    if (!RootNameIsValid(newName, error))
        return NO;
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
