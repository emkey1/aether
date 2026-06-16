//
//  FileProviderItem.m
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import <MobileCoreServices/MobileCoreServices.h>
#include <sys/stat.h>
#include <dirent.h>
#import "../AppGroup.h"
#import "FileProviderExtension.h"
#import "FileProviderItem.h"
#include "fs/fake-db.h"
#include "kernel/errno.h"

static NSString *const ISHFileProviderVirtualIdentifierPrefix = @"virt_";
// Separates the root name from the per-root inner identifier in a fully scoped
// item identifier. '+' can't appear in a root name (RootNameIsValid restricts
// to [A-Za-z0-9._-]), in a decimal inode, or in a base64url "virt_" payload, so
// splitting on the first '+' is unambiguous and the whole identifier stays a
// single path component (important for the on-disk materialization storage and
// persistentIdentifierForItemAtURL round-trip).
static NSString *const ISHFileProviderRootIdentifierSeparator = @"+";

static NSString *ISHBase64URLEncode(NSData *data) {
    NSString *encoded = [data base64EncodedStringWithOptions:0];
    encoded = [encoded stringByReplacingOccurrencesOfString:@"+" withString:@"-"];
    encoded = [encoded stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
    return [encoded stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"="]];
}

static NSData *ISHBase64URLDecode(NSString *encoded) {
    NSMutableString *base64 = [[encoded stringByReplacingOccurrencesOfString:@"-" withString:@"+"] mutableCopy];
    [base64 replaceOccurrencesOfString:@"_" withString:@"/" options:0 range:NSMakeRange(0, base64.length)];
    while ((base64.length % 4) != 0)
        [base64 appendString:@"="];
    return [[NSData alloc] initWithBase64EncodedString:base64 options:0];
}

NSString *ISHFileProviderVirtualIdentifierForPath(NSString *path) {
    NSData *data = [path dataUsingEncoding:NSUTF8StringEncoding];
    if (data == nil)
        return nil;
    return [ISHFileProviderVirtualIdentifierPrefix stringByAppendingString:ISHBase64URLEncode(data)];
}

BOOL ISHFileProviderIsVirtualIdentifier(NSString *identifier) {
    return [identifier hasPrefix:ISHFileProviderVirtualIdentifierPrefix];
}

NSString *ISHFileProviderPathForIdentifier(NSString *identifier) {
    if (!ISHFileProviderIsVirtualIdentifier(identifier))
        return nil;
    NSString *payload = [identifier substringFromIndex:ISHFileProviderVirtualIdentifierPrefix.length];
    NSData *data = ISHBase64URLDecode(payload);
    if (data == nil)
        return nil;
    NSString *path = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (path.length == 0)
        return nil;
    return path;
}

NSString *ISHFileProviderComposeIdentifier(NSString *rootName, NSFileProviderItemIdentifier innerIdentifier) {
    if (innerIdentifier == nil || [innerIdentifier isEqualToString:NSFileProviderRootContainerItemIdentifier])
        return rootName; // the root folder itself (that root's "/")
    return [NSString stringWithFormat:@"%@%@%@", rootName, ISHFileProviderRootIdentifierSeparator, innerIdentifier];
}

NSString *ISHFileProviderRootNameForIdentifier(NSFileProviderItemIdentifier identifier) {
    if (identifier == nil || [identifier isEqualToString:NSFileProviderRootContainerItemIdentifier])
        return nil; // the domain root container, which belongs to no single root
    NSRange range = [identifier rangeOfString:ISHFileProviderRootIdentifierSeparator];
    if (range.location == NSNotFound)
        return identifier; // a bare root name -> that root's folder
    return [identifier substringToIndex:range.location];
}

NSFileProviderItemIdentifier ISHFileProviderInnerIdentifier(NSFileProviderItemIdentifier identifier) {
    if (identifier == nil || [identifier isEqualToString:NSFileProviderRootContainerItemIdentifier])
        return NSFileProviderRootContainerItemIdentifier;
    NSRange range = [identifier rangeOfString:ISHFileProviderRootIdentifierSeparator];
    if (range.location == NSNotFound)
        return NSFileProviderRootContainerItemIdentifier; // root folder -> that root's "/"
    return [identifier substringFromIndex:range.location + range.length];
}

