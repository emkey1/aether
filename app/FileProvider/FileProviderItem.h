//
//  FileProviderItem.h
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import <FileProvider/FileProvider.h>
#import "FileProviderExtension.h"

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString *ISHFileProviderVirtualIdentifierForPath(NSString *path);
FOUNDATION_EXPORT BOOL ISHFileProviderIsVirtualIdentifier(NSString *identifier);
FOUNDATION_EXPORT NSString * _Nullable ISHFileProviderPathForIdentifier(NSString *identifier);

@interface FileProviderItem : NSObject <NSFileProviderItem>

- (instancetype)initWithIdentifier:(NSFileProviderItemIdentifier)identifier mountOwner:(ISHFileProviderMount *)mountOwner error:(NSError *_Nullable *)err;
- (void)loadToURL:(NSURL *)url;
- (void)saveFromURL:(NSURL *)url;
- (int)openNewFDWithError:(NSError *_Nullable *)err;

@property (readonly) NSString *path;
@property (readonly) ISHFileProviderMount *mountOwner;
@property (readonly) struct fakefs_mount *mount;

@end

NS_ASSUME_NONNULL_END
