//
//  AppDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#include <resolv.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <MetricKit/MetricKit.h>
#import "AboutViewController.h"
#import "AppDelegate.h"
#import "AppGroup.h"
#import "CurrentRoot.h"
#import "Diagnostics.h"
#import "iOSFS.h"
#import "SceneDelegate.h"
#import "AudioDevice.h"
#import "PasteboardDevice.h"
#import "LocationDevice.h"
#import "NSObject+SaneKVO.h"
#import "Roots.h"
#import "TerminalViewController.h"
#import "UserPreferences.h"
#import "UIApplication+OpenURL.h"
#include "kernel/init.h"
#include "kernel/calls.h"
#include "fs/dyndev.h"
#include "fs/devices.h"
#include "fs/path.h"
#include "app/RTCDevice.h"
#include "util/sync.h"

#if ISH_LINUX
#import "LinuxInterop.h"
#endif

@class ISHMetricKitSubscriber;

@interface AppDelegate ()

@property BOOL exiting;
@property SCNetworkReachabilityRef reachability;
@property (strong, nonatomic) ISHMetricKitSubscriber *metricKitSubscriber;
@property BOOL dnsRefreshQueued;
@property BOOL dnsRefreshRunning;
@property BOOL waitingForInitialRootImport;

@end

#if !ISH_LINUX
static void ios_handle_exit(struct task *task, int code) {
    // we are interested in init and in children of init
    // this is called with pids_lock as an implementation side effect, please do not cite as an example of good API design
    if (task == NULL)
        return;

    if (task->pid > MAX_PID) { // Corruption
        printk("ERROR: Insane PID in ios_handle_exit(%d)\n", task->pid);
        return;
    }

    // The exit hook runs under pids_lock, and for the leader exit path the task's
    // general_lock is still held by do_exit(). Taking it again here deadlocks the
    // exiting thread and wedges any later task creation behind pids_lock.
    bool should_notify = task->parent == NULL || task->parent->parent == NULL;
    if (!should_notify)
        return;

    // pid should be saved now since task would be freed
    pid_t pid = task->pid;

    dispatch_async(dispatch_get_main_queue(), ^{
        [ISHDiagnosticsStore recordBreadcrumb:@"process.exit"
                                      details:@{@"pid": @(pid), @"code": @(code)}];
        [[NSNotificationCenter defaultCenter] postNotificationName:ProcessExitedNotification
                                                            object:nil
                                                          userInfo:@{@"pid": @(pid),
                                                                     @"code": @(code)}];
    });
}

const char* getRenameRunDirString(void) {
    NSDate *currentDate = [NSDate date];
    NSDateFormatter *dateFormatter = [[NSDateFormatter alloc] init];
    [dateFormatter setDateFormat:@"yyyy-MM-dd_HH-mm-ss"];
    NSString *timestamp = [dateFormatter stringFromDate:currentDate];

    NSString *prefixedTimestamp = [NSString stringWithFormat:@"/tmp/old-run/%@", timestamp];

    // Convert to const char* and return
    return [prefixedTimestamp UTF8String];
}

// Put the abort message in the thread name so it gets included in the crash dump
static void ios_handle_die(const char *msg) {
    char name[17];
    pthread_getname_np(pthread_self(), name, sizeof(name));
    NSString *newName = [NSString stringWithFormat:@"%s died: %s", name, msg];
    pthread_setname_np(newName.UTF8String);
}
#elif ISH_LINUX
void ReportPanic(const char *message) {
    [NSNotificationCenter.defaultCenter postNotificationName:KernelPanicNotification object:nil userInfo:@{@"message":@(message)}];
}
#endif

static intptr_t bootError;
static NSString *const kSkipStartupMessage = @"Skip Startup Message";
static NSString *const kMetricKitDiagnosticsDirectory = @"MetricKitDiagnostics";
NSString *const ISHDiagnosticsStoreDidUpdateNotification = @"ISHDiagnosticsStoreDidUpdateNotification";
static NSString *const kDiagnosticsDirectory = @"Diagnostics";
static NSString *const kDiagnosticsBreadcrumbsFile = @"breadcrumbs.json";

static NSString *MetricKitISO8601StringFromDate(NSDate *date) {
    if (date == nil)
        return nil;
    static NSISO8601DateFormatter *formatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        formatter = [NSISO8601DateFormatter new];
        formatter.formatOptions = NSISO8601DateFormatWithInternetDateTime;
    });
    return [formatter stringFromDate:date];
}

static double MetricKitNowSeconds(void) {
    return CFAbsoluteTimeGetCurrent();
}

static NSString *MetricKitSafeDescription(id value) {
    if (value == nil || value == [NSNull null])
        return nil;
    return [value description];
}

static NSURL *MetricKitDiagnosticsDirectoryURL(void) {
    NSURL *baseURL = ContainerURL();
    if (baseURL == nil) {
        baseURL = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                      inDomains:NSUserDomainMask].firstObject;
    }
    if (baseURL == nil)
        return nil;
    return [baseURL URLByAppendingPathComponent:kMetricKitDiagnosticsDirectory isDirectory:YES];
}

static NSURL *DiagnosticsDirectoryURL(void) {
    NSURL *baseURL = ContainerURL();
    if (baseURL == nil) {
        baseURL = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                      inDomains:NSUserDomainMask].firstObject;
    }
    if (baseURL == nil)
        return nil;
    return [baseURL URLByAppendingPathComponent:kDiagnosticsDirectory isDirectory:YES];
}

static NSURL *DiagnosticsBreadcrumbsURL(void) {
    NSURL *directoryURL = DiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return nil;
    return [directoryURL URLByAppendingPathComponent:kDiagnosticsBreadcrumbsFile isDirectory:NO];
}

static NSString *DiagnosticsISO8601StringFromDate(NSDate *date) {
    return MetricKitISO8601StringFromDate(date);
}

static NSString *DiagnosticsHostMachine(void) {
    struct utsname systemInfo;
    if (uname(&systemInfo) != 0)
        return nil;
    return [NSString stringWithUTF8String:systemInfo.machine];
}

static NSString *DiagnosticsByteCountString(long long bytes) {
    return [NSByteCountFormatter stringFromByteCount:bytes countStyle:NSByteCountFormatterCountStyleFile];
}

static int EnsurePathRemoved(const char *path, const struct statbuf *stat) {
    if (S_ISDIR(stat->mode))
        return generic_rmdirat(AT_PWD, path);
    return generic_unlinkat(AT_PWD, path);
}

