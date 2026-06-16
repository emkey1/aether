//
//  FileProviderEnumerator.m
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import <MobileCoreServices/MobileCoreServices.h>
#include <dirent.h>
#import "../AppGroup.h"
#import "FileProviderExtension.h"
#import "FileProviderEnumerator.h"
#import "FileProviderItem.h"
#import "NSError+ISHErrno.h"
#include "fs/fake-db.h"

static NSNumber *ISHFileProviderEnumeratorDurationMilliseconds(NSTimeInterval start) {
    return @((NSInteger) ((NSDate.date.timeIntervalSinceReferenceDate - start) * 1000.0));
}

@interface FileProviderEnumerator ()

@property FileProviderItem *item;
@property (weak) FileProviderExtension *extension;

@end

@implementation FileProviderEnumerator

- (instancetype)initWithItem:(FileProviderItem *)item extension:(FileProviderExtension *)extension {
    if (self = [super init]) {
        self.item = item;
        self.extension = extension;
    }
    return self;
}

- (void)enumerateItemsForObserver:(id<NSFileProviderEnumerationObserver>)observer startingAtPage:(NSFileProviderPage)page {
    NSLog(@"enumeration start %@", self.item.itemIdentifier);
    NSString *containerIdentifier = self.item.itemIdentifier ?: @"working-set";
    NSString *domainIdentifier = self.item.mountOwner.rootName ?: @"iSH-AOK";
    ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.begin",
                                    @{@"container": containerIdentifier,
                                      @"domain": domainIdentifier});
    NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
    // if we're asked to enumerate the working set
    if (self.item == nil) {
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"count": @0});
        [observer finishEnumeratingUpToPage:page];
        return;
    }
    // The synthetic domain root lists each installed root as a folder.
    if (self.item.mountOwner == nil) {
        NSMutableArray<FileProviderItem *> *items = [NSMutableArray new];
        for (NSString *rootName in [self.extension installedRootNames]) {
            NSError *err;
            ISHFileProviderMount *mount = [self.extension mountForRootName:rootName error:&err];
            if (mount == nil) {
                NSLog(@"skipping root %@: %@", rootName, err);
                continue;
            }
            FileProviderItem *rootItem = [[FileProviderItem alloc] initWithIdentifier:NSFileProviderRootContainerItemIdentifier mountOwner:mount error:&err];
            if (rootItem != nil)
                [items addObject:rootItem];
        }
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"count": @(items.count)});
        [observer didEnumerateItems:items];
        [observer finishEnumeratingUpToPage:nil];
        return;
    }
    // if we're asked to enumerate a file
    if (![self.item.typeIdentifier isEqualToString:(NSString *) kUTTypeFolder]) {
        NSLog(@"not enumerating a file (%@)", self.item.typeIdentifier);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"count": @0,
                                          @"skipped": @YES});
        [observer finishEnumeratingUpToPage:page];
        return;
    }

    int rootLockFd = ISHAppGroupAcquireNamedLock(@"root", self.item.mountOwner.rootName ?: @"", NO, nil);

    NSError *error;
    int fd = [self.item openNewFDWithError:&error];
    if (fd == -1) {
        ISHAppGroupReleaseLock(rootLockFd);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.failed",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"error": error.localizedDescription ?: @"unknown"});
        [observer finishEnumeratingWithError:error];
        return;
    }
    DIR *dir = fdopendir(fd);
    NSMutableArray<FileProviderItem *> *items = [NSMutableArray new];
    struct dirent *dirent;
    errno = 0;
    while ((dirent = readdir(dir))) {
        if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0)
            continue;

        NSString *path = _item.path;
        NSString *name = [NSString stringWithUTF8String:dirent->d_name];
        // Join with an explicit leading slash so both a root folder (path == "")
        // and a sub-directory (path == "/bin") produce the slash-prefixed path
        // the fakefs db stores ("/bin", "/bin/ls"). -stringByAppendingPathComponent:
        // drops the leading slash on an empty receiver, which left a root's
        // direct children unresolvable (every lookup missed -> empty listing).
        NSString *childPath = [NSString stringWithFormat:@"%@/%@", path, name];
        db_begin_read(&_item.mount->db);
        inode_t inode = path_get_inode(&_item.mount->db, childPath.fileSystemRepresentation);
        db_commit(&_item.mount->db);
        NSString *childIdent;
        if (inode == 0) {
            childIdent = ISHFileProviderVirtualIdentifierForPath(childPath);
        } else {
            childIdent = [NSString stringWithFormat:@"%lu", (unsigned long) inode];
        }

        NSLog(@"returning %s %@", dirent->d_name, childIdent);
        FileProviderItem *item = [[FileProviderItem alloc] initWithIdentifier:childIdent mountOwner:_item.mountOwner error:&error];
        if (item == nil) {
            ISHAppGroupReleaseLock(rootLockFd);
            ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.failed",
                                            @{@"container": containerIdentifier,
                                              @"domain": domainIdentifier,
                                              @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                              @"error": error.localizedDescription ?: @"unknown"});
            [observer finishEnumeratingWithError:error];
            closedir(dir);
            return;
        }
        [items addObject:item];
        errno = 0;
    }
    if (errno != 0) {
        NSError *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        NSLog(@"readdir returned %@", error);
        ISHAppGroupReleaseLock(rootLockFd);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.failed",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"error": error.localizedDescription ?: @"unknown"});
        [observer finishEnumeratingWithError:error];
        closedir(dir);
        return;
    }

    closedir(dir);
    ISHAppGroupReleaseLock(rootLockFd);
    NSLog(@"returning %@", items);
    ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end",
                                    @{@"container": containerIdentifier,
                                      @"domain": domainIdentifier,
                                      @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                      @"count": @(items.count)});
    [observer didEnumerateItems:items];
    [observer finishEnumeratingUpToPage:nil];
}

- (void)enumerateChangesForObserver:(id<NSFileProviderChangeObserver>)observer fromSyncAnchor:(NSFileProviderSyncAnchor)anchor {
    NSLog(@"saying no file changes");
    // TODO implement by having the sync anchor be a serialized list of files
    [observer finishEnumeratingChangesUpToSyncAnchor:anchor moreComing:NO];
}

- (void)invalidate {
}

@end
