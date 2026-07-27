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
#include "kernel/calls.h"
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
        NSURL *container = ContainerURL();
        if (container == nil) {
            NSLog(@"RootsDir: no app group container available (missing or misconfigured App Group entitlement?)");
            return;
        }
        rootsDir = [container URLByAppendingPathComponent:@"roots"];
        NSFileManager *manager = [NSFileManager defaultManager];
        NSError *error = nil;
        if (![manager createDirectoryAtURL:rootsDir
                withIntermediateDirectories:YES
                                 attributes:@{}
                                      error:&error]) {
            NSLog(@"RootsDir: couldn't create roots directory: %@", error);
        }
    });
    return rootsDir;
}

// /AOK/persist is a single shared real-fs mount -- the AppGroup container's
// AOK/persist directory (see AOKPersistDirectoryURL / the do_mount in
// AppDelegate). It is the same regardless of which root is booted and is NOT
// inside any root's fakefs data dir (that's just an empty mount point), so
// both cachedRootArchiveURLs and the bundled-archive downloader address it
// directly as a host directory.
static NSURL *PersistRootsDir(void) {
    NSURL *container = ContainerURL();
    if (container == nil)
        return nil;
    return [[[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
             URLByAppendingPathComponent:@"persist" isDirectory:YES]
            URLByAppendingPathComponent:@"roots" isDirectory:YES];
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
// Groups arch variants of the same distro together in the picker UI (e.g. the
// four Alpine entries below share family "alpine3233") so RootsTableViewController
// can show one row per distro and offer architecture as a sub-choice, instead of
// one flat row per distro_arch combination.
static NSString *const kBundledRootFamilyKey = @"family";
static NSString *const kBundledRootFamilyDisplayNameKey = @"familyDisplayName";
// "official" (maintained as part of iSH-AOK's regression-tested base images) vs
// "community" (contributed/experimental, no distro-specific patching guarantees).
static NSString *const kBundledRootTierKey = @"tier";
static NSString *const kBundledRootTierOfficial = @"official";
static NSString *const kBundledRootTierCommunity = @"community";
// Present only for choices whose archive isn't shipped in the app bundle --
// importing them downloads this URL into /AOK/persist/roots on demand
// instead (see DownloadBundledArchive / importBundledRootChoice:).
static NSString *const kBundledRootDownloadURLKey = @"downloadURL";
static NSString *const kBundledRootDownloadSizeKey = @"downloadSize";
static NSString *const kRootsErrorDomain = @"iSH.Roots";
static NSString *const kRootMetadataFileName = @"ish-root.plist";
static NSString *const kRootMetadataGuestABIKey = @"guestABI";

NSNotificationName const RootsDidFinishInitialSelectionNotification = @"RootsDidFinishInitialSelectionNotification";

// The download-backed choices (everything with a kBundledRootDownloadURLKey)
// live in the deps/rootfs-manifest submodule (github.com/emkey1/ish-AOK-rootfs)
// as manifest.json, bundled into the app as the "rootfs-manifest" resource --
// see that repo's README for the entry schema and how to contribute a new
// rootfs without touching this file. Required keys are validated here so a
// malformed community PR degrades to "entry silently dropped", never a crash.
static NSArray<NSString *> *RequiredManifestKeys(void) {
    static NSArray<NSString *> *keys;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        keys = @[kBundledRootIdentifierKey, kBundledRootDisplayNameKey, kBundledRootArchiveNameKey,
                 kBundledRootImportNameKey, kBundledRootInitialWindowKey, kBundledRootGuestABIKey,
                 kBundledRootDownloadURLKey, kBundledRootDownloadSizeKey, kBundledRootFamilyKey,
                 kBundledRootFamilyDisplayNameKey];
    });
    return keys;
}