@interface FileProviderItem ()

// The per-root inner identifier: a decimal inode, a "virt_" identifier, or
// NSFileProviderRootContainerItemIdentifier for a root's "/". The fully scoped
// identifier exposed to the system is derived from this plus the mount's root.
@property (readonly) NSFileProviderItemIdentifier identifier;
@property (readonly) int fd;
@property (readonly) BOOL isRoot;

@end

@implementation FileProviderItem

- (instancetype)initWithIdentifier:(NSFileProviderItemIdentifier)identifier mountOwner:(ISHFileProviderMount * _Nullable)mountOwner error:(NSError *__autoreleasing  _Nullable *)error {
    if (self = [super init]) {
        _identifier = identifier;
        _mountOwner = mountOwner;
        if (mountOwner == nil) {
            // Domain root: the synthetic folder that lists the installed roots.
            // It has no backing fakefs mount or file descriptor.
            _fd = -1;
            return self;
        }
        _fd = [self openNewFDWithError:error];
        if (_fd == -1)
            return nil;
    }
    return self;
}

- (struct fakefs_mount *)mount {
    return self.mountOwner.mount;
}

// The synthetic top-level folder that enumerates the installed roots.
- (BOOL)isDomainRoot {
    return self.mountOwner == nil;
}

- (BOOL)isRoot {
    return [self.identifier isEqualToString:NSFileProviderRootContainerItemIdentifier];
}

- (NSString *)rootName {
    return self.mountOwner.rootName;
}

- (BOOL)isVirtualItem {
    return ISHFileProviderIsVirtualIdentifier(self.identifier);
}

- (NSString *)virtualPath {
    return ISHFileProviderPathForIdentifier(self.identifier);
}

- (int)openNewFDWithError:(NSError *__autoreleasing  _Nullable *)error {
    int fd = -1;
    if (self.isRoot) {
        fd = open(self.mount->source, O_DIRECTORY | O_RDONLY);
    } else if (self.isVirtualItem) {
        NSString *path = self.virtualPath;
        if (path != nil)
            fd = openat(self.mount->root_fd, fix_path(path.fileSystemRepresentation), O_RDONLY);
    } else {
        db_begin_read(&self.mount->db);
        sqlite3_stmt *stmt = self.mount->db.stmt.path_from_inode;
        sqlite3_bind_int64(self.mount->db.stmt.path_from_inode, 1, _identifier.longLongValue);
        while (db_exec(&self.mount->db, stmt)) {
            const char *path = (const char *) sqlite3_column_text(stmt, 0);
            fd = openat(self.mount->root_fd, fix_path(path), O_RDWR);
            if (fd == -1 && errno == EISDIR)
                fd = openat(self.mount->root_fd, fix_path(path), O_RDONLY);
            if (fd == -1 && errno != ENOENT)
                break;
        }
        db_reset(&self.mount->db, stmt);
        db_commit(&self.mount->db);
    }
    if (fd == -1) {
        NSError *openError = nil;
        if (error != nil) {
            if (errno == ENOENT)
                openError = [NSError fileProviderErrorForNonExistentItemWithIdentifier:self.itemIdentifier];
            else
                openError = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
            *error = openError;
        }
        NSLog(@"opening %@ failed: %@", self.itemIdentifier, openError);
        return -1;
    }
    return fd;
}

- (NSString *)path {
    if (self.isDomainRoot)
        return @"";
    if (self.isVirtualItem)
        return self.virtualPath;
    char path[PATH_MAX] = "";
    int err = fcntl(_fd, F_GETPATH, path);
    [self handleError:err inFunction:@"getpath"];
    const char *myPath = path + strlen(self.mount->source);
    return [NSFileManager.defaultManager stringWithFileSystemRepresentation:myPath length:strlen(myPath)];
}

