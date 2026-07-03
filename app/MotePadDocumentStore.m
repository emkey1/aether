//
//  MotePadDocumentStore.m
//  iSH-AOK
//

#import "MotePadDocumentStore.h"
#import "AppGroup.h"

#include <sys/stat.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/stat.h"
#include "fs/path.h"
#include "util/sync.h"
#include "debug.h"

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif

static NSString *const kPersistGuestPrefix = @"/AOK/persist/";

@implementation MotePadDirectoryEntry
@end

@implementation MotePadDocumentStore

+ (instancetype)sharedStore {
    static MotePadDocumentStore *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[MotePadDocumentStore alloc] init]; });
    return shared;
}

#pragma mark Host paths (realfs /AOK/persist fast path)

- (NSURL *)persistHostBaseURL {
    NSURL *container = ContainerURL();
    if (container == nil) return nil;
    return [[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

// Host URL for a path under the realfs /AOK/persist mount, or nil if not under it.
// /AOK/persist is a real host directory, so anything under it can be reached with
// NSFileManager from any thread — no guest VFS, no borrowed task context needed.
- (NSURL *)hostURLForPersistGuestPath:(NSString *)guestPath {
    BOOL isPersistRoot = [guestPath isEqualToString:@"/AOK/persist"] || [guestPath isEqualToString:@"/AOK/persist/"];
    if (!isPersistRoot && ![guestPath hasPrefix:kPersistGuestPrefix])
        return nil;
    NSURL *base = [self persistHostBaseURL];
    if (base == nil)
        return nil;
    NSString *rel = isPersistRoot ? @"" : [guestPath substringFromIndex:kPersistGuestPrefix.length];
    NSURL *hostURL = base;
    for (NSString *component in rel.pathComponents) {
        if (component.length == 0 || [component isEqualToString:@"/"])
            continue;
        hostURL = [hostURL URLByAppendingPathComponent:component];
    }
    return hostURL;
}

#pragma mark Borrowed task context

// Run a guest-VFS operation under a borrowed task context. fs/path.c resolves
// paths through the thread-local `current` task, which is unset on GCD/UI threads,
// so we borrow pid 1 exactly as the app's boot code does (PushInitTaskAsCurrent)
// and restore afterward. Returns NO if init isn't usable yet.
- (BOOL)withGuestTaskContext:(void (^)(void))block {
    struct task *previous = current;
    struct task *init = pid_get_task_ref(1);
    if (init != NULL) {
        bool usable;
        lock(&init->general_lock, 0);
        usable = init->mm != NULL && init->mem != NULL && init->files != NULL && init->fs != NULL;
        unlock(&init->general_lock);
        if (!usable) { task_ref_cnt_mod(init, -1); init = NULL; }
    }
    if (init == NULL)
        return NO;
    current = init;
    block();
    current = previous;
    task_ref_cnt_mod(init, -1);
    return YES;
}

#pragma mark Directory listing

- (nullable NSArray<MotePadDirectoryEntry *> *)listDirectoryAtGuestPath:(NSString *)guestPath {
    guestPath = [self normalizedGuestPath:guestPath];

    NSURL *hostDir = [self hostURLForPersistGuestPath:guestPath];
    BOOL hostIsDir = NO;
    if (hostDir != nil && [NSFileManager.defaultManager fileExistsAtPath:hostDir.path isDirectory:&hostIsDir] && hostIsDir)
        return [self listHostDirectory:hostDir guestBasePath:guestPath];

    __block NSArray<MotePadDirectoryEntry *> *result = nil;
    [self withGuestTaskContext:^{
        result = [self listGuestDirectoryViaVFS:guestPath];
    }];
    return result;
}

- (NSArray<MotePadDirectoryEntry *> *)listHostDirectory:(NSURL *)hostDir guestBasePath:(NSString *)guestBasePath {
    NSArray<NSURL *> *contents =
        [NSFileManager.defaultManager contentsOfDirectoryAtURL:hostDir
                                    includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                         error:NULL];
    NSMutableArray<MotePadDirectoryEntry *> *entries = [NSMutableArray array];
    for (NSURL *fileURL in contents) {
        NSNumber *isDir = nil;
        [fileURL getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:NULL];
        MotePadDirectoryEntry *entry = [MotePadDirectoryEntry new];
        entry.name = fileURL.lastPathComponent;
        entry.guestPath = [guestBasePath stringByAppendingPathComponent:entry.name];
        entry.type = isDir.boolValue ? MotePadEntryTypeDirectory : MotePadEntryTypeFile;
        [entries addObject:entry];
    }
    return [self sortedEntries:entries];
}

// Caller must already hold a borrowed task context.
- (NSArray<MotePadDirectoryEntry *> *)listGuestDirectoryViaVFS:(NSString *)guestPath {
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return nil;
    if (!S_ISDIR(fd->type) || fd->ops->readdir == NULL) { fd_close(fd); return nil; }

    NSMutableArray<MotePadDirectoryEntry *> *entries = [NSMutableArray array];
    if (fd->ops->readdir_begin) fd->ops->readdir_begin(fd);
    while (true) {
        struct dir_entry raw;
        memset(&raw, 0, sizeof(raw));
        raw.type = DT_UNKNOWN;
        int err = fd->ops->readdir(fd, &raw);
        if (err <= 0) break;  // 0 = end, <0 = error
        NSString *name = [NSString stringWithUTF8String:raw.name];
        if (name == nil || [name isEqualToString:@"."] || [name isEqualToString:@".."])
            continue;
        NSString *childPath = [guestPath stringByAppendingPathComponent:name];

        int type = raw.type;
        if (type == DT_UNKNOWN) {
            struct statbuf st;
            memset(&st, 0, sizeof(st));
            if (generic_statat(AT_PWD, childPath.fileSystemRepresentation, &st, 0) >= 0)
                type = S_ISDIR(st.mode) ? DT_DIR : DT_REG;
        }

        MotePadDirectoryEntry *entry = [MotePadDirectoryEntry new];
        entry.name = name;
        entry.guestPath = childPath;
        entry.type = (type == DT_DIR) ? MotePadEntryTypeDirectory
                   : (type == DT_REG) ? MotePadEntryTypeFile
                   : MotePadEntryTypeOther;
        [entries addObject:entry];
    }
    if (fd->ops->readdir_end) fd->ops->readdir_end(fd);
    fd_close(fd);
    return [self sortedEntries:entries];
}

- (NSArray<MotePadDirectoryEntry *> *)sortedEntries:(NSArray<MotePadDirectoryEntry *> *)entries {
    return [entries sortedArrayUsingComparator:^NSComparisonResult(MotePadDirectoryEntry *a, MotePadDirectoryEntry *b) {
        BOOL aDir = a.type == MotePadEntryTypeDirectory;
        BOOL bDir = b.type == MotePadEntryTypeDirectory;
        if (aDir != bDir)
            return aDir ? NSOrderedAscending : NSOrderedDescending;  // directories first
        return [a.name caseInsensitiveCompare:b.name];
    }];
}

#pragma mark Reading

- (nullable NSString *)readTextFileAtGuestPath:(NSString *)guestPath error:(NSError **)error {
    guestPath = [self normalizedGuestPath:guestPath];

    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil && [NSFileManager.defaultManager fileExistsAtPath:hostURL.path]) {
        NSData *data = [NSData dataWithContentsOfURL:hostURL options:0 error:error];
        if (data == nil) return nil;
        return [self stringFromData:data];
    }

    __block NSData *data = nil;
    __block NSError *localError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        data = [self readGuestFileViaVFS:guestPath error:&localError];
    }];
    if (!hadContext) {
        if (error) *error = [self errorWithCode:0 message:@"Guest filesystem not ready"];
        return nil;
    }
    if (data == nil) {
        if (error) *error = localError;
        return nil;
    }
    return [self stringFromData:data];
}