static int EnsureCharacterDevice(const char *path, mode_t_ mode, dev_t_ device) {
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, AT_SYMLINK_NOFOLLOW_);
    if (err == _ENOENT)
        return generic_mknodat(AT_PWD, path, mode, device);
    if (err < 0)
        return err;

    mode_t_ permissions = mode & 07777;
    bool wrongType = !S_ISCHR(stat.mode);
    bool wrongDevice = stat.rdev != device;
    if (wrongType || wrongDevice) {
        err = EnsurePathRemoved(path, &stat);
        if (err < 0)
            return err;
        return generic_mknodat(AT_PWD, path, mode, device);
    }

    if ((stat.mode & 07777) != permissions)
        return generic_setattrat(AT_PWD, path, make_attr(mode, permissions), false);
    return 0;
}

static int EnsureSymlink(const char *path, const char *target) {
    char existing[MAX_PATH];
    ssize_t len = generic_readlinkat(AT_PWD, path, existing, sizeof(existing) - 1);
    if (len == _ENOENT)
        return generic_symlinkat(target, AT_PWD, path);
    if (len >= 0) {
        existing[len] = '\0';
        if (strcmp(existing, target) == 0)
            return 0;
        struct statbuf stat;
        int err = generic_statat(AT_PWD, path, &stat, AT_SYMLINK_NOFOLLOW_);
        if (err < 0)
            return err;
        err = EnsurePathRemoved(path, &stat);
        if (err < 0)
            return err;
        return generic_symlinkat(target, AT_PWD, path);
    }

    if (len == _EINVAL) {
        struct statbuf stat;
        int err = generic_statat(AT_PWD, path, &stat, AT_SYMLINK_NOFOLLOW_);
        if (err < 0)
            return err;
        err = EnsurePathRemoved(path, &stat);
        if (err < 0)
            return err;
        return generic_symlinkat(target, AT_PWD, path);
    }
    return (int) len;
}

@implementation ISHDiagnosticsStore

+ (dispatch_queue_t)queue {
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("app.ish.iSH-AOK.Diagnostics", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

+ (void)recordBreadcrumb:(NSString *)event {
    [self recordBreadcrumb:event details:nil];
}

+ (void)recordBreadcrumb:(NSString *)event details:(NSDictionary<NSString *,id> *)details {
    if (event.length == 0)
        return;
    dispatch_async(self.queue, ^{
        NSURL *directoryURL = DiagnosticsDirectoryURL();
        NSURL *breadcrumbsURL = DiagnosticsBreadcrumbsURL();
        if (directoryURL == nil || breadcrumbsURL == nil)
            return;
        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];

        NSData *existingData = [NSData dataWithContentsOfURL:breadcrumbsURL];
        NSMutableArray<NSDictionary<NSString *, id> *> *breadcrumbs = [NSMutableArray array];
        if (existingData.length > 0) {
            id existingObject = [NSJSONSerialization JSONObjectWithData:existingData options:NSJSONReadingMutableContainers error:nil];
            if ([existingObject isKindOfClass:[NSArray class]])
                [breadcrumbs addObjectsFromArray:existingObject];
        }

        NSMutableDictionary<NSString *, id> *entry = [NSMutableDictionary dictionary];
        entry[@"timestamp"] = DiagnosticsISO8601StringFromDate([NSDate date]) ?: @"";
        entry[@"event"] = event;
        if (details.count != 0)
            entry[@"details"] = details;
        [breadcrumbs addObject:entry];

        const NSUInteger maxBreadcrumbs = 200;
        if (breadcrumbs.count > maxBreadcrumbs) {
            NSRange overflow = NSMakeRange(0, breadcrumbs.count - maxBreadcrumbs);
            [breadcrumbs removeObjectsInRange:overflow];
        }

        NSData *jsonData = [NSJSONSerialization dataWithJSONObject:breadcrumbs options:NSJSONWritingPrettyPrinted error:nil];
        if (jsonData != nil) {
            [jsonData writeToURL:breadcrumbsURL options:NSDataWritingAtomic error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSNotificationCenter.defaultCenter postNotificationName:ISHDiagnosticsStoreDidUpdateNotification object:nil];
            });
        }
    });
}

+ (NSArray<NSDictionary<NSString *,id> *> *)recentBreadcrumbsWithLimit:(NSUInteger)limit {
    __block NSArray<NSDictionary<NSString *, id> *> *result = @[];
    dispatch_sync(self.queue, ^{
        NSData *data = [NSData dataWithContentsOfURL:DiagnosticsBreadcrumbsURL()];
        if (data.length == 0)
            return;
        id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![object isKindOfClass:[NSArray class]])
            return;
        NSArray<NSDictionary<NSString *, id> *> *entries = object;
        if (limit == 0 || entries.count <= limit) {
            result = [[entries reverseObjectEnumerator] allObjects];
        } else {
            NSRange range = NSMakeRange(entries.count - limit, limit);
            result = [[[entries subarrayWithRange:range] reverseObjectEnumerator] allObjects];
        }
    });
    return result;
}

+ (NSArray<NSDictionary<NSString *,id> *> *)recentMetricKitPayloadsWithLimit:(NSUInteger)limit {
    NSURL *directoryURL = MetricKitDiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return @[];

    NSArray<NSURL *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtURL:directoryURL
                                                          includingPropertiesForKeys:@[NSURLContentModificationDateKey]
                                                                             options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                               error:nil];
    files = [files sortedArrayUsingComparator:^NSComparisonResult(NSURL *left, NSURL *right) {
        NSDate *leftDate = nil;
        NSDate *rightDate = nil;
        [left getResourceValue:&leftDate forKey:NSURLContentModificationDateKey error:nil];
        [right getResourceValue:&rightDate forKey:NSURLContentModificationDateKey error:nil];
        return [rightDate compare:leftDate];
    }];
    if (limit != 0 && files.count > limit)
        files = [files subarrayWithRange:NSMakeRange(0, limit)];

    NSMutableArray<NSDictionary<NSString *, id> *> *payloads = [NSMutableArray array];
    for (NSURL *fileURL in files) {
        NSData *data = [NSData dataWithContentsOfURL:fileURL];
        if (data.length == 0)
            continue;
        id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![object isKindOfClass:[NSDictionary class]])
            continue;
        NSMutableDictionary<NSString *, id> *entry = [object mutableCopy];
        entry[@"filename"] = fileURL.lastPathComponent ?: @"";
        [payloads addObject:entry];
    }
    return payloads;
}