static NSArray<NSDictionary<NSString *, NSString *> *> *LoadDownloadableRootChoicesFromManifest(void) {
    NSURL *manifestURL = [NSBundle.mainBundle URLForResource:@"manifest" withExtension:@"json"];
    if (manifestURL == nil) {
        NSLog(@"manifest.json not found in app bundle (deps/rootfs-manifest submodule) -- no downloadable filesystem choices available");
        return @[];
    }
    NSData *data = [NSData dataWithContentsOfURL:manifestURL];
    NSError *error = nil;
    id parsed = data != nil ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
    if (![parsed isKindOfClass:NSArray.class]) {
        NSLog(@"rootfs-manifest.json could not be parsed: %@", error);
        return @[];
    }

    NSMutableArray<NSDictionary<NSString *, NSString *> *> *entries = [NSMutableArray array];
    NSMutableSet<NSString *> *seenIdentifiers = [NSMutableSet set];
    for (id rawEntry in (NSArray *) parsed) {
        if (![rawEntry isKindOfClass:NSDictionary.class]) {
            NSLog(@"rootfs-manifest.json: skipping non-object entry");
            continue;
        }
        NSDictionary<NSString *, NSString *> *entry = rawEntry;
        BOOL valid = YES;
        for (NSString *key in RequiredManifestKeys()) {
            id value = entry[key];
            if (![value isKindOfClass:NSString.class] || [(NSString *) value length] == 0) {
                NSLog(@"rootfs-manifest.json: entry missing required key '%@', skipping", key);
                valid = NO;
                break;
            }
        }
        if (!valid)
            continue;
        NSString *identifier = entry[kBundledRootIdentifierKey];
        if ([seenIdentifiers containsObject:identifier]) {
            NSLog(@"rootfs-manifest.json: duplicate identifier '%@', skipping", identifier);
            continue;
        }
        [seenIdentifiers addObject:identifier];

        NSString *tier = entry[kBundledRootTierKey];
        if (![tier isEqualToString:kBundledRootTierOfficial] && ![tier isEqualToString:kBundledRootTierCommunity]) {
            NSMutableDictionary<NSString *, NSString *> *fixedUp = [entry mutableCopy];
            fixedUp[kBundledRootTierKey] = kBundledRootTierCommunity;
            entry = fixedUp;
        }
        [entries addObject:entry];
    }
    return entries;
}

static NSArray<NSDictionary<NSString *, NSString *> *> *BundledRootChoices(void) {
    static NSArray<NSDictionary<NSString *, NSString *> *> *choices;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        NSMutableArray<NSDictionary<NSString *, NSString *> *> *mutableChoices = [@[
            // Bundled-in-the-app choices (arm64 guests -- the only architecture
            // still shipped in the IPA); every download-backed choice comes from
            // the rootfs-manifest submodule instead (loaded below).
            // RootsTableViewController groups these by kBundledRootFamilyKey
            // (one row per distro, architecture picked as a sub-choice) and
            // splits into "Official Distributions" / "Community Distributions"
            // table sections along kBundledRootTierKey.
            @{
                kBundledRootIdentifierKey: @"alpine3233arm64",
                kBundledRootDisplayNameKey: @"Alpine3.23.3(arm64)",
                kBundledRootArchiveNameKey: @"alpine-minirootfs-3.23.3-aarch64",
                // Native AArch64 guest (same-architecture dispatch on Apple
                // silicon — see aarch64_guest_plan.md). Import name follows
                // the RootNameIsValid rules like the x86_64 entry above.
                kBundledRootImportNameKey: @"Alpine3.23.3-arm64",
                kBundledRootInitialWindowKey: @"session-shell",
                kBundledRootGuestABIKey: @"arm64",
                kBundledRootFamilyKey: @"alpine3233",
                kBundledRootFamilyDisplayNameKey: @"Alpine 3.23.3",
                kBundledRootTierKey: kBundledRootTierOfficial,
            },
            @{
                kBundledRootIdentifierKey: @"devuan6arm64",
                kBundledRootDisplayNameKey: @"Devuan 6 (excalibur, arm64)",
                kBundledRootArchiveNameKey: @"devuan-minirootfs-6.0-aarch64",
                kBundledRootImportNameKey: @"Devuan6-arm64",
                kBundledRootInitialWindowKey: @"session-shell",
                kBundledRootGuestABIKey: @"arm64",
                kBundledRootFamilyKey: @"devuan6",
                kBundledRootFamilyDisplayNameKey: @"Devuan 6 (excalibur)",
                kBundledRootTierKey: kBundledRootTierOfficial,
            },
        ] mutableCopy];
        [mutableChoices addObjectsFromArray:LoadDownloadableRootChoicesFromManifest()];
        choices = mutableChoices;
    });
    return choices;
}