- (NSURL *)URL {
    if (self.isDomainRoot)
        return nil;
    NSURL *rootURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:self.mount->source]];
    if (self.isRoot)
        return rootURL;
    return [rootURL URLByAppendingPathComponent:self.path];
}

- (struct ish_stat)ishStat {
    struct ish_stat stat = {};
    if (self.isDomainRoot)
        return stat;
    if (self.isVirtualItem) {
        struct stat real = self.realStat;
        stat.mode = real.st_mode;
        stat.uid = real.st_uid;
        stat.gid = real.st_gid;
        stat.rdev = real.st_rdev;
        return stat;
    }
    db_begin_read(&self.mount->db);
    inode_t inode = _identifier.longLongValue;
    if ([_identifier isEqualToString:NSFileProviderRootContainerItemIdentifier])
        inode = path_get_inode(&self.mount->db, "");
    inode_read_stat(&self.mount->db, inode, &stat);
    db_commit(&self.mount->db);
    return stat;
}
- (struct stat)realStat {
    struct stat statbuf = {};
    if (self.isDomainRoot)
        return statbuf;
    int err = fstat(_fd, &statbuf);
    [self handleError:err inFunction:@"realStat"];
    return statbuf;
}

- (NSFileProviderItemIdentifier)itemIdentifier {
    if (self.isDomainRoot)
        return NSFileProviderRootContainerItemIdentifier;
    return ISHFileProviderComposeIdentifier(self.rootName, _identifier);
}
- (NSFileProviderItemIdentifier)parentItemIdentifier {
    if (self.isDomainRoot)
        return NSFileProviderRootContainerItemIdentifier;
    if (self.isRoot) {
        // A root folder hangs directly off the domain root container.
        return NSFileProviderRootContainerItemIdentifier;
    }
    NSString *parentPath = self.path.stringByDeletingLastPathComponent;
    if ([parentPath isEqualToString:@"/"])
        return ISHFileProviderComposeIdentifier(self.rootName, NSFileProviderRootContainerItemIdentifier); // the root folder
    NSString *innerParent;
    db_begin_read(&self.mount->db);
    inode_t parentInode = path_get_inode(&self.mount->db, parentPath.UTF8String);
    db_commit(&self.mount->db);
    if (self.isVirtualItem) {
        innerParent = (parentInode != 0)
            ? [NSString stringWithFormat:@"%lu", (unsigned long) parentInode]
            : ISHFileProviderVirtualIdentifierForPath(parentPath);
    } else {
        assert(parentInode != 0);
        innerParent = [NSString stringWithFormat:@"%lu", (unsigned long) parentInode];
    }
    NSFileProviderItemIdentifier parent = ISHFileProviderComposeIdentifier(self.rootName, innerParent);
    NSLog(@"parent of %@ is %@", self.path, parent);
    return parent;
}

- (NSFileProviderItemCapabilities)capabilities {
    if (self.isDomainRoot)
        return NSFileProviderItemCapabilitiesAllowsContentEnumerating;
    if (self.isRoot) {
        // A root folder can be browsed and have files dropped into it, but the
        // root itself can't be deleted/renamed/moved through Files.
        return NSFileProviderItemCapabilitiesAllowsContentEnumerating |
               NSFileProviderItemCapabilitiesAllowsAddingSubItems;
    }
    if (self.isVirtualItem) {
        mode_t mode = self.realStat.st_mode;
        if (S_ISDIR(mode))
            return NSFileProviderItemCapabilitiesAllowsContentEnumerating;
        if (S_ISREG(mode))
            return NSFileProviderItemCapabilitiesAllowsReading;
        return 0;
    }
    NSFileProviderItemCapabilities caps = NSFileProviderItemCapabilitiesAllowsDeleting | NSFileProviderItemCapabilitiesAllowsRenaming | NSFileProviderItemCapabilitiesAllowsReparenting;
    if (S_ISREG(self.ishStat.mode))
        caps |= NSFileProviderItemCapabilitiesAllowsReading | NSFileProviderItemCapabilitiesAllowsWriting;
    else if (S_ISDIR(self.ishStat.mode))
        caps |= NSFileProviderItemCapabilitiesAllowsAddingSubItems | NSFileProviderItemCapabilitiesAllowsContentEnumerating;
    else
        return 0;
    return caps;
}