// Caller must already hold a borrowed task context.
- (NSData *)readGuestFileViaVFS:(NSString *)guestPath error:(NSError **)error {
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_, 0);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithCode:(int)PTR_ERR(fd) message:@"Cannot open file"];
        return nil;
    }
    if (S_ISDIR(fd->type)) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:0 message:@"That path is a directory"];
        return nil;
    }
    NSMutableData *data = [NSMutableData data];
    char buffer[65536];
    ssize_t n;
    while ((n = fd->ops->read(fd, buffer, sizeof(buffer))) > 0)
        [data appendBytes:buffer length:(NSUInteger)n];
    fd_close(fd);
    if (n < 0) {
        if (error) *error = [self errorWithCode:(int)n message:@"Failed reading file"];
        return nil;
    }
    return data;
}

- (NSString *)stringFromData:(NSData *)data {
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (text == nil)  // non-UTF-8: degrade gracefully rather than refuse to open
        text = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
    return text ?: @"";
}

#pragma mark Writing

- (BOOL)writeText:(NSString *)text toGuestPath:(NSString *)guestPath error:(NSError **)error {
    guestPath = [self normalizedGuestPath:guestPath];
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];

    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil) {
        [NSFileManager.defaultManager createDirectoryAtURL:hostURL.URLByDeletingLastPathComponent
                               withIntermediateDirectories:YES attributes:nil error:NULL];
        return [data writeToURL:hostURL options:NSDataWritingAtomic error:error];
    }

    __block BOOL ok = NO;
    __block NSError *localError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        ok = [self writeData:data toGuestPathViaVFS:guestPath error:&localError];
    }];
    if (!hadContext) {
        if (error) *error = [self errorWithCode:0 message:@"Guest filesystem not ready"];
        return NO;
    }
    if (!ok && error) *error = localError;
    return ok;
}