// Bundled root archives may ship in any container format libarchive (and thus
// fakefs_import) can read -- gzip, xz, zstd, bzip2, ... -- so resolve the
// bundled resource by trying each supported extension instead of assuming
// .tar.gz. (Keep the extension list in sync with the cached-archive suffixes in
// cachedRootArchiveURLs and the filters in fakefs_import / tools/fakefs.c.)
static NSURL *BundledRootArchiveURL(NSString *archiveName) {
    if (archiveName.length == 0)
        return nil;
    static NSArray<NSString *> *extensions;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        extensions = @[@"tar.xz", @"tar.zst", @"tar.gz", @"tar.bz2",
                       @"tar.lz", @"tar.lzma", @"txz", @"tzst", @"tgz", @"tar"];
    });
    for (NSString *ext in extensions) {
        NSURL *url = [NSBundle.mainBundle URLForResource:archiveName withExtension:ext];
        if (url != nil)
            return url;
    }
    return nil;
}

// Where a download-backed bundled choice's archive lands once fetched --
// alongside (and indistinguishable from, once present) any archive a user
// drops into /AOK/persist/roots themselves.
static NSURL *DownloadedBundledArchiveURL(NSDictionary<NSString *, NSString *> *choice) {
    NSString *archiveName = choice[kBundledRootArchiveNameKey];
    NSURL *dir = PersistRootsDir();
    if (archiveName.length == 0 || dir == nil)
        return nil;
    return [dir URLByAppendingPathComponent:[archiveName stringByAppendingString:@".tar.xz"]];
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

// Drives one bundled-archive download for DownloadBundledArchive(). Progress
// and cancellation are reported through the same id<ProgressReporter> that
// fakefs_import's root_progress_callback (below) reports import progress
// through, so the caller sees one continuous "Downloading..."/"Extracting..."
// progress sheet.
@interface RootsArchiveDownload : NSObject <NSURLSessionDownloadDelegate>
@property (nonatomic, weak) id<ProgressReporter> progress;
@property (nonatomic) NSURL *destinationURL;
@property (nonatomic) NSError *error;
@property (nonatomic) BOOL cancelled;
@property (nonatomic) dispatch_semaphore_t semaphore;
@end

@implementation RootsArchiveDownload

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    double fraction = totalBytesExpectedToWrite > 0
        ? (double) totalBytesWritten / (double) totalBytesExpectedToWrite : 0;
    NSString *message = totalBytesExpectedToWrite > 0
        ? [NSString stringWithFormat:@"Downloading… %@ of %@",
           FormatByteCount(totalBytesWritten), FormatByteCount(totalBytesExpectedToWrite)]
        : [NSString stringWithFormat:@"Downloading… %@", FormatByteCount(totalBytesWritten)];
    [self.progress updateProgress:fraction message:message];
    if ([self.progress shouldCancel]) {
        self.cancelled = YES;
        [downloadTask cancel];
    }
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location {
    NSHTTPURLResponse *response = (NSHTTPURLResponse *) downloadTask.response;
    if ([response isKindOfClass:NSHTTPURLResponse.class] && response.statusCode != 200) {
        self.error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadServerResponse userInfo:@{
            NSLocalizedDescriptionKey: [NSString stringWithFormat:@"Server returned status %ld", (long) response.statusCode],
        }];
        return;
    }
    [NSFileManager.defaultManager removeItemAtURL:self.destinationURL error:nil];
    NSError *moveError = nil;
    if (![NSFileManager.defaultManager moveItemAtURL:location toURL:self.destinationURL error:&moveError])
        self.error = moveError;
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
    if (error != nil && self.error == nil && !self.cancelled)
        self.error = error;
    dispatch_semaphore_signal(self.semaphore);
}

@end

