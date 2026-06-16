//
//  FileProviderEnumerator.h
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import <FileProvider/FileProvider.h>
#import "FileProviderItem.h"
#import "FileProviderExtension.h"

@interface FileProviderEnumerator : NSObject <NSFileProviderEnumerator>

- (instancetype)init NS_UNAVAILABLE;
// `extension` is needed so the domain-root enumerator can resolve a mount per
// installed root when listing the roots as folders.
- (instancetype)initWithItem:(FileProviderItem *)item extension:(FileProviderExtension *)extension;

@end