- (NSString *)filename {
    if (self.isDomainRoot)
        return @"iSH-AOK";
    if (self.isRoot)
        return self.rootName;
    NSString *filename = self.path.lastPathComponent;
    if ([filename isEqualToString:@""])
        filename = @"/";
    NSLog(@"filename %@", filename);
    return filename;
}

- (NSNumber *)documentSize {
    if (self.isDomainRoot)
        return @0;
    struct stat statbuf;
    int err = fstat(_fd, &statbuf);
    [self handleError:err inFunction:@"documentSize"];
    return [NSNumber numberWithUnsignedLongLong:statbuf.st_size];
}

- (NSNumber *)childItemCount {
    if (self.isDomainRoot)
        return nil; // unknown; the enumerator produces the root list
    if (!S_ISDIR(self.ishStat.mode))
        return @0;
    int fd = [self openNewFDWithError:nil];
    if (fd == -1)
        return @0;
    unsigned n = 0;
    DIR *dir = fdopendir(fd);
    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0)
            continue;
        n++;
    }
    closedir(dir);
    return @(n);
}

- (NSDate *)contentModificationDate {
    if (self.isDomainRoot)
        return nil;
    struct stat statbuf = self.realStat;
    return [NSDate dateWithTimeIntervalSince1970:statbuf.st_mtimespec.tv_sec + (NSTimeInterval) statbuf.st_mtimespec.tv_nsec / 1000000000];
}

- (NSString *)typeIdentifier {
    if (self.isDomainRoot || self.isRoot) {
        return (NSString *) kUTTypeFolder;
    }
    mode_t_ mode = self.ishStat.mode;
    if ((mode & S_IFMT) == S_IFDIR)
        return (NSString *) kUTTypeFolder;
    if ((mode & S_IFMT) == S_IFLNK)
        return (NSString *) kUTTypeSymLink;
    NSString *uti = CFBridgingRelease(UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension,
                                                                            (__bridge CFStringRef _Nonnull) self.path.pathExtension, nil));
    if ([uti hasPrefix:@"dyn."])
        uti = (NSString *) kUTTypePlainText;
    NSLog(@"uti of %@ is %@", self.path, uti);
    return uti;
}

// locking on these keeps the remove/copy operation atomic
// or at least tries to

- (void)loadToURL:(NSURL *)url {
    NSLog(@"copying %@ to %@", self.path, url);
    NSURL *itemURL = self.URL;
    NSError *err;
    int rootLockFd = ISHAppGroupAcquireNamedLock(@"root", self.mountOwner.rootName ?: @"", NO, nil);
    [self.mountOwner.ioLock lock];
    [NSFileManager.defaultManager removeItemAtURL:url error:nil];
    BOOL success = [NSFileManager.defaultManager copyItemAtURL:itemURL
                                                         toURL:url
                                                         error:&err];
    [self.mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!success) {
        NSLog(@"error copying to %@: %@", url, err);
    }
}

- (void)saveFromURL:(NSURL *)url {
    NSLog(@"copying %@ from %@", self.path, url);
    NSURL *itemURL = self.URL;
    NSError *err;
    int rootLockFd = ISHAppGroupAcquireNamedLock(@"root", self.mountOwner.rootName ?: @"", YES, nil);
    [self.mountOwner.ioLock lock];
    [NSFileManager.defaultManager removeItemAtURL:itemURL error:nil];
    BOOL success = [NSFileManager.defaultManager copyItemAtURL:url
                                                         toURL:itemURL
                                                         error:&err];
    [self.mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!success) {
        NSLog(@"error copying to %@: %@", url, err);
    }
}

- (void)dealloc {
    if (self.fd != -1)
        close(self.fd);
}

- (void)handleError:(long)err inFunction:(NSString *)func {
    if (err < 0) {
        [NSException raise:NSGenericException format:@"%@ returned %ld %d", func, err, errno];
    }
}

@end
