#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const ISHDiagnosticsStoreDidUpdateNotification;

@interface ISHDiagnosticsStore : NSObject

+ (void)recordBreadcrumb:(NSString *)event;
+ (void)recordBreadcrumb:(NSString *)event details:(nullable NSDictionary<NSString *, id> *)details;
+ (NSArray<NSDictionary<NSString *, id> *> *)recentBreadcrumbsWithLimit:(NSUInteger)limit;
+ (NSArray<NSDictionary<NSString *, id> *> *)recentMetricKitPayloadsWithLimit:(NSUInteger)limit;
+ (NSString *)diagnosticsReport;
+ (nullable NSURL *)prepareExportBundle:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