+ (NSDictionary<NSString *, id> *)currentSummaryDictionary {
    NSMutableDictionary<NSString *, id> *summary = [NSMutableDictionary dictionary];
    NSDictionary *info = NSBundle.mainBundle.infoDictionary ?: @{};
    summary[@"appVersion"] = info[@"CFBundleShortVersionString"] ?: @"";
    summary[@"build"] = info[@"CFBundleVersion"] ?: @"";
    summary[@"deviceName"] = UIDevice.currentDevice.name ?: @"";
    summary[@"deviceModel"] = UIDevice.currentDevice.model ?: @"";
    summary[@"hostMachine"] = DiagnosticsHostMachine() ?: @"";
    summary[@"systemVersion"] = UIDevice.currentDevice.systemVersion ?: @"";
    summary[@"defaultRoot"] = Roots.instance.defaultRoot ?: @"";
    summary[@"rootCount"] = @(Roots.instance.roots.count);
    summary[@"needsInitialRootSelection"] = @(Roots.instance.needsInitialRootSelection);
    if (Roots.instance.initialBundledRootImportError != nil)
        summary[@"initialRootImportError"] = Roots.instance.initialBundledRootImportError.localizedDescription ?: @"";

    NSURL *containerURL = ContainerURL();
    if (containerURL != nil) {
        NSDictionary<NSFileAttributeKey, id> *attributes =
            [NSFileManager.defaultManager attributesOfFileSystemForPath:containerURL.path error:nil];
        NSNumber *freeBytes = attributes[NSFileSystemFreeSize];
        if (freeBytes != nil) {
            summary[@"freeBytes"] = freeBytes;
            summary[@"freeSpace"] = DiagnosticsByteCountString(freeBytes.longLongValue);
        }
    }
    return summary;
}

