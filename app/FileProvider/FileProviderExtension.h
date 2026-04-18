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
@property (nonatomic, readonly) NSString *domainIdentifier;
@property (nonatomic, readonly) NSURL *rootURL;
@property (nonatomic, readonly) NSRecursiveLock *ioLock;

- (nullable instancetype)initWithDomainIdentifier:(NSString *)domainIdentifier error:(NSError * _Nullable * _Nullable)error;

@end

@interface FileProviderExtension : NSFileProviderExtension

- (struct fakefs_mount *)mount;

@end

FOUNDATION_EXPORT void ISHFileProviderRecordBreadcrumb(NSString *event, NSDictionary<NSString *, id> * _Nullable details);

NS_ASSUME_NONNULL_END