// Caller must already hold a borrowed task context.
- (BOOL)writeData:(NSData *)data toGuestPathViaVFS:(NSString *)guestPath error:(NSError **)error {
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithCode:(int)PTR_ERR(fd) message:@"Cannot create file"];
        return NO;
    }
    const char *bytes = data.bytes;
    size_t remaining = data.length;
    BOOL ok = YES;
    while (remaining > 0) {
        ssize_t n = fd->ops->write(fd, bytes, remaining);
        if (n <= 0) { ok = NO; break; }
        bytes += n;
        remaining -= (size_t)n;
    }
    fd_close(fd);
    if (!ok && error)
        *error = [self errorWithCode:0 message:@"Failed writing file"];
    return ok;
}

#pragma mark Existence / defaults

- (BOOL)fileExistsAtGuestPath:(NSString *)guestPath isDirectory:(BOOL *)isDirectory {
    guestPath = [self normalizedGuestPath:guestPath];

    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil) {
        BOOL dir = NO;
        BOOL exists = [NSFileManager.defaultManager fileExistsAtPath:hostURL.path isDirectory:&dir];
        if (isDirectory) *isDirectory = dir;
        return exists;
    }

    __block BOOL exists = NO;
    __block BOOL dir = NO;
    [self withGuestTaskContext:^{
        struct statbuf st;
        memset(&st, 0, sizeof(st));
        if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &st, 0) >= 0) {
            exists = YES;
            dir = S_ISDIR(st.mode);
        }
    }];
    if (isDirectory) *isDirectory = dir;
    return exists;
}

- (NSString *)defaultDirectoryGuestPath {
    BOOL isDir = NO;
    if ([self fileExistsAtGuestPath:@"/AOK/persist" isDirectory:&isDir] && isDir)
        return @"/AOK/persist";
    return @"/";
}

#pragma mark Helpers

- (NSString *)normalizedGuestPath:(NSString *)guestPath {
    NSString *path = guestPath ?: @"/";
    if (![path hasPrefix:@"/"])
        path = [@"/" stringByAppendingString:path];
    // Collapse a trailing slash (except the root itself) so path arithmetic is stable.
    while (path.length > 1 && [path hasSuffix:@"/"])
        path = [path substringToIndex:path.length - 1];
    return path;
}

- (NSError *)errorWithCode:(int)code message:(NSString *)message {
    return [NSError errorWithDomain:@"MotePadDocumentStore" code:code
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}

@end