+ (NSString *)diagnosticsReport {
    NSMutableString *report = [NSMutableString string];
    NSDictionary<NSString *, id> *summary = [self currentSummaryDictionary];
    [report appendString:@"iSH-AOK Diagnostics\n\n"];
    [report appendFormat:@"App: %@ (Build %@)\n", summary[@"appVersion"], summary[@"build"]];
    [report appendFormat:@"Device: %@ / %@ / %@\n", summary[@"deviceName"], summary[@"deviceModel"], summary[@"hostMachine"]];
    [report appendFormat:@"OS: iOS %@\n", summary[@"systemVersion"]];
    [report appendFormat:@"Free Space: %@\n", summary[@"freeSpace"] ?: @"unknown"];
    [report appendFormat:@"Default Root: %@\n", summary[@"defaultRoot"] ?: @"(none)"];
    [report appendFormat:@"Roots: %@\n", summary[@"rootCount"]];
    [report appendFormat:@"Needs Initial Root Selection: %@\n", [summary[@"needsInitialRootSelection"] boolValue] ? @"yes" : @"no"];
    if (summary[@"initialRootImportError"] != nil)
        [report appendFormat:@"Initial Root Import Error: %@\n", summary[@"initialRootImportError"]];

    NSArray<NSDictionary<NSString *, id> *> *payloads = [self recentMetricKitPayloadsWithLimit:5];
    [report appendString:@"\nRecent MetricKit Payloads\n"];
    if (payloads.count == 0) {
        [report appendString:@"  none\n"];
    } else {
        for (NSDictionary<NSString *, id> *payload in payloads) {
            [report appendFormat:@"  %@\n", payload[@"filename"] ?: @"payload.json"];
            if (payload[@"receivedAt"] != nil)
                [report appendFormat:@"    received: %@\n", payload[@"receivedAt"]];
            if (payload[@"timeStampBegin"] != nil || payload[@"timeStampEnd"] != nil) {
                [report appendFormat:@"    window: %@ -> %@\n",
                 payload[@"timeStampBegin"] ?: @"?",
                 payload[@"timeStampEnd"] ?: @"?"];
            }
            NSArray *summaries = payload[@"summaries"];
            for (NSDictionary<NSString *, id> *entry in summaries) {
                [report appendFormat:@"    %@: ", entry[@"kind"] ?: @"diagnostic"];
                NSMutableArray<NSString *> *parts = [NSMutableArray array];
                if (entry[@"terminationReason"] != nil)
                    [parts addObject:[NSString stringWithFormat:@"termination=%@", entry[@"terminationReason"]]];
                if (entry[@"signal"] != nil)
                    [parts addObject:[NSString stringWithFormat:@"signal=%@", entry[@"signal"]]];
                if (entry[@"exceptionType"] != nil)
                    [parts addObject:[NSString stringWithFormat:@"exceptionType=%@", entry[@"exceptionType"]]];
                if (entry[@"hangDuration"] != nil)
                    [parts addObject:[NSString stringWithFormat:@"hang=%@", entry[@"hangDuration"]]];
                if (parts.count == 0)
                    [parts addObject:@"no summary fields"];
                [report appendFormat:@"%@\n", [parts componentsJoinedByString:@", "]];
            }
        }
    }

    NSArray<NSDictionary<NSString *, id> *> *breadcrumbs = [self recentBreadcrumbsWithLimit:50];
    [report appendString:@"\nRecent Breadcrumbs\n"];
    if (breadcrumbs.count == 0) {
        [report appendString:@"  none\n"];
    } else {
        for (NSDictionary<NSString *, id> *entry in breadcrumbs) {
            [report appendFormat:@"  %@  %@",
             entry[@"timestamp"] ?: @"",
             entry[@"event"] ?: @""];
            NSDictionary *details = entry[@"details"];
            if (details.count != 0)
                [report appendFormat:@"  %@",
                 [[details description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "]];
            [report appendString:@"\n"];
        }
    }
    return report;
}

+ (NSURL *)prepareExportBundle:(NSError **)error {
    NSURL *baseDirectory = [NSFileManager.defaultManager.temporaryDirectory
                            URLByAppendingPathComponent:[NSString stringWithFormat:@"diagnostics-%@", NSUUID.UUID.UUIDString]
                                            isDirectory:YES];
    if (![NSFileManager.defaultManager createDirectoryAtURL:baseDirectory
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:error]) {
        return nil;
    }

    NSURL *reportURL = [baseDirectory URLByAppendingPathComponent:@"diagnostics-report.txt"];
    NSData *reportData = [[self diagnosticsReport] dataUsingEncoding:NSUTF8StringEncoding];
    if (![reportData writeToURL:reportURL options:NSDataWritingAtomic error:error])
        return nil;

    NSURL *breadcrumbsURL = DiagnosticsBreadcrumbsURL();
    if (breadcrumbsURL != nil && [NSFileManager.defaultManager fileExistsAtPath:breadcrumbsURL.path]) {
        [NSFileManager.defaultManager copyItemAtURL:breadcrumbsURL
                                              toURL:[baseDirectory URLByAppendingPathComponent:breadcrumbsURL.lastPathComponent]
                                              error:nil];
    }

    NSURL *metricDirectoryURL = MetricKitDiagnosticsDirectoryURL();
    NSArray<NSURL *> *metricFiles = [NSFileManager.defaultManager contentsOfDirectoryAtURL:metricDirectoryURL
                                                                includingPropertiesForKeys:nil
                                                                                   options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                                     error:nil];
    if (metricFiles.count != 0) {
        NSURL *exportMetricURL = [baseDirectory URLByAppendingPathComponent:kMetricKitDiagnosticsDirectory isDirectory:YES];
        [NSFileManager.defaultManager createDirectoryAtURL:exportMetricURL
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];
        for (NSURL *fileURL in metricFiles) {
            [NSFileManager.defaultManager copyItemAtURL:fileURL
                                                  toURL:[exportMetricURL URLByAppendingPathComponent:fileURL.lastPathComponent]
                                                  error:nil];
        }
    }
    return baseDirectory;
}

@end

static NSDictionary *MetricKitDiagnosticSummary(MXDiagnostic *diagnostic, NSString *kind) API_AVAILABLE(ios(14.0));
static NSDictionary *MetricKitDiagnosticSummary(MXDiagnostic *diagnostic, NSString *kind) {
    NSMutableDictionary *summary = [NSMutableDictionary dictionary];
    summary[@"kind"] = kind;

    NSString *applicationVersion = diagnostic.applicationVersion;
    if (applicationVersion != nil)
        summary[@"applicationVersion"] = applicationVersion;

    MXMetaData *metadata = diagnostic.metaData;
    NSString *buildVersion = metadata.applicationBuildVersion;
    if (buildVersion != nil)
        summary[@"applicationBuildVersion"] = buildVersion;
    NSString *osVersion = metadata.osVersion;
    if (osVersion != nil)
        summary[@"osVersion"] = osVersion;
    NSString *deviceType = metadata.deviceType;
    if (deviceType != nil)
        summary[@"deviceType"] = deviceType;
    NSString *architecture = metadata.platformArchitecture;
    if (architecture != nil)
        summary[@"platformArchitecture"] = architecture;

    if ([kind isEqualToString:@"crash"]) {
        MXCrashDiagnostic *crashDiagnostic = (MXCrashDiagnostic *) diagnostic;
        NSString *terminationReason = crashDiagnostic.terminationReason;
        if (terminationReason != nil)
            summary[@"terminationReason"] = terminationReason;

        NSNumber *exceptionType = crashDiagnostic.exceptionType;
        if (exceptionType != nil)
            summary[@"exceptionType"] = exceptionType;

        NSNumber *exceptionCode = crashDiagnostic.exceptionCode;
        if (exceptionCode != nil)
            summary[@"exceptionCode"] = exceptionCode;

        NSNumber *signal = crashDiagnostic.signal;
        if (signal != nil)
            summary[@"signal"] = signal;

        NSString *vmInfo = crashDiagnostic.virtualMemoryRegionInfo;
        if (vmInfo != nil)
            summary[@"virtualMemoryRegionInfo"] = vmInfo;
    } else if ([kind isEqualToString:@"hang"]) {
        MXHangDiagnostic *hangDiagnostic = (MXHangDiagnostic *) diagnostic;
        NSString *hangDurationDescription = MetricKitSafeDescription(hangDiagnostic.hangDuration);
        if (hangDurationDescription != nil)
            summary[@"hangDuration"] = hangDurationDescription;
    }

    return summary;
}

@interface ISHMetricKitSubscriber : NSObject <MXMetricManagerSubscriber>

- (void)registerIfAvailable;
- (void)unregisterIfNeeded;
- (void)persistDiagnosticPayload:(id)payload index:(NSUInteger)index API_AVAILABLE(ios(14.0));

@end

@implementation ISHMetricKitSubscriber {
    id _metricManager;
}

- (void)registerIfAvailable {
    if (@available(iOS 14.0, *)) {
        MXMetricManager *metricManager = MXMetricManager.sharedManager;
        if (metricManager == nil) {
            NSLog(@"MetricKit unavailable: shared manager missing");
            return;
        }

        _metricManager = metricManager;
        [metricManager addSubscriber:self];
        NSLog(@"MetricKit diagnostic subscriber registered");
    }
}

- (void)unregisterIfNeeded {
    if (@available(iOS 14.0, *)) {
        if (_metricManager != nil)
            [(MXMetricManager *) _metricManager removeSubscriber:self];
        _metricManager = nil;
    }
}

- (void)didReceiveDiagnosticPayloads:(NSArray<MXDiagnosticPayload *> *)payloads API_AVAILABLE(ios(14.0)) {
    if (@available(iOS 14.0, *)) {
        NSLog(@"MetricKit delivered %lu diagnostic payload(s)", (unsigned long) payloads.count);
        [ISHDiagnosticsStore recordBreadcrumb:@"metrickit.payloads"
                                      details:@{@"count": @(payloads.count)}];
        for (NSUInteger i = 0; i < payloads.count; i++) {
            [self persistDiagnosticPayload:payloads[i] index:i];
        }
    }
}

- (void)persistDiagnosticPayload:(MXDiagnosticPayload *)payload index:(NSUInteger)index API_AVAILABLE(ios(14.0)) {
    NSArray<MXCrashDiagnostic *> *crashDiagnostics = payload.crashDiagnostics ?: @[];
    NSArray<MXHangDiagnostic *> *hangDiagnostics = payload.hangDiagnostics ?: @[];
    NSDate *timeStampBegin = payload.timeStampBegin;
    NSDate *timeStampEnd = payload.timeStampEnd;

    NSMutableArray *summaries = [NSMutableArray array];
    for (id crashDiagnostic in crashDiagnostics) {
        NSDictionary *summary = MetricKitDiagnosticSummary(crashDiagnostic, @"crash");
        [summaries addObject:summary];
        NSLog(@"MetricKit crash diagnostic: %@", summary);
    }
    for (id hangDiagnostic in hangDiagnostics) {
        NSDictionary *summary = MetricKitDiagnosticSummary(hangDiagnostic, @"hang");
        [summaries addObject:summary];
        NSLog(@"MetricKit hang diagnostic: %@", summary);
    }

    NSMutableDictionary *envelope = [NSMutableDictionary dictionary];
    NSString *receivedAt = MetricKitISO8601StringFromDate([NSDate date]);
    NSString *beginAt = MetricKitISO8601StringFromDate(timeStampBegin);
    NSString *endAt = MetricKitISO8601StringFromDate(timeStampEnd);
    if (receivedAt != nil)
        envelope[@"receivedAt"] = receivedAt;
    if (beginAt != nil)
        envelope[@"timeStampBegin"] = beginAt;
    if (endAt != nil)
        envelope[@"timeStampEnd"] = endAt;
    envelope[@"crashDiagnosticCount"] = @(crashDiagnostics.count);
    envelope[@"hangDiagnosticCount"] = @(hangDiagnostics.count);
    envelope[@"summaries"] = summaries;

    NSDictionary *payloadDictionary = payload.dictionaryRepresentation;
    if (payloadDictionary != nil)
        envelope[@"payload"] = payloadDictionary;

    NSURL *directoryURL = MetricKitDiagnosticsDirectoryURL();
    if (directoryURL == nil) {
        NSLog(@"MetricKit failed to resolve diagnostics directory");
        return;
    }

    NSError *directoryError = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:directoryURL
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:&directoryError]) {
        NSLog(@"MetricKit failed to create diagnostics directory: %@", directoryError);
        return;
    }

    NSString *beginString = MetricKitISO8601StringFromDate(timeStampBegin) ?: @"unknown";
    NSString *safeBeginString = [[beginString stringByReplacingOccurrencesOfString:@":" withString:@"-"]
                                 stringByReplacingOccurrencesOfString:@"/" withString:@"-"];
    NSString *filename = [NSString stringWithFormat:@"diagnostic-%@-%lu-%@.json",
                          safeBeginString, (unsigned long) index, NSUUID.UUID.UUIDString];
    NSURL *fileURL = [directoryURL URLByAppendingPathComponent:filename isDirectory:NO];

    NSError *jsonError = nil;
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:envelope
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:&jsonError];
    if (jsonData == nil) {
        NSData *rawPayloadData = payload.JSONRepresentation;
        if (rawPayloadData != nil) {
            jsonData = rawPayloadData;
        } else {
            NSLog(@"MetricKit failed to serialize diagnostic payload: %@", jsonError);
            return;
        }
    }

    NSError *writeError = nil;
    if (![jsonData writeToURL:fileURL options:NSDataWritingAtomic error:&writeError]) {
        NSLog(@"MetricKit failed to persist diagnostic payload to %@: %@", fileURL.path, writeError);
        return;
    }

    NSLog(@"MetricKit wrote diagnostic payload to %@", fileURL.path);
}

