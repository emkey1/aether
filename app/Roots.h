//
//  Roots.h
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@protocol ProgressReporter

- (void)updateProgress:(double)progressFraction message:(NSString *)progressMessage;
- (BOOL)shouldCancel;

@end

FOUNDATION_EXPORT NSNotificationName const RootsDidFinishInitialSelectionNotification;

@interface Roots : NSObject

+ (instancetype)instance;

@property (readonly) NSOrderedSet<NSString *> *roots;
@property NSString *defaultRoot;
@property (readonly) BOOL wantsVersionFile;
@property (readonly) BOOL needsInitialRootSelection;
@property (readonly) BOOL initialBundledRootImportInProgress;
@property (readonly, nullable) NSError *initialBundledRootImportError;
- (NSArray<NSDictionary<NSString *, NSString *> *> *)bundledRootChoices;
- (NSURL *)rootUrl:(NSString *)name;
- (nullable NSString *)guestABIForRootNamed:(NSString *)name;
// Archive files in the shared /AOK/persist/roots directory (the AppGroup
// container's AOK/persist mount, common to all roots). Surfaced in the
// Filesystems screen as the "Root Cached Filesystems" section.
- (NSArray<NSURL *> *)cachedRootArchiveURLs;
- (void)resumeDeferredFileProviderDomainSync;
- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)importBundledRootChoice:(NSString *)identifier error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error;
- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
