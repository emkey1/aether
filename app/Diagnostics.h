#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const ISHDiagnosticsStoreDidUpdateNotification;

@interface ISHDiagnosticsStore : NSObject

+ (void)recordBreadcrumb:(NSString *)event;
+ (void)recordBreadcrumb:(NSString *)event details:(nullable NSDictionary<NSString *, id> *)details;
+ (void)recordLaunchStage:(NSString *)stage;
+ (void)recordLaunchStage:(NSString *)stage details:(nullable NSDictionary<NSString *, id> *)details;
+ (void)completeLaunchWithDetails:(nullable NSDictionary<NSString *, id> *)details;
+ (nullable NSDictionary<NSString *, id> *)currentLaunchJournal;
+ (nullable NSDictionary<NSString *, id> *)currentGuestFatalEvent;
+ (NSArray<NSDictionary<NSString *, id> *> *)recentGuestExitsWithLimit:(NSUInteger)limit;
+ (NSArray<NSDictionary<NSString *, id> *> *)recentBreadcrumbsWithLimit:(NSUInteger)limit;
+ (NSArray<NSDictionary<NSString *, id> *> *)recentMetricKitPayloadsWithLimit:(NSUInteger)limit;
+ (NSString *)diagnosticsReport;
+ (nullable NSURL *)prepareExportBundle:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