@end

static bool PushInitTaskAsCurrent(struct task **previousCurrent) {
    *previousCurrent = current;

    complex_lockt(&pids_lock, 0);
    struct task *init = pid_get_task(1);
    if (init != NULL) {
        task_ref_cnt_mod(init, 1);
    }
    unlock(&pids_lock);

    current = init;
    return init != NULL;
}

static void PopCurrentTask(struct task *previousCurrent) {
    struct task *borrowedCurrent = current;
    current = previousCurrent;
    if (borrowedCurrent != NULL) {
        task_ref_cnt_mod(borrowedCurrent, -1);
    }
}

@implementation AppDelegate

static UIViewController *CreateRootSelectionViewController(void) {
    UIViewController *rootsViewController = [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:rootsViewController];
    return navigationController;
}

static TerminalViewController *CreateTerminalViewController(void) {
    UIViewController *viewController = [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    return [viewController isKindOfClass:TerminalViewController.class] ? (TerminalViewController *) viewController : nil;
}

+ (intptr_t)ensureBooted {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        AppDelegate *delegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
        if ([delegate isKindOfClass:AppDelegate.class]) {
            bootError = [delegate boot];
        } else {
            bootError = _ESRCH;
        }
    });
    return bootError;
}

- (intptr_t)boot {
#if !ISH_LINUX
    NSString *defaultRoot = Roots.instance.defaultRoot;
    if (defaultRoot == nil)
        return _ENOENT;

    NSURL *root = [Roots.instance rootUrl:defaultRoot];

    intptr_t err = mount_root(&fakefs, [root URLByAppendingPathComponent:@"data"].fileSystemRepresentation);
    if (err < 0)
        return err;

    fs_register(&iosfs);
    fs_register(&iosfs_unsafe);

    // need to do this first so that we can have a valid current for the generic_mknod calls
    err = become_first_process();
    if (err < 0)
        return err;

    FsInitialize();

    // Repair or recreate the core device nodes the app owns. This keeps older
    // roots working when a device major/minor changes in a later app build.
    EnsureCharacterDevice("/dev/tty1", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 1));
    EnsureCharacterDevice("/dev/tty2", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 2));
    EnsureCharacterDevice("/dev/tty3", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 3));
    EnsureCharacterDevice("/dev/tty4", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 4));
    EnsureCharacterDevice("/dev/tty5", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 5));
    EnsureCharacterDevice("/dev/tty6", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 6));
    EnsureCharacterDevice("/dev/tty7", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 7));

    EnsureCharacterDevice("/dev/tty", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
    EnsureCharacterDevice("/dev/console", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
    EnsureCharacterDevice("/dev/ptmx", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));

    EnsureCharacterDevice("/dev/null", S_IFCHR|0777, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
    EnsureCharacterDevice("/dev/zero", S_IFCHR|0777, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
    EnsureCharacterDevice("/dev/full", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
    EnsureCharacterDevice("/dev/random", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
    EnsureCharacterDevice("/dev/urandom", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));
    
    generic_mkdirat(AT_PWD, "/dev/pts", 0755);
    
    // Permissions on / have been broken for a while, let's fix them
    generic_setattrat(AT_PWD, "/", (struct attr) {.type = attr_mode, .mode = 0755}, false);
    
    // mv current /run to /tmp/run-old/[timestamp], create new /run and link to /var/run
    generic_mkdirat(AT_PWD, "/tmp/old-run", 0755);
    const char *rename = getRenameRunDirString();
    generic_renameat(AT_PWD, "/run", AT_PWD, rename);
    generic_mkdirat(AT_PWD, "/run", 0755);
    generic_unlinkat(AT_PWD, "/var/run");
    generic_symlinkat("/run", AT_PWD, "/var/run");
    
    // Create directories/links to simulate /sys stuff for battery monitoring
    generic_mkdirat(AT_PWD, "/sys/class", 0755);
    generic_mkdirat(AT_PWD, "/sys/class/power_supply", 0755);
    generic_mkdirat(AT_PWD, "/sys/class/power_supply/BAT0", 0755);
    generic_mkdirat(AT_PWD, "/AOK", 0555);
    generic_symlinkat("/proc/ish/BAT0_capacity", AT_PWD, "/sys/class/power_supply/BAT0/capacity");
    generic_symlinkat("/proc/ish/BAT0_status", AT_PWD, "/sys/class/power_supply/BAT0/status");
    
    
    
    // Register clipboard device driver and create device node for it
    err = dyn_dev_register(&clipboard_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR);
    if (err != 0) {
        return err;
    }
    EnsureCharacterDevice("/dev/clipboard", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR));
    
    err = dyn_dev_register(&location_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_LOCATION_MINOR);
    if (err != 0)
        return err;
    EnsureCharacterDevice("/dev/location", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_LOCATION_MINOR));

    err = dyn_dev_register((struct dev_ops *) &audio_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_DSP_MINOR);
    if (err != 0)
        return err;
    EnsureCharacterDevice("/dev/dsp", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_DSP_MINOR));
    
    // Emulate a RTC, read time only
    err = dyn_dev_register(&rtc_dev, DEV_CHAR, DEV_RTC_MAJOR, DEV_RTC_MINOR);
    if (err != 0)
        return err;
    EnsureCharacterDevice("/dev/rtc0", S_IFCHR|0666, dev_make(DEV_RTC_MAJOR, DEV_RTC_MINOR));
    EnsureSymlink("/dev/rtc", "/dev/rtc0");

    do_mount(&aokfs, NSBundle.mainBundle.resourcePath.UTF8String, "/AOK", "", MS_READONLY_);
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);

    iosfs_init(); // let it mount any filesystems from user defaults

    [self scheduleDnsRefresh:@"boot"];
    
    exit_hook = ios_handle_exit;
    die_handler = ios_handle_die;
