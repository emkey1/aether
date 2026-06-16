//
//  FileProviderExtension.h
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import <FileProvider/FileProvider.h>
#include "fs/fake-db.h"

NS_ASSUME_NONNULL_BEGIN

struct fakefs_mount {
    struct fakefs_db db;
    int root_fd;
    const char *_Nullable source;
};

@interface ISHFileProviderMount : NSObject

@property (nonatomic, readonly) struct fakefs_mount *mount;
// The installed root this mount is opened against. Also the name used for the
// per-root app-group lock (category "root"), so it coordinates with the running
// app's boot lock on the same root.
@property (nonatomic, readonly) NSString *rootName;
@property (nonatomic, readonly) NSURL *rootURL;
@property (nonatomic, readonly) NSRecursiveLock *ioLock;

- (nullable instancetype)initWithRootName:(NSString *)rootName error:(NSError * _Nullable * _Nullable)error;

@end

@interface FileProviderExtension : NSFileProviderExtension

// Lazily opens (and caches) the fakefs mount for a single installed root.
- (nullable ISHFileProviderMount *)mountForRootName:(NSString *)rootName error:(NSError * _Nullable * _Nullable)error;
// The valid installed root names found in the shared app-group container.
- (NSArray<NSString *> *)installedRootNames;

@end

FOUNDATION_EXPORT void ISHFileProviderRecordBreadcrumb(NSString *event, NSDictionary<NSString *, id> * _Nullable details);

NS_ASSUME_NONNULL_END