// Synchronously downloads a bundled root's archive into /AOK/persist/roots.
// Runs on a background queue already (see importBundledRootChoice: callers),
// so blocking on a semaphore here is fine -- mirrors the rest of this file's
// synchronous archive-import style rather than threading async completion
// handlers through Roots' public API.
static BOOL DownloadBundledArchive(NSURL *url, NSURL *destination, id<ProgressReporter> progress, NSError **error) {
    NSError *dirError = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:destination.URLByDeletingLastPathComponent
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&dirError]) {
        if (error != NULL)
            *error = dirError;
        return NO;
    }

    RootsArchiveDownload *delegate = [RootsArchiveDownload new];
    delegate.progress = progress;
    delegate.destinationURL = destination;
    delegate.semaphore = dispatch_semaphore_create(0);

    NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:nil];
    NSURLSessionDownloadTask *task = [session downloadTaskWithURL:url];
    [progress updateProgress:0 message:@"Downloading…"];
    [task resume];
    dispatch_semaphore_wait(delegate.semaphore, DISPATCH_TIME_FOREVER);
    [session finishTasksAndInvalidate];

    if (delegate.cancelled) {
        if (error != NULL)
            *error = nil;
        return NO;
    }
    if (delegate.error != nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"Couldn't download the filesystem image.",
                NSLocalizedRecoverySuggestionErrorKey: @"Check your internet connection and try again.",
                NSUnderlyingErrorKey: delegate.error,
            }];
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

// Finds the bundled/manifest choice a root's name was imported from, matching
// by prefix to account for the "_2", "_3", ... dedup suffix
// importBundledRootChoice: appends when a name collides. Returns nil for
// roots that didn't come from a bundled choice (e.g. a user's own import).
static NSDictionary<NSString *, NSString *> *BundledRootChoiceMatchingName(NSString *name) {
    for (NSDictionary<NSString *, NSString *> *choice in BundledRootChoices()) {
        NSString *baseName = choice[kBundledRootImportNameKey];
        if (baseName.length == 0)
            continue;
        if ([name isEqualToString:baseName] ||
                [name hasPrefix:[baseName stringByAppendingString:@"_"]]) {
            return choice;
        }
    }
    return nil;
}

// Prefers a root whose bundled-choice origin isn't the experimental
// "community" tier (or a root with no bundled-choice match at all -- e.g. a
// user's own import) over one that is, when picking a fallback default with
// no valid stored preference to honor. contentsOfDirectoryAtPath: order is
// otherwise arbitrary (see -init), so without this an experimental
// community-tier root (e.g. a downloaded Arch Linux choice) installed
// alongside the official Alpine/Devuan roots could silently become the
// default with no user prompt.
static NSString *PreferredDefaultRootName(NSOrderedSet<NSString *> *roots) {
    for (NSString *name in roots) {
        NSDictionary<NSString *, NSString *> *choice = BundledRootChoiceMatchingName(name);
        if (![choice[kBundledRootTierKey] isEqualToString:kBundledRootTierCommunity])
            return name;
    }
    return roots.firstObject;
}

@implementation Roots

- (instancetype)init {
    if (self = [super init]) {
        EnableCaseSensitiveFilesystemLookupsIfPossible();
        NSMutableOrderedSet<NSString *> *roots = [NSMutableOrderedSet orderedSet];
        NSURL *rootsDir = RootsDir();
        if (rootsDir == nil) {
            NSLog(@"Roots: roots directory unavailable, starting with no roots");
        } else {
            NSError *error = nil;
            NSArray<NSString *> *rootNames = [NSFileManager.defaultManager contentsOfDirectoryAtPath:rootsDir.path error:&error];
            if (error != nil) {
                NSLog(@"Roots: couldn't list roots: %@", error);
            }
            for (NSString *rootName in rootNames) {
                if (RootURLLooksValid([self rootUrl:rootName])) {
                    [roots addObject:rootName];
                } else {
                    NSLog(@"ignoring invalid root entry %@", rootName);
                }
            }
        }
        self.roots = roots;
        self.fileProviderDomainSyncEnabled = NO;
        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = PreferredDefaultRootName(self.roots);
            [self requestFileProviderDomainSync];
        }];
        [self requestFileProviderDomainSync];

        if ((!self.defaultRoot || ![self.roots containsObject:self.defaultRoot]) && self.roots.count)
            self.defaultRoot = PreferredDefaultRootName(self.roots);
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

- (BOOL)bundledRootChoiceNeedsDownload:(NSDictionary<NSString *, NSString *> *)choice {
    if (choice[kBundledRootDownloadURLKey].length == 0)
        return NO;
    if (BundledRootArchiveURL(choice[kBundledRootArchiveNameKey]) != nil)
        return NO;
    // importBundledRootChoice: always fetches fresh for a download-backed
    // choice now (see its comment), so this is "will download", full stop --
    // not "download only if not already cached".
    return YES;
}

- (NSURL *)rootUrl:(NSString *)name {
    return [RootsDir() URLByAppendingPathComponent:name];
}