#if !TARGET_OS_SIMULATOR
    NSString *sockTmp = [NSTemporaryDirectory() stringByAppendingString:@"ishsock"];
    sock_tmp_prefix = strdup(sockTmp.UTF8String);
#endif
    
    tty_drivers[TTY_CONSOLE_MAJOR] = &ios_console_driver;
    set_console_device(TTY_CONSOLE_MAJOR, 1);
    err = create_stdio("/dev/console", TTY_CONSOLE_MAJOR, 1);
    if (err < 0)
        return err;
    
    NSArray<NSString *> *command;
    command = UserPreferences.shared.bootCommand;
    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "TERM=xterm-256color\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0)
        return err;
    task_start(current);

#else
    // On first launch, this will trigger the import of the default root. Make sure to do this before entering the kernel, because it needs to run something on the main thread, and that would deadlock.
    [Roots instance];
    NSArray<NSString *> *args = @[];
    actuate_kernel([args componentsJoinedByString:@" "].UTF8String);
#endif
    
    return 0;
}

#if ISH_LINUX
const char *DefaultRootPath() {
    return [Roots.instance rootUrl:Roots.instance.defaultRoot].fileSystemRepresentation;
}

static BOOL GuestHostnameFromFile(char *hostname, size_t size) {
    if (size == 0)
        return NO;

    ssize_t len = linux_read_file("/etc/hostname", hostname, size - 1);
    if (len <= 0)
        return NO;

    hostname[len] = '\0';
    while (len > 0 && isspace((unsigned char) hostname[len - 1])) {
        hostname[--len] = '\0';
    }
    size_t start = 0;
    while (hostname[start] != '\0' && isspace((unsigned char) hostname[start])) {
        start++;
    }
    if (start != 0) {
        memmove(hostname, hostname + start, len - start + 1);
    }
    return hostname[0] != '\0';
}

