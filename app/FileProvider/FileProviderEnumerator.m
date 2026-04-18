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

@end

@implementation FileProviderEnumerator

- (instancetype)initWithItem:(FileProviderItem *)item {
    if (self = [super init]) {
        self.item = item;
    }
    return self;
}

- (void)enumerateItemsForObserver:(id<NSFileProviderEnumerationObserver>)observer startingAtPage:(NSFileProviderPage)page {
    NSLog(@"enumeration start %@", self.item.itemIdentifier);
    NSString *containerIdentifier = self.item.itemIdentifier ?: @"working-set";
    NSString *domainIdentifier = self.item.mountOwner.domainIdentifier ?: @"";
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

    int rootLockFd = ISHAppGroupAcquireNamedLock(@"root", domainIdentifier, NO, nil);
    
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

        // this is annoying
        NSString *path = _item.path;
        NSString *childIdent;
        if (strcmp(dirent->d_name, "..") == 0) {
            childIdent = _item.parentItemIdentifier;
        } else if (strcmp(dirent->d_name, ".") != 0) {
            NSString *childPath = [path stringByAppendingPathComponent:[NSString stringWithUTF8String:dirent->d_name]];
            db_begin_read(&_item.mount->db);
            inode_t inode = path_get_inode(&_item.mount->db, childPath.fileSystemRepresentation);
            db_commit(&_item.mount->db);
            if (inode == 0) {
                childIdent = ISHFileProviderVirtualIdentifierForPath(childPath);
            } else {
                childIdent = [NSString stringWithFormat:@"%lu", (unsigned long) inode];
            }
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