- (nullable NSString *)guestABIForRootNamed:(NSString *)name {
    NSDictionary<NSString *, id> *metadata = ReadRootMetadata([self rootUrl:name]);
    NSString *guestABI = metadata[kRootMetadataGuestABIKey];
    if ([guestABI isKindOfClass:NSString.class] && guestABI.length != 0)
        return guestABI;

    NSString *choiceGuestABI = BundledRootChoiceMatchingName(name)[kBundledRootGuestABIKey];
    return choiceGuestABI.length != 0 ? choiceGuestABI : nil;
}

- (NSArray<NSURL *> *)cachedRootArchiveURLs {
    NSURL *cacheDir = PersistRootsDir();
    if (cacheDir == nil)
        return @[];
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

    NSURL *archive = BundledRootArchiveURL(selectedChoice[kBundledRootArchiveNameKey]);
    if (archive == nil) {
        NSString *downloadURLString = selectedChoice[kBundledRootDownloadURLKey];
        NSURL *downloadDestination = DownloadedBundledArchiveURL(selectedChoice);
        if (downloadURLString.length != 0 && downloadDestination != nil) {
            // Always fetch fresh from the network here -- this is the explicit
            // "remote" choice (Official/Community Distributions), distinct from
            // the "Root Cached Filesystems" section that imports an on-disk
            // archive directly. Silently reusing a stale cached download from a
            // previous import made "remote" indistinguishable from "cached",
            // with no way to force a fresh fetch (e.g. to pick up an updated
            // archive, or replace a corrupted one) short of manually deleting
            // the file first. DownloadBundledArchive already removes any
            // existing file at the destination before moving the new one in.
            NSURL *downloadURL = [NSURL URLWithString:downloadURLString];
            if (downloadURL != nil &&
                DownloadBundledArchive(downloadURL, downloadDestination, progress, error)) {
                archive = downloadDestination;
            } else {
                return NO;
            }
        }
    }
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
    // RootsDir() returns nil when the app group container is unavailable (a
    // missing or misconfigured App Group entitlement -- notably a build made
    // with CODE_SIGNING_ALLOWED=NO, which strips entitlements entirely). It
    // logs and returns gracefully, but a nil destination then flows into
    // -moveItemAtURL:toURL:options:error: below, which raises
    // NSInvalidArgumentException and aborts the whole app instead of surfacing
    // the "Import failed" alert this method's error return already drives.
    if (destination == nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"No filesystem storage available "
                    @"(the app group container is missing -- check the App Group entitlement)"
            }];
        }
        return NO;
    }
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

// Every root other than the booted one is auto-exposed read-write at
// /AOK/roots/<name> (see AppDelegate's boot), and opt/AOK/tools/mount-root.sh
// can additionally bind /proc, /sys, /dev, /dev/pts, /run and /AOK/tools into
// it for a working chroot. Deleting or renaming that root's backing
// directory out from under a still-mounted (or actively chrooted-into) fake
// filesystem would corrupt or orphan it, so tear those mounts down first.
//
// Returns NO with *error set only if the root is genuinely busy (something
// still has an open fd/cwd/root inside it, e.g. an active chroot session --
// do_umount reports this as EBUSY). Any other do_umount outcome (most
// commonly EINVAL: this root was never touched via mount-root.sh, so there's
// nothing mounted at all) is harmless and silently ignored -- do_umount is
// safe to call unconditionally on a path that isn't currently mounted.
- (BOOL)unmountExposedRootNamed:(NSString *)name error:(NSError **)error {
#if ISH_LINUX
    NSString *base = [@"/AOK/roots/" stringByAppendingString:name];
    NSArray<NSString *> *binds = @[@"AOK/tools", @"run", @"dev/pts", @"dev", @"sys", @"proc"];
    for (NSString *bind in binds)
        do_umount([base stringByAppendingPathComponent:bind].UTF8String);

    if (do_umount(base.UTF8String) == _EBUSY) {
        if (error != nil) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey:
                @"This filesystem is currently mounted or in use (e.g. via mount-root.sh) -- exit any active session into it first"}];
        }
        return NO;
    }
#endif
    return YES;
}

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot delete the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    if (![self unmountExposedRootNamed:name error:error])
        return NO;
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
    if (![self unmountExposedRootNamed:name error:error])
        return NO;
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