static void EnsureGuestHostsEntry(const char *hostname) {
    if (hostname == NULL || hostname[0] == '\0')
        return;

    struct task *previousCurrent;
    if (!PushInitTaskAsCurrent(&previousCurrent))
        return;

    char hosts[8192];
    ssize_t len = linux_read_file("/etc/hosts", hosts, sizeof(hosts) - 1);
    NSMutableString *updatedHosts = nil;
    BOOL hasHostname = NO;

    if (len >= 0) {
        hosts[len] = '\0';
        NSString *existingHosts = [[NSString alloc] initWithBytes:hosts
                                                           length:len
                                                         encoding:NSUTF8StringEncoding];
        if (existingHosts == nil) {
            existingHosts = [[NSString alloc] initWithCString:hosts encoding:NSISOLatin1StringEncoding];
        }
        if (existingHosts != nil) {
            NSArray<NSString *> *lines = [existingHosts componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
            for (NSString *line in lines) {
                NSString *content = [[line componentsSeparatedByString:@"#"] firstObject];
                NSArray<NSString *> *fields = [content componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                for (NSString *field in fields) {
                    if ([field length] == 0)
                        continue;
                    if ([field isEqualToString:@(hostname)]) {
                        hasHostname = YES;
                        break;
                    }
                }
                if (hasHostname)
                    break;
            }
            updatedHosts = [existingHosts mutableCopy];
        }
    }

    if (hasHostname)
        goto out;

    if (updatedHosts == nil) {
        updatedHosts = [NSMutableString stringWithString:@"127.0.0.1\tlocalhost\n"];
    } else if (![updatedHosts hasSuffix:@"\n"]) {
        [updatedHosts appendString:@"\n"];
    }
    [updatedHosts appendFormat:@"127.0.1.1\t%s\n", hostname];

    struct fd *fd = generic_open("/etc/hosts", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
    if (IS_ERR(fd)) {
        NSLog(@"failed to write /etc/hosts: %d", PTR_ERR(fd));
        goto out;
    }
    fd->ops->write(fd, updatedHosts.UTF8String, [updatedHosts lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    fd_close(fd);

out:
    PopCurrentTask(previousCurrent);
}

void SyncHostname(void) {
    async_do_in_workqueue(^{
        char hostname[256];
        if (!GuestHostnameFromFile(hostname, sizeof(hostname))) {
            if (gethostname(hostname, sizeof(hostname)) < 0)
                return;
        }
        linux_sethostname(hostname);
        EnsureGuestHostsEntry(hostname);
    });
}
#endif

- (void)configureDns {
#if !ISH_LINUX
    [self scheduleDnsRefresh:@"manual"];
#endif
}

- (void)scheduleDnsRefresh:(NSString *)reason {
#if !ISH_LINUX
    @synchronized (self) {
        if (self.dnsRefreshRunning) {
            self.dnsRefreshQueued = YES;
            NSLog(@"DNS refresh deferred while one is running (%@)", reason);
            return;
        }
        self.dnsRefreshRunning = YES;
        self.dnsRefreshQueued = NO;
    }

    NSString *reasonCopy = [reason copy] ?: @"unknown";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        [self performDnsRefresh:reasonCopy];
    });
#endif
}

- (void)performDnsRefresh:(NSString *)reason {
#if !ISH_LINUX
    double refreshStart = MetricKitNowSeconds();
    NSLog(@"DNS refresh begin (%@)", reason);

    struct __res_state res;
    double resolverInitStart = MetricKitNowSeconds();
    if (EXIT_SUCCESS != res_ninit(&res)) {
        NSLog(@"DNS refresh res_ninit failed after %.3fs (%@)", MetricKitNowSeconds() - resolverInitStart, reason);
        [self finishDnsRefreshAndRescheduleIfNeeded:reason];
        return;
    }
    NSLog(@"DNS refresh res_ninit completed in %.3fs (%@)", MetricKitNowSeconds() - resolverInitStart, reason);

    NSMutableString *resolvConf = [NSMutableString new];
    if (res.dnsrch[0] != NULL) {
        [resolvConf appendString:@"search"];
        for (int i = 0; res.dnsrch[i] != NULL; i++) {
            [resolvConf appendFormat:@" %s", res.dnsrch[i]];
        }
        [resolvConf appendString:@"\n"];
    }
    union res_sockaddr_union servers[NI_MAXSERV];
    double getServersStart = MetricKitNowSeconds();
    int serversFound = res_getservers(&res, servers, NI_MAXSERV);
    NSLog(@"DNS refresh res_getservers completed in %.3fs with %d server(s) (%@)",
          MetricKitNowSeconds() - getServersStart, serversFound, reason);
    char address[NI_MAXHOST];
    int usableServers = 0;
    for (int i = 0; i < serversFound; i ++) {
        union res_sockaddr_union s = servers[i];
        sa_family_t family = s.sin.sin_family;
        socklen_t sockaddrLen = s.sin.sin_len;
        if (family == AF_INET_) {
            if (sockaddrLen == 0)
                sockaddrLen = sizeof(s.sin);
        } else if (family == AF_INET6_) {
            if (IN6_IS_ADDR_LINKLOCAL(&s.sin6.sin6_addr)) {
                NSLog(@"DNS refresh skipping link-local IPv6 nameserver (%@)", reason);
                continue;
            }
            if (sockaddrLen == 0)
                sockaddrLen = sizeof(s.sin6);
        } else {
            continue;
        }
        int err = getnameinfo((struct sockaddr *) &s.sin, sockaddrLen,
                              address, sizeof(address),
                              NULL, 0, NI_NUMERICHOST);
        if (err != 0) {
            NSLog(@"DNS refresh getnameinfo failed for server %d: %s (%@)", i, gai_strerror(err), reason);
            continue;
        }
        [resolvConf appendFormat:@"nameserver %s\n", address];
        usableServers++;
    }

    if (usableServers == 0) {
        NSLog(@"DNS refresh found no usable nameservers, leaving existing /etc/resolv.conf in place (%@)", reason);
        res_nclose(&res);
        [self finishDnsRefreshAndRescheduleIfNeeded:reason];
        return;
    }

    struct task *previousCurrent;
    if (!PushInitTaskAsCurrent(&previousCurrent)) {
        NSLog(@"failed to resolve init task while updating DNS");
        res_nclose(&res);
        [self finishDnsRefreshAndRescheduleIfNeeded:reason];
        return;
    }

    struct fd *fd = generic_open("/etc/resolv.conf", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0666);
    if (IS_ERR(fd) && PTR_ERR(fd) == _ENOENT) {
        // Newer roots often ship /etc/resolv.conf as a symlink into /run.
        // If that target tree does not exist in the guest, replace the symlink
        // with a plain file so libc can still resolve names.
        generic_unlinkat(AT_PWD, "/etc/resolv.conf");
        fd = generic_open("/etc/resolv.conf", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0666);
    }
    if (!IS_ERR(fd)) {
        fd->ops->write(fd, resolvConf.UTF8String, [resolvConf lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        fd_close(fd);
    } else {
        NSLog(@"failed to write /etc/resolv.conf: %ld", (long) PTR_ERR(fd));
    }
    PopCurrentTask(previousCurrent);
    res_nclose(&res);
    NSLog(@"DNS refresh finished in %.3fs (%@)", MetricKitNowSeconds() - refreshStart, reason);
    [self finishDnsRefreshAndRescheduleIfNeeded:reason];
#endif
}

- (void)finishDnsRefreshAndRescheduleIfNeeded:(NSString *)reason {
#if !ISH_LINUX
    BOOL shouldReschedule = NO;
    @synchronized (self) {
        shouldReschedule = self.dnsRefreshQueued;
        self.dnsRefreshQueued = NO;
        self.dnsRefreshRunning = NO;
    }
    if (shouldReschedule) {
        NSString *nextReason = [NSString stringWithFormat:@"%@-coalesced", reason ?: @"dns"];
        [self scheduleDnsRefresh:nextReason];
    }
#endif
}

+ (intptr_t)bootError {
    return bootError;
}

+ (void)maybePresentStartupMessageOnViewController:(UIViewController *)vc {
    if ([NSUserDefaults.standardUserDefaults integerForKey:kSkipStartupMessage] >= 1)
        return;
    [NSUserDefaults.standardUserDefaults setInteger:1 forKey:kSkipStartupMessage];
}

- (BOOL)application:(UIApplication *)application willFinishLaunchingWithOptions:(NSDictionary<UIApplicationLaunchOptionsKey,id> *)launchOptions {
    [ISHDiagnosticsStore recordBreadcrumb:@"application.willFinishLaunching"
                                  details:launchOptions.count != 0 ? @{@"launchOptions": launchOptions.description} : nil];
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults boolForKey:@"hail mary"]) {
        [defaults removeObjectForKey:kPreferenceBootCommandKey];
        [defaults removeObjectForKey:kPreferenceLaunchCommandKey];
        [defaults setBool:NO forKey:@"hail mary"];
    }
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])
        return YES;

    [Roots instance];
    if (!Roots.instance.needsInitialRootSelection) {
        bootError = [AppDelegate ensureBooted];
    }

#if ISH_LINUX
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillEnterForegroundNotification object:UIApplication.sharedApplication queue:nil usingBlock:^(NSNotification * _Nonnull note) {
        SyncHostname();
    }];
    SyncHostname();
#endif

    return YES;
}

void NetworkReachabilityCallback(SCNetworkReachabilityRef target, SCNetworkReachabilityFlags flags, void *info) {
    AppDelegate *self = (__bridge AppDelegate *) info;
    [self scheduleDnsRefresh:@"reachability"];
}

static UINavigationController *CreateAboutNavigationController(BOOL recoveryMode, BOOL startInDiagnostics) {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
    aboutViewController.recoveryMode = recoveryMode;
    aboutViewController.startInDiagnostics = startInDiagnostics;
    return navigationController;
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    [ISHDiagnosticsStore recordBreadcrumb:@"application.didFinishLaunching"
                                  details:launchOptions.count != 0 ? @{@"launchOptions": launchOptions.description} : nil];
    // get the network permissions popup to appear on chinese devices
    [[NSURLSession.sharedSession dataTaskWithURL:[NSURL URLWithString:@"http://captive.apple.com"]] resume];

    if ([NSUserDefaults.standardUserDefaults boolForKey:@"FASTLANE_SNAPSHOT"])
        [UIView setAnimationsEnabled:NO];

#if !ISH_LINUX
    NSString *ishVersion = [NSString stringWithFormat:@"iSH-AOK %@ (%@)",
                         [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                         [NSBundle.mainBundle objectForInfoDictionaryKey:(NSString *) kCFBundleVersionKey]];
    extern const char *proc_ish_version;
    proc_ish_version = strdup(ishVersion.UTF8String);
    // this defaults key is set when taking app store screenshots
    extern const char *uname_hostname_override;
    extern bool doEnableMulticore;
    extern bool doEnableExtraLocking;
    extern pthread_mutex_t multicore_lock;
    extern pthread_mutex_t extra_lock;
    NSString *hostnameOverride = [NSUserDefaults.standardUserDefaults stringForKey:@"hostnameOverride"];
    if (hostnameOverride) {
        free((void *) uname_hostname_override);
        uname_hostname_override = strdup(hostnameOverride.UTF8String);
    }
#endif
    
    [UserPreferences.shared observe:@[@"shouldDisableDimming"] options:NSKeyValueObservingOptionInitial
                              owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            UIApplication.sharedApplication.idleTimerDisabled = UserPreferences.shared.shouldDisableDimming;
        });
    }];
    
    [UserPreferences.shared observe:@[@"shouldEnableMulticore"] options:NSKeyValueObservingOptionInitial
                              owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            doEnableMulticore = UserPreferences.shared.shouldEnableMulticore;
        });
    }];
    [UserPreferences.shared observe:@[@"shouldEnableExtraLocking"] options:NSKeyValueObservingOptionInitial
                              owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            doEnableExtraLocking = UserPreferences.shared.shouldEnableExtraLocking;
        });
    }];
    
        // This code is IPv4 and IPv6 aware: see https://developer.apple.com/library/archive/samplecode/Reachability/Listings/ReadMe_md.html.
    struct sockaddr_in address = {
        .sin_len = sizeof(address),
        .sin_family = AF_INET,
    };

    self.reachability = SCNetworkReachabilityCreateWithAddress(kCFAllocatorDefault, (struct sockaddr *) &address);
    SCNetworkReachabilityContext context = {
        .info = (__bridge void *) self,
    };
    SCNetworkReachabilitySetCallback(self.reachability, NetworkReachabilityCallback, &context);
    SCNetworkReachabilityScheduleWithRunLoop(self.reachability, CFRunLoopGetMain(), kCFRunLoopCommonModes);

    self.metricKitSubscriber = [ISHMetricKitSubscriber new];
    [self.metricKitSubscriber registerIfAvailable];

    if (self.window != nil) {
        // For iOS <13, where the app delegate owns the window instead of the scene
        if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
            self.window.rootViewController = CreateAboutNavigationController(NO, YES);
            return YES;
        }
        if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
            self.window.rootViewController = CreateAboutNavigationController(YES, NO);
            return YES;
        }
        if (Roots.instance.needsInitialRootSelection) {
            [NSNotificationCenter.defaultCenter removeObserver:self
                                                          name:RootsDidFinishInitialSelectionNotification
                                                        object:nil];
            [NSNotificationCenter.defaultCenter addObserver:self
                                                   selector:@selector(rootsDidFinishInitialSelection:)
                                                       name:RootsDidFinishInitialSelectionNotification
                                                     object:nil];
            self.window.rootViewController = CreateRootSelectionViewController();
            return YES;
        }
        TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
        currentTerminalViewController = vc;
        [vc startNewSession];
    }
    return YES;
}

- (void)continueAfterInitialRootImportIfNeeded {
    if (Roots.instance.needsInitialRootSelection)
        return;
    if (self.window == nil)
        return;
    if ([self.window.rootViewController isKindOfClass:TerminalViewController.class])
        return;

    self.waitingForInitialRootImport = NO;
    TerminalViewController *vc = CreateTerminalViewController();
    if (vc == nil)
        return;

    self.window.rootViewController = vc;
    currentTerminalViewController = vc;
    [vc startNewSession];
}

- (void)rootsDidFinishInitialSelection:(__unused NSNotification *)notification {
    [self continueAfterInitialRootImportIfNeeded];
}

- (void)application:(UIApplication *)application didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions API_AVAILABLE(ios(13.0)) {
    for (UISceneSession *sceneSession in sceneSessions) {
        NSString *terminalUUID = sceneSession.stateRestorationActivity.userInfo[@"TerminalUUID"];
        [[Terminal terminalWithUUID:[[NSUUID alloc] initWithUUIDString:terminalUUID]] destroy];
    }
}

- (void)dealloc {
    [self.metricKitSubscriber unregisterIfNeeded];
    if (self.reachability != NULL) {
        SCNetworkReachabilityUnscheduleFromRunLoop(self.reachability, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(self.reachability);
    }
}

- (void)exitApp {
    self.exiting = YES;
    id app = [UIApplication sharedApplication];
    [app suspend];
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
    [ISHDiagnosticsStore recordBreadcrumb:@"application.didEnterBackground"
                                  details:@{@"exiting": @(self.exiting)}];
    if (self.exiting)
        exit(0);
}

- (void)applicationWillEnterForeground:(UIApplication *)application {
    [ISHDiagnosticsStore recordBreadcrumb:@"application.willEnterForeground"];
}

@end

#if !ISH_LINUX
NSString *const ProcessExitedNotification = @"ProcessExitedNotification";
#else
NSString *const KernelPanicNotification = @"KernelPanicNotification";
#endif
