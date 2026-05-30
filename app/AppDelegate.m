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
#include <dlfcn.h>
#include <notify.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#import <SystemConfiguration/SystemConfiguration.h>
#if __has_include(<Network/Network.h>)
#import <Network/Network.h>
#endif
#import <MetricKit/MetricKit.h>
#import "AboutViewController.h"
#import "AppDelegate.h"
#import "AppGroup.h"
#import "CurrentRoot.h"
#import "Diagnostics.h"
#import "DiagnosticsBridge.h"
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
#import "WorkspaceViewController.h"
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/task.h"
#include "fs/dyndev.h"
#include "fs/devices.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "app/RTCDevice.h"
#include "util/sync.h"

#if ISH_LINUX
#import "LinuxInterop.h"
#endif

@class ISHMetricKitSubscriber;

@interface AppDelegate ()

@property BOOL exiting;
@property SCNetworkReachabilityRef reachability;
#if __has_include(<Network/Network.h>)
@property (strong, nonatomic) nw_path_monitor_t pathMonitor API_AVAILABLE(ios(12.0));
@property (strong, nonatomic) dispatch_queue_t pathMonitorQueue API_AVAILABLE(ios(12.0));
#endif
@property int dnsNotifyToken;
@property BOOL dnsNotifyRegistered;
@property (strong, nonatomic) ISHMetricKitSubscriber *metricKitSubscriber;
@property BOOL dnsRefreshQueued;
@property BOOL dnsRefreshRunning;
@property BOOL waitingForInitialRootImport;

@end

#if !ISH_LINUX
#pragma pack(push, 4)
typedef struct {
    struct in_addr address;
    struct in_addr mask;
} ish_dns_sortaddr_t;

typedef struct {
    char *domain;
    int32_t n_nameserver;
    struct sockaddr **nameserver;
    uint16_t port;
    int32_t n_search;
    char **search;
    int32_t n_sortaddr;
    ish_dns_sortaddr_t **sortaddr;
    char *options;
    uint32_t timeout;
    uint32_t search_order;
    uint32_t if_index;
    uint32_t flags;
    uint32_t reach_flags;
    uint32_t reserved[5];
} ish_dns_resolver_t;

typedef struct {
    int32_t n_resolver;
    ish_dns_resolver_t **resolver;
    int32_t n_scoped_resolver;
    ish_dns_resolver_t **scoped_resolver;
    uint32_t reserved[5];
} ish_dns_config_t;
#pragma pack(pop)

typedef ish_dns_config_t *(*ISHDnsConfigurationCopyFunc)(void);
typedef void (*ISHDnsConfigurationFreeFunc)(ish_dns_config_t *config);
typedef const char *(*ISHDnsConfigurationNotifyKeyFunc)(void);

static ISHDnsConfigurationCopyFunc ISHDnsConfigurationCopySymbol(void) {
    static ISHDnsConfigurationCopyFunc copyFunc;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        copyFunc = (ISHDnsConfigurationCopyFunc) dlsym(RTLD_DEFAULT, "dns_configuration_copy");
    });
    return copyFunc;
}

static ISHDnsConfigurationFreeFunc ISHDnsConfigurationFreeSymbol(void) {
    static ISHDnsConfigurationFreeFunc freeFunc;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        freeFunc = (ISHDnsConfigurationFreeFunc) dlsym(RTLD_DEFAULT, "dns_configuration_free");
    });
    return freeFunc;
}

static ISHDnsConfigurationNotifyKeyFunc ISHDnsConfigurationNotifyKeySymbol(void) {
    static ISHDnsConfigurationNotifyKeyFunc notifyKeyFunc;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        notifyKeyFunc = (ISHDnsConfigurationNotifyKeyFunc) dlsym(RTLD_DEFAULT, "dns_configuration_notify_key");
    });
    return notifyKeyFunc;
}

static ish_dns_resolver_t *ISHResolverPointerAt(ish_dns_resolver_t **resolvers, int index) {
    ish_dns_resolver_t *resolver = NULL;
    if (resolvers == NULL || index < 0)
        return NULL;
    memcpy(&resolver, resolvers + index, sizeof(resolver));
    return resolver;
}

static struct sockaddr *ISHNameserverPointerAt(struct sockaddr **nameservers, int index) {
    struct sockaddr *sockaddr = NULL;
    if (nameservers == NULL || index < 0)
        return NULL;
    memcpy(&sockaddr, nameservers + index, sizeof(sockaddr));
    return sockaddr;
}

static NSString *ISHResolvConfFromDnsConfiguration(void) {
    ISHDnsConfigurationCopyFunc copyFunc = ISHDnsConfigurationCopySymbol();
    ISHDnsConfigurationFreeFunc freeFunc = ISHDnsConfigurationFreeSymbol();
    if (copyFunc == NULL || freeFunc == NULL)
        return nil;

    ish_dns_config_t *config = copyFunc();
    if (config == NULL)
        return nil;

    NSMutableString *resolvConf = [NSMutableString new];
    NSMutableOrderedSet<NSString *> *uniqueServers = [NSMutableOrderedSet orderedSet];
    BOOL wroteSearch = NO;

    for (int resolverIndex = 0; resolverIndex < config->n_resolver; resolverIndex++) {
        ish_dns_resolver_t *resolver = ISHResolverPointerAt(config->resolver, resolverIndex);
        if (resolver == NULL || resolver->n_nameserver <= 0)
            continue;
        if (resolver->options != NULL && strcmp(resolver->options, "mdns") == 0)
            continue;

        if (!wroteSearch && resolver->n_search > 0 && resolver->search != NULL) {
            for (int searchIndex = 0; searchIndex < resolver->n_search; searchIndex++) {
                char *searchDomain = resolver->search[searchIndex];
                if (searchDomain == NULL || searchDomain[0] == '\0')
                    continue;
                if (!wroteSearch) {
                    [resolvConf appendString:@"search"];
                    wroteSearch = YES;
                }
                [resolvConf appendFormat:@" %s", searchDomain];
            }
            if (wroteSearch)
                [resolvConf appendString:@"\n"];
        }

        char address[NI_MAXHOST];
        for (int nameserverIndex = 0; nameserverIndex < resolver->n_nameserver; nameserverIndex++) {
            struct sockaddr *sockaddr = ISHNameserverPointerAt(resolver->nameserver, nameserverIndex);
            if (sockaddr == NULL)
                continue;
            sa_family_t family = sockaddr->sa_family;
            socklen_t sockaddrLen = 0;
            if (family == AF_INET_) {
                sockaddrLen = sizeof(struct sockaddr_in);
            } else if (family == AF_INET6_) {
                struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *) sockaddr;
                if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr))
                    continue;
                sockaddrLen = sizeof(struct sockaddr_in6);
            } else {
                continue;
            }
            int err = getnameinfo(sockaddr, sockaddrLen,
                                  address, sizeof(address),
                                  NULL, 0, NI_NUMERICHOST);
            if (err != 0)
                continue;
            NSString *server = [NSString stringWithUTF8String:address];
            if (server.length != 0)
                [uniqueServers addObject:server];
        }
    }

    for (NSString *server in uniqueServers) {
        [resolvConf appendFormat:@"nameserver %@\n", server];
    }

    if (uniqueServers.count == 0)
        resolvConf = nil;

    freeFunc(config);
    return resolvConf;
}

static NSString *ISHDnsBreadcrumbSummary(NSString *source, NSString *reason, NSString *resolvConf) {
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    if (source.length != 0)
        [parts addObject:[NSString stringWithFormat:@"source=%@", source]];
    if (reason.length != 0)
        [parts addObject:[NSString stringWithFormat:@"reason=%@", reason]];
    NSString *trimmed = [resolvConf stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (trimmed.length != 0) {
        NSString *singleLine = [[trimmed componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]] componentsJoinedByString:@" | "];
        [parts addObject:[NSString stringWithFormat:@"conf=%@", singleLine]];
    }
    return [parts componentsJoinedByString:@"; "];
}
#endif

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
    pid_t ppid = task->parent != NULL ? task->parent->pid : -1;
    pid_t tgid = task->tgid;
    char comm[sizeof(task->comm)];
    snprintf(comm, sizeof(comm), "%s", task->comm);
    ISHDiagnosticsRecordGuestExitSyncDetailed(pid, ppid, tgid, comm, code);
    NSString *commString = [NSString stringWithUTF8String:comm] ?: @"";

    dispatch_async(dispatch_get_main_queue(), ^{
        NSString *decoded = nil;
        if ((code & 0xff) == 0x7f) {
            decoded = [NSString stringWithFormat:@"stopped sig=%d status=%#x", (code >> 8) & 0xff, code];
        } else if ((code & 0x7f) == 0) {
            decoded = [NSString stringWithFormat:@"exited code=%d status=%#x", (code >> 8) & 0xff, code];
        } else {
            decoded = [NSString stringWithFormat:@"signaled sig=%d core=%d status=%#x",
                       code & 0x7f, (code & 0x80) != 0, code];
        }
        [ISHDiagnosticsStore recordBreadcrumb:@"process.exit"
                                      details:@{@"pid": @(pid),
                                                @"ppid": @(ppid),
                                                @"tgid": @(tgid),
                                                @"comm": commString,
                                                @"code": @(code),
                                                @"decoded": decoded ?: @""}];
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
    ISHDiagnosticsRecordGuestFatalSync("die", msg, NULL);
}
#elif ISH_LINUX
void ReportPanic(const char *message) {
    NSDictionary *userInfo = message != NULL ? @{@"message": @(message)} : nil;
    [NSNotificationCenter.defaultCenter postNotificationName:KernelPanicNotification
                                                      object:nil
                                                    userInfo:userInfo];
}
#endif

static intptr_t bootError;
static NSString *bootFailureTitle;
static NSString *bootFailureMessage;
static NSString *bootFailureOverlayText;
static NSDictionary<NSString *, id> *bootFailureDetails;
static BOOL bootUsesConsoleSessionFallback;
static BOOL bootUsesNativeFakeInit;
static NSString *const kSkipStartupMessage = @"Skip Startup Message";
static NSString *const kMetricKitDiagnosticsDirectory = @"MetricKitDiagnostics";
NSString *const ISHDiagnosticsStoreDidUpdateNotification = @"ISHDiagnosticsStoreDidUpdateNotification";
static NSString *const kDiagnosticsDirectory = @"Diagnostics";
static NSString *const kDiagnosticsBreadcrumbsFile = @"breadcrumbs.json";
static NSString *const kDiagnosticsLaunchJournalFile = @"launch-journal.json";
static NSString *const kDiagnosticsGuestFatalFile = @"guest-fatal-event.json";
static NSString *const kDiagnosticsGuestExitsFile = @"guest-exits.json";

typedef struct {
    intptr_t code;
    const char *name;
    const char *description;
} ISHErrnoInfo;

static const ISHErrnoInfo kISHErrnoInfos[] = {
    {_EPERM, "EPERM", "Operation not permitted"},
    {_ENOENT, "ENOENT", "No such file or directory"},
    {_ESRCH, "ESRCH", "No such process"},
    {_EINTR, "EINTR", "Interrupted system call"},
    {_EIO, "EIO", "I/O error"},
    {_ENXIO, "ENXIO", "No such device or address"},
    {_E2BIG, "E2BIG", "Argument list too long"},
    {_ENOEXEC, "ENOEXEC", "Exec format error"},
    {_EBADF, "EBADF", "Bad file descriptor"},
    {_ECHILD, "ECHILD", "No child processes"},
    {_EAGAIN, "EAGAIN", "Resource temporarily unavailable"},
    {_ENOMEM, "ENOMEM", "Out of memory"},
    {_EACCES, "EACCES", "Permission denied"},
    {_EFAULT, "EFAULT", "Bad address"},
    {_ENOTBLK, "ENOTBLK", "Block device required"},
    {_EBUSY, "EBUSY", "Device or resource busy"},
    {_EEXIST, "EEXIST", "File exists"},
    {_EXDEV, "EXDEV", "Cross-device link"},
    {_ENODEV, "ENODEV", "No such device"},
    {_ENOTDIR, "ENOTDIR", "Not a directory"},
    {_EISDIR, "EISDIR", "Is a directory"},
    {_EINVAL, "EINVAL", "Invalid argument"},
    {_ENFILE, "ENFILE", "File table overflow"},
    {_EMFILE, "EMFILE", "Too many open files"},
    {_ENOTTY, "ENOTTY", "Inappropriate ioctl for device"},
    {_ETXTBSY, "ETXTBSY", "Text file busy"},
    {_EFBIG, "EFBIG", "File too large"},
    {_ENOSPC, "ENOSPC", "No space left on device"},
    {_ESPIPE, "ESPIPE", "Illegal seek"},
    {_EROFS, "EROFS", "Read-only file system"},
    {_EMLINK, "EMLINK", "Too many links"},
    {_EPIPE, "EPIPE", "Broken pipe"},
    {_EDOM, "EDOM", "Numerical argument out of domain"},
    {_ERANGE, "ERANGE", "Numerical result out of range"},
    {_EDEADLK, "EDEADLK", "Resource deadlock would occur"},
    {_ENAMETOOLONG, "ENAMETOOLONG", "File name too long"},
    {_ENOLCK, "ENOLCK", "No record locks available"},
    {_ENOSYS, "ENOSYS", "Function not implemented"},
    {_ENOTEMPTY, "ENOTEMPTY", "Directory not empty"},
    {_ELOOP, "ELOOP", "Too many levels of symbolic links"},
    {_EBFONT, "EBFONT", "Bad font file format"},
    {_ENOSTR, "ENOSTR", "Device is not a stream"},
    {_ENODATA, "ENODATA", "No data available"},
    {_ETIME, "ETIME", "Timer expired"},
    {_ENOSR, "ENOSR", "Out of stream resources"},
    {_ENONET, "ENONET", "Machine is not on the network"},
    {_ENOPKG, "ENOPKG", "Package not installed"},
    {_EREMOTE, "EREMOTE", "Object is remote"},
    {_ENOLINK, "ENOLINK", "Link has been severed"},
    {_EADV, "EADV", "Advertise error"},
    {_ESRMNT, "ESRMNT", "Srmount error"},
    {_ECOMM, "ECOMM", "Communication error on send"},
    {_EPROTO, "EPROTO", "Protocol error"},
    {_EMULTIHOP, "EMULTIHOP", "Multihop attempted"},
    {_EDOTDOT, "EDOTDOT", "RFS-specific error"},
    {_EBADMSG, "EBADMSG", "Bad message"},
    {_EOVERFLOW, "EOVERFLOW", "Value too large for defined data type"},
    {_ENOTUNIQ, "ENOTUNIQ", "Name not unique on network"},
    {_EBADFD, "EBADFD", "File descriptor in bad state"},
    {_EREMCHG, "EREMCHG", "Remote address changed"},
    {_ELIBACC, "ELIBACC", "Cannot access a needed shared library"},
    {_ELIBBAD, "ELIBBAD", "Accessing a corrupted shared library"},
    {_ELIBSCN, "ELIBSCN", "Library section is corrupted"},
    {_ELIBMAX, "ELIBMAX", "Too many shared libraries"},
    {_ELIBEXEC, "ELIBEXEC", "Cannot execute a shared library directly"},
    {_EILSEQ, "EILSEQ", "Illegal byte sequence"},
    {_ERESTART, "ERESTART", "Interrupted system call should be restarted"},
    {_ESTRPIPE, "ESTRPIPE", "Streams pipe error"},
    {_EUSERS, "EUSERS", "Too many users"},
    {_ENOTSOCK, "ENOTSOCK", "Socket operation on non-socket"},
    {_EDESTADDRREQ, "EDESTADDRREQ", "Destination address required"},
    {_EMSGSIZE, "EMSGSIZE", "Message too long"},
    {_EPROTOTYPE, "EPROTOTYPE", "Protocol wrong type for socket"},
    {_ENOPROTOOPT, "ENOPROTOOPT", "Protocol not available"},
    {_EPROTONOSUPPORT, "EPROTONOSUPPORT", "Protocol not supported"},
    {_ESOCKTNOSUPPORT, "ESOCKTNOSUPPORT", "Socket type not supported"},
    {_EOPNOTSUPP, "EOPNOTSUPP", "Operation not supported"},
    {_EPFNOSUPPORT, "EPFNOSUPPORT", "Protocol family not supported"},
    {_EAFNOSUPPORT, "EAFNOSUPPORT", "Address family not supported by protocol"},
    {_EADDRINUSE, "EADDRINUSE", "Address already in use"},
    {_EADDRNOTAVAIL, "EADDRNOTAVAIL", "Cannot assign requested address"},
    {_ENETDOWN, "ENETDOWN", "Network is down"},
    {_ENETUNREACH, "ENETUNREACH", "Network is unreachable"},
    {_ENETRESET, "ENETRESET", "Network dropped connection on reset"},
    {_ECONNABORTED, "ECONNABORTED", "Software caused connection abort"},
    {_ECONNRESET, "ECONNRESET", "Connection reset by peer"},
    {_ENOBUFS, "ENOBUFS", "No buffer space available"},
    {_EISCONN, "EISCONN", "Transport endpoint is already connected"},
    {_ENOTCONN, "ENOTCONN", "Transport endpoint is not connected"},
    {_ESHUTDOWN, "ESHUTDOWN", "Cannot send after transport endpoint shutdown"},
    {_ETOOMANYREFS, "ETOOMANYREFS", "Too many references"},
    {_ETIMEDOUT, "ETIMEDOUT", "Connection timed out"},
    {_ECONNREFUSED, "ECONNREFUSED", "Connection refused"},
    {_EHOSTDOWN, "EHOSTDOWN", "Host is down"},
    {_EHOSTUNREACH, "EHOSTUNREACH", "No route to host"},
    {_EALREADY, "EALREADY", "Operation already in progress"},
    {_EINPROGRESS, "EINPROGRESS", "Operation now in progress"},
    {_ESTALE, "ESTALE", "Stale file handle"},
    {_EUCLEAN, "EUCLEAN", "Structure needs cleaning"},
    {_ENOTNAM, "ENOTNAM", "Not a named type file"},
    {_ENAVAIL, "ENAVAIL", "No semaphores available"},
    {_EISNAM, "EISNAM", "Is a named type file"},
    {_EREMOTEIO, "EREMOTEIO", "Remote I/O error"},
    {_EDQUOT, "EDQUOT", "Quota exceeded"},
    {_ECANCELED, "ECANCELED", "Operation canceled"},
};

static const ISHErrnoInfo *ISHInfoForErrno(intptr_t err) {
    for (size_t i = 0; i < sizeof(kISHErrnoInfos) / sizeof(kISHErrnoInfos[0]); i++) {
        if (kISHErrnoInfos[i].code == err)
            return &kISHErrnoInfos[i];
    }
    return NULL;
}

static NSString *ISHDescriptionForErrno(intptr_t err) {
    const ISHErrnoInfo *info = ISHInfoForErrno(err);
    if (info != NULL) {
        return [NSString stringWithFormat:@"%s (%ld): %s",
                info->name, (long) err, info->description];
    }

    NSInteger posixCode = err < 0 ? (NSInteger) -err : (NSInteger) err;
    NSError *posixError = [NSError errorWithDomain:NSPOSIXErrorDomain code:posixCode userInfo:nil];
    return [NSString stringWithFormat:@"errno %ld: %@",
            (long) err, posixError.localizedDescription ?: @"unknown error"];
}

static NSString *BootFailureSentence(NSString *text) {
    if (text.length == 0)
        return @"Boot failed.";
    unichar last = [text characterAtIndex:text.length - 1];
    if (last == '.' || last == '!' || last == '?')
        return text;
    return [text stringByAppendingString:@"."];
}

static NSString *BootFailureMessage(NSString *reason, intptr_t err, NSDictionary<NSString *, id> *details, NSString *recovery) {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    if (reason.length != 0) {
        [lines addObject:reason];
        [lines addObject:@""];
    }

    NSString *root = details[@"root"];
    NSString *guestABI = details[@"guestABI"];
    NSString *path = details[@"path"];
    NSString *command = details[@"command"];
    if (root.length != 0)
        [lines addObject:[NSString stringWithFormat:@"Root: %@", root]];
    if (guestABI.length != 0)
        [lines addObject:[NSString stringWithFormat:@"Guest ABI: %@", guestABI]];
    if (path.length != 0)
        [lines addObject:[NSString stringWithFormat:@"Path: %@", path]];
    if (command.length != 0)
        [lines addObject:[NSString stringWithFormat:@"Command: %@", command]];
    [lines addObject:[NSString stringWithFormat:@"Error: %@", ISHDescriptionForErrno(err)]];

    if (recovery.length != 0) {
        [lines addObject:@""];
        [lines addObject:recovery];
    }
    return [lines componentsJoinedByString:@"\n"];
}

static NSMutableDictionary<NSString *, id> *BootFailureDetails(NSString *stage,
                                                               intptr_t err,
                                                               NSString *title,
                                                               NSString *reason,
                                                               NSString *recovery,
                                                               NSDictionary<NSString *, id> *details) {
    NSMutableDictionary<NSString *, id> *result = [NSMutableDictionary dictionaryWithDictionary:details ?: @{}];
    if (stage.length != 0)
        result[@"stage"] = stage;
    result[@"error"] = @(err);
    result[@"errorDescription"] = ISHDescriptionForErrno(err);
    if (title.length != 0)
        result[@"title"] = title;
    if (reason.length != 0)
        result[@"reason"] = reason;
    if (recovery.length != 0)
        result[@"recovery"] = recovery;
    return result;
}

static intptr_t RecordBootFailure(intptr_t err,
                                  NSString *stage,
                                  NSString *title,
                                  NSString *reason,
                                  NSString *recovery,
                                  NSDictionary<NSString *, id> *details) {
    if (err > 0)
        err = -err;
    NSString *safeStage = stage.length != 0 ? stage : @"boot.failed";
    NSString *safeTitle = title.length != 0 ? title : @"Boot failed";
    NSDictionary<NSString *, id> *safeDetails = BootFailureDetails(safeStage, err, safeTitle, reason, recovery, details);
    bootFailureTitle = safeTitle;
    bootFailureMessage = BootFailureMessage(reason, err, safeDetails, recovery);
    bootFailureOverlayText = BootFailureSentence(safeTitle);
    bootFailureDetails = safeDetails;
    [ISHDiagnosticsStore recordLaunchStage:safeStage details:safeDetails];
    NSLog(@"boot failed at %@: %@", safeStage, bootFailureMessage);
    return err;
}

static NSString *BootMountRecovery(intptr_t err) {
    if (err == _EINVAL) {
        return @"The filesystem metadata database may be incompatible or corrupt. Choose another filesystem or reimport this one.";
    }
    if (err == _EACCES || err == _EPERM) {
        return @"iOS denied access to the filesystem files. Restart the app; if it still fails, choose another filesystem or reimport this one.";
    }
    if (err == _ENOSPC) {
        return @"Free storage space on the device, then restart iSH-AOK.";
    }
    return @"Choose another filesystem or reimport this one.";
}

static NSString *BootExecRecovery(intptr_t err, NSString *guestABI) {
    if (err == _ENOENT) {
        return @"The configured boot command is missing inside the root. Check Settings -> Boot Command or reimport the filesystem.";
    }
    if (err == _EACCES || err == _EPERM) {
        return @"The configured boot command exists but is not executable. Fix its permissions or choose a different boot command.";
    }
    if (err == _ENOEXEC) {
        if ([guestABI isEqualToString:@"amd64"]) {
            return @"This root is marked x86_64/amd64. amd64 support is still experimental, so early exec or decode failures are expected with some binaries.";
        }
        return @"The configured boot command is not a supported Linux executable or script. Check Settings -> Boot Command.";
    }
    if (err == _ENOMEM) {
        return @"The device did not have enough memory to start init. Close other apps and restart iSH-AOK.";
    }
    return @"Check Settings -> Boot Command, or choose/reimport the filesystem.";
}

static BOOL BootCommandIsDefaultInit(NSArray<NSString *> *command) {
    return command.count > 0 && [command[0] isEqualToString:@"/sbin/init"];
}

static BOOL BootCommandIsInteractiveConsoleShellFallback(NSArray<NSString *> *command) {
    if (command.count == 0)
        return NO;

    NSString *name = command.firstObject.lastPathComponent;
    NSSet<NSString *> *shells = [NSSet setWithObjects:@"ash", @"bash", @"dash", @"ksh", @"sh", @"zsh", nil];
    if ([shells containsObject:name])
        return YES;

    if ([name isEqualToString:@"busybox"] && command.count > 1) {
        NSString *applet = command[1].lastPathComponent;
        return [shells containsObject:applet];
    }

    return NO;
}

static BOOL BootExecutableExists(NSString *path, intptr_t *errOut) {
    struct task *previousCurrent = NULL;
    BOOL borrowedInit = [AppDelegate pushUsableInitTaskAsCurrent:&previousCurrent];
    if (!borrowedInit) {
        [AppDelegate popCurrentTask:previousCurrent];
        if (errOut != NULL)
            *errOut = _ENOENT;
        return NO;
    }
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path.UTF8String, &stat, 0);
    [AppDelegate popCurrentTask:previousCurrent];
    if (err < 0) {
        if (errOut != NULL)
            *errOut = err;
        return NO;
    }
    if (!S_ISREG(stat.mode)) {
        if (errOut != NULL)
            *errOut = _EACCES;
        return NO;
    }
    if (!(stat.mode & 0111)) {
        if (errOut != NULL)
            *errOut = _EACCES;
        return NO;
    }
    if (errOut != NULL)
        *errOut = 0;
    return YES;
}

static NSArray<NSString *> *BootCommandFallbackCandidatesDescription(NSArray<NSArray<NSString *> *> *candidates) {
    NSMutableArray<NSString *> *descriptions = [NSMutableArray arrayWithCapacity:candidates.count];
    for (NSArray<NSString *> *candidate in candidates) {
        [descriptions addObject:[candidate componentsJoinedByString:@" "]];
    }
    return descriptions;
}

static int EnsureDirectory(const char *path, mode_t_ mode);
static int EnsureRegularFileIfMissing(const char *path, const char *contents, mode_t_ mode);
static int EnsureSymlink(const char *path, const char *target);

static NSData *BootEnvironmentForCommand(NSString *commandPath) {
    NSString *shell = commandPath.length != 0 ? commandPath : @"/bin/sh";
    NSArray<NSString *> *entries = @[
        @"TERM=xterm-256color",
        @"HOME=/root",
        @"USER=root",
        @"LOGNAME=root",
        [NSString stringWithFormat:@"SHELL=%@", shell],
        @"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        @"PS1=# ",
        @"COLUMNS=80",
        @"LINES=24",
    ];

    NSMutableData *env = [NSMutableData data];
    for (NSString *entry in entries) {
        NSData *bytes = [entry dataUsingEncoding:NSUTF8StringEncoding];
        if (bytes.length != 0)
            [env appendData:bytes];
        uint8_t nul = 0;
        [env appendBytes:&nul length:1];
    }
    uint8_t terminator = 0;
    [env appendBytes:&terminator length:1];
    return env;
}

static NSArray<NSString *> *FakeInitLoginShellArgvForCommand(NSArray<NSString *> *command) {
    if (command.count == 0)
        return @[];

    NSString *name = command.firstObject.lastPathComponent;
    if ([name isEqualToString:@"busybox"]) {
        NSString *applet = command.count > 1 ? command[1].lastPathComponent : @"sh";
        return @[applet.length != 0 ? applet : @"sh", @"-l", @"-i"];
    }

    if ([name isEqualToString:@"sh"] || [name isEqualToString:@"ash"] || [name isEqualToString:@"bash"] ||
        [name isEqualToString:@"dash"] || [name isEqualToString:@"ksh"] || [name isEqualToString:@"zsh"]) {
        return @[[@"-" stringByAppendingString:name], @"-i"];
    }

    return command;
}

static void FakeInitPrepareGuestRoot(void) {
    EnsureDirectory("/dev", 0755);
    EnsureDirectory("/dev/pts", 0755);
    EnsureDirectory("/etc", 0755);
    EnsureDirectory("/proc", 0555);
    EnsureDirectory("/root", 0700);
    EnsureDirectory("/tmp", 01777);
    EnsureDirectory("/run", 0755);
    EnsureDirectory("/run/lock", 0755);
    EnsureDirectory("/var", 0755);

    EnsureSymlink("/dev/fd", "/proc/self/fd");
    EnsureSymlink("/dev/stdin", "/proc/self/fd/0");
    EnsureSymlink("/dev/stdout", "/proc/self/fd/1");
    EnsureSymlink("/dev/stderr", "/proc/self/fd/2");
    EnsureSymlink("/etc/mtab", "/proc/mounts");

    EnsureRegularFileIfMissing("/etc/hostname", "localhost\n", 0644);
    EnsureRegularFileIfMissing("/etc/hosts", "127.0.0.1\tlocalhost\n127.0.1.1\tlocalhost\n", 0644);
}

typedef struct {
    struct task *init;
    char *file;
    size_t argc;
    char *argv;
    char *envp;
} ISHFallbackInitConfig;

static ISHFallbackInitConfig *CreateFallbackInitConfig(struct task *init,
                                                       NSString *filePath,
                                                       NSArray<NSString *> *argvCommand,
                                                       const char *argv,
                                                       size_t argvSize,
                                                       NSData *envpData) {
    ISHFallbackInitConfig *config = calloc(1, sizeof(*config));
    if (config == NULL)
        return NULL;

    config->init = init;
    config->argc = argvCommand.count;
    config->file = strdup(filePath.UTF8String);
    config->argv = malloc(argvSize);
    config->envp = malloc(envpData.length);
    if (config->file == NULL || config->argv == NULL || config->envp == NULL) {
        free(config->file);
        free(config->argv);
        free(config->envp);
        free(config);
        return NULL;
    }

    memcpy(config->argv, argv, argvSize);
    memcpy(config->envp, envpData.bytes, envpData.length);
    return config;
}

static void *FallbackConsoleInitThread(void *context) {
    ISHFallbackInitConfig *config = context;
    current = config->init;

    while (true) {
        intptr_t err = become_new_init_child();
        if (err < 0) {
            printk("ERROR: fallback init could not create console shell child: %ld\n", (long) err);
            sleep(1);
            current = config->init;
            continue;
        }

        struct task *child = current;
        pid_t childPid = child->pid;
        bool needsSession = true;
        if (child->group != NULL) {
            lock(&child->group->lock, 0);
            needsSession = child->group->sid != childPid || child->group->pgid != childPid;
            unlock(&child->group->lock);
        }
        if (needsSession) {
            intptr_t session = (intptr_t) (int32_t) sys_setsid();
            if (session < 0)
                printk("fake init could not start a new session for pid %d: %ld\n", childPid, (long) session);
        }
        err = create_stdio("/dev/console", TTY_CONSOLE_MAJOR, 1);
        if (err == 0) {
            intptr_t cttyErr = (intptr_t) (int32_t) sys_ioctl(0, TIOCSCTTY_, 1);
            if (cttyErr < 0)
                printk("fake init could not set controlling tty for pid %d: %ld\n", childPid, (long) cttyErr);
        }
        if (err == 0)
            err = do_execve(config->file, config->argc, config->argv, config->envp);

        if (err < 0) {
            printk("ERROR: fake init could not exec %s as console shell: %ld\n", config->file, (long) err);
            current = config->init;
            return NULL;
        }

        printk("fake init started console shell pid=%d command=%s\n", childPid, config->file);
        task_start(child);
        current = config->init;

        while (true) {
            dword_t waited = sys_wait4_guest(-1, 0, 0, 0);
            printk("fake init reaped child wait_result=%#x console_shell_pid=%d\n", waited, childPid);
            if (waited == (dword_t) childPid)
                break;
            if ((int32_t) waited == _ECHILD)
                break;
        }
        sleep(1);
    }
}

static intptr_t StartFallbackConsoleSupervisor(NSArray<NSString *> *command,
                                               NSArray<NSString *> *argvCommand,
                                               const char *argv,
                                               size_t argvSize,
                                               NSData *envpData) {
    ISHFallbackInitConfig *config = CreateFallbackInitConfig(current, command.firstObject, argvCommand, argv, argvSize, envpData);
    if (config == NULL)
        return _ENOMEM;

    pthread_t thread;
    if (pthread_create(&thread, NULL, FallbackConsoleInitThread, config) != 0) {
        free(config->file);
        free(config->argv);
        free(config->envp);
        free(config);
        return _EAGAIN;
    }
    pthread_detach(thread);
    return 0;
}

static NSArray<NSString *> *BootCommandWithInitFallback(NSArray<NSString *> *command,
                                                        NSDictionary<NSString *, id> *details) {
    if (!BootCommandIsDefaultInit(command))
        return command;

    intptr_t configuredErr = 0;
    if (BootExecutableExists(command[0], &configuredErr)) {
        bootUsesConsoleSessionFallback = NO;
        bootUsesNativeFakeInit = NO;
        return command;
    }

    NSArray<NSArray<NSString *> *> *candidates = @[
        @[@"/init"],
        @[@"/etc/init"],
        @[@"/bin/init"],
        @[@"/bin/busybox", @"init"],
        @[@"/usr/bin/busybox", @"init"],
        @[@"/usr/bin/bash", @"-i"],
        @[@"/bin/bash", @"-i"],
        @[@"/usr/bin/sh", @"-i"],
        @[@"/bin/sh", @"-i"],
        @[@"/usr/bin/ash", @"-i"],
        @[@"/bin/ash", @"-i"],
        @[@"/bin/busybox", @"sh", @"-i"],
        @[@"/usr/bin/busybox", @"sh", @"-i"],
        @[@"/usr/bin/login", @"-f", @"root"],
        @[@"/bin/login", @"-f", @"root"],
    ];
    NSMutableArray<NSDictionary<NSString *, id> *> *attempts = [NSMutableArray arrayWithCapacity:candidates.count + 1];
    [attempts addObject:@{@"command": [command componentsJoinedByString:@" "],
                          @"path": command[0],
                          @"error": @(configuredErr),
                          @"errorDescription": ISHDescriptionForErrno(configuredErr)}];

    for (NSArray<NSString *> *candidate in candidates) {
        NSString *path = candidate.firstObject;
        intptr_t err = 0;
        if (BootExecutableExists(path, &err)) {
            NSMutableDictionary<NSString *, id> *fallbackDetails = [NSMutableDictionary dictionaryWithDictionary:details ?: @{}];
            fallbackDetails[@"configuredCommand"] = [command componentsJoinedByString:@" "];
            fallbackDetails[@"fallbackCommand"] = [candidate componentsJoinedByString:@" "];
            fallbackDetails[@"fallbackPath"] = path ?: @"";
            BOOL nativeFakeInit = BootCommandIsInteractiveConsoleShellFallback(candidate);
            fallbackDetails[@"mode"] = nativeFakeInit ? @"native-fake-init" : @"console-session";
            fallbackDetails[@"missingInitError"] = @(configuredErr);
            fallbackDetails[@"missingInitErrorDescription"] = ISHDescriptionForErrno(configuredErr);
            fallbackDetails[@"candidates"] = BootCommandFallbackCandidatesDescription(candidates);
            fallbackDetails[@"attempts"] = attempts;
            bootUsesConsoleSessionFallback = YES;
            bootUsesNativeFakeInit = nativeFakeInit;
            [ISHDiagnosticsStore recordLaunchStage:nativeFakeInit ? @"boot.init.fake.selected" : @"boot.init.fallback.selected"
                                           details:fallbackDetails];
            if (nativeFakeInit) {
                NSLog(@"boot native fake init selected: %@ missing; supervisor will launch console command %@",
                      [command componentsJoinedByString:@" "],
                      [candidate componentsJoinedByString:@" "]);
            } else {
                NSLog(@"boot init fallback selected: %@ -> %@",
                      [command componentsJoinedByString:@" "],
                      [candidate componentsJoinedByString:@" "]);
            }
            return candidate;
        }
        [attempts addObject:@{@"command": [candidate componentsJoinedByString:@" "],
                              @"path": path ?: @"",
                              @"error": @(err),
                              @"errorDescription": ISHDescriptionForErrno(err)}];
    }

    NSMutableDictionary<NSString *, id> *fallbackDetails = [NSMutableDictionary dictionaryWithDictionary:details ?: @{}];
    fallbackDetails[@"configuredCommand"] = [command componentsJoinedByString:@" "];
    fallbackDetails[@"missingInitError"] = @(configuredErr);
    fallbackDetails[@"missingInitErrorDescription"] = ISHDescriptionForErrno(configuredErr);
    fallbackDetails[@"candidates"] = BootCommandFallbackCandidatesDescription(candidates);
    fallbackDetails[@"attempts"] = attempts;
    bootUsesConsoleSessionFallback = NO;
    bootUsesNativeFakeInit = NO;
    [ISHDiagnosticsStore recordLaunchStage:@"boot.init.fallback.none"
                                   details:fallbackDetails];
    return command;
}

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

static NSURL *DiagnosticsLaunchJournalURL(void) {
    NSURL *directoryURL = DiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return nil;
    return [directoryURL URLByAppendingPathComponent:kDiagnosticsLaunchJournalFile isDirectory:NO];
}

static NSURL *DiagnosticsGuestFatalURL(void) {
    NSURL *directoryURL = DiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return nil;
    return [directoryURL URLByAppendingPathComponent:kDiagnosticsGuestFatalFile isDirectory:NO];
}

static NSURL *DiagnosticsGuestExitsURL(void) {
    NSURL *directoryURL = DiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return nil;
    return [directoryURL URLByAppendingPathComponent:kDiagnosticsGuestExitsFile isDirectory:NO];
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

static void DiagnosticsEnsureDirectoryExists(void) {
    NSURL *directoryURL = DiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return;
    [NSFileManager.defaultManager createDirectoryAtURL:directoryURL
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
}

static void DiagnosticsWriteJSONObject(NSURL *url, id object) {
    if (url == nil || object == nil)
        return;
    DiagnosticsEnsureDirectoryExists();
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:object
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:nil];
    if (jsonData != nil)
        [jsonData writeToURL:url options:NSDataWritingAtomic error:nil];
}

static NSDictionary<NSString *, id> *DiagnosticsGuestExitDictionary(int pid, int ppid, int tgid,
                                                                    NSString *comm, int code) {
    NSMutableDictionary<NSString *, id> *entry = [NSMutableDictionary dictionary];
    entry[@"timestamp"] = DiagnosticsISO8601StringFromDate([NSDate date]) ?: @"";
    entry[@"pid"] = @(pid);
    entry[@"ppid"] = @(ppid);
    entry[@"tgid"] = @(tgid);
    if (comm.length != 0)
        entry[@"comm"] = comm;
    entry[@"code"] = @(code);
    if ((code & 0xff) == 0x7f) {
        entry[@"kind"] = @"stopped";
        entry[@"signal"] = @((code >> 8) & 0xff);
    } else if ((code & 0x7f) == 0) {
        entry[@"kind"] = @"exited";
        entry[@"exitCode"] = @((code >> 8) & 0xff);
    } else {
        entry[@"kind"] = @"signaled";
        entry[@"signal"] = @(code & 0x7f);
        entry[@"coreDumped"] = @((code & 0x80) != 0);
    }
    entry[@"statusHex"] = [NSString stringWithFormat:@"%#x", code];
    return entry;
}

void ISHDiagnosticsRecordGuestFatalSync(const char *kind, const char *summary, const char *detail) {
    @autoreleasepool {
        NSMutableDictionary<NSString *, id> *record = [NSMutableDictionary dictionary];
        record[@"timestamp"] = DiagnosticsISO8601StringFromDate([NSDate date]) ?: @"";
        if (kind != NULL)
            record[@"kind"] = [NSString stringWithUTF8String:kind] ?: @"";
        if (summary != NULL)
            record[@"summary"] = [NSString stringWithUTF8String:summary] ?: @"";
        if (detail != NULL)
            record[@"detail"] = [NSString stringWithUTF8String:detail] ?: @"";
        DiagnosticsWriteJSONObject(DiagnosticsGuestFatalURL(), record);
    }
}

void ISHDiagnosticsRecordGuestExitSyncDetailed(int pid, int ppid, int tgid, const char *comm, int code) {
    @autoreleasepool {
        NSURL *url = DiagnosticsGuestExitsURL();
        if (url == nil)
            return;
        DiagnosticsEnsureDirectoryExists();

        NSData *existingData = [NSData dataWithContentsOfURL:url];
        NSMutableArray<NSDictionary<NSString *, id> *> *entries = [NSMutableArray array];
        if (existingData.length > 0) {
            id existingObject = [NSJSONSerialization JSONObjectWithData:existingData options:NSJSONReadingMutableContainers error:nil];
            if ([existingObject isKindOfClass:[NSArray class]])
                [entries addObjectsFromArray:existingObject];
        }

        NSString *commString = comm != NULL ? [NSString stringWithUTF8String:comm] : nil;
        [entries addObject:DiagnosticsGuestExitDictionary(pid, ppid, tgid, commString, code)];
        const NSUInteger maxEntries = 32;
        if (entries.count > maxEntries) {
            NSRange overflow = NSMakeRange(0, entries.count - maxEntries);
            [entries removeObjectsInRange:overflow];
        }
        DiagnosticsWriteJSONObject(url, entries);
    }
}

void ISHScheduleLaunchJournalCompletion(NSDictionary<NSString *, id> *details) {
    NSDictionary<NSString *, id> *detailsCopy = [details copy];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [ISHDiagnosticsStore completeLaunchWithDetails:detailsCopy];
    });
}

static int EnsurePathRemoved(const char *path, const struct statbuf *stat) {
    if (S_ISDIR(stat->mode))
        return generic_rmdirat(AT_PWD, path);
    return generic_unlinkat(AT_PWD, path);
}

static int EnsureDirectory(const char *path, mode_t_ mode) {
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, AT_SYMLINK_NOFOLLOW_);
    if (err == _ENOENT)
        return generic_mkdirat(AT_PWD, path, mode & 07777);
    if (err < 0)
        return err;

    if (!S_ISDIR(stat.mode)) {
        err = EnsurePathRemoved(path, &stat);
        if (err < 0)
            return err;
        return generic_mkdirat(AT_PWD, path, mode & 07777);
    }

    if ((stat.mode & 07777) != (mode & 07777))
        return generic_setattrat(AT_PWD, path, make_attr(mode, mode & 07777), false);
    return 0;
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

static int EnsureRegularFileIfMissing(const char *path, const char *contents, mode_t_ mode) {
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, AT_SYMLINK_NOFOLLOW_);
    if (err >= 0)
        return S_ISREG(stat.mode) ? 0 : _EEXIST;
    if (err != _ENOENT)
        return err;

    struct fd *fd = generic_open(path, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode & 07777);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    if (contents != NULL && contents[0] != '\0') {
        ssize_t written = fd->ops->write(fd, contents, strlen(contents));
        if (written < 0) {
            fd_close(fd);
            return (int) written;
        }
    }
    fd_close(fd);
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

static NSURL *AOKPersistDirectoryURL(void) {
    NSURL *containerURL = ContainerURL();
    if (containerURL == nil)
        return nil;
    return [[containerURL URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
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

+ (void)updateLaunchJournal:(void (^)(NSMutableDictionary<NSString *, id> *journal))block {
    if (block == nil)
        return;
    dispatch_sync(self.queue, ^{
        NSURL *directoryURL = DiagnosticsDirectoryURL();
        NSURL *journalURL = DiagnosticsLaunchJournalURL();
        if (directoryURL == nil || journalURL == nil)
            return;
        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];

        NSMutableDictionary<NSString *, id> *journal = [NSMutableDictionary dictionary];
        NSData *existingData = [NSData dataWithContentsOfURL:journalURL];
        if (existingData.length > 0) {
            id existingObject = [NSJSONSerialization JSONObjectWithData:existingData options:NSJSONReadingMutableContainers error:nil];
            if ([existingObject isKindOfClass:[NSDictionary class]])
                [journal addEntriesFromDictionary:existingObject];
        }

        block(journal);

        NSData *jsonData = [NSJSONSerialization dataWithJSONObject:journal options:NSJSONWritingPrettyPrinted error:nil];
        if (jsonData != nil)
            [jsonData writeToURL:journalURL options:NSDataWritingAtomic error:nil];
    });
}

+ (void)recordLaunchStage:(NSString *)stage {
    [self recordLaunchStage:stage details:nil];
}

+ (void)recordLaunchStage:(NSString *)stage details:(NSDictionary<NSString *,id> *)details {
    if (stage.length == 0)
        return;
    [self updateLaunchJournal:^(NSMutableDictionary<NSString *,id> *journal) {
        NSString *processIdentifier = [NSString stringWithFormat:@"%d", NSProcessInfo.processInfo.processIdentifier];
        NSString *existingProcessIdentifier = journal[@"processIdentifier"];
        if (![existingProcessIdentifier isEqualToString:processIdentifier]) {
            [journal removeAllObjects];
            journal[@"launchID"] = NSUUID.UUID.UUIDString;
            journal[@"processIdentifier"] = processIdentifier;
            journal[@"startedAt"] = DiagnosticsISO8601StringFromDate([NSDate date]) ?: @"";
            journal[@"completed"] = @NO;
            journal[@"stages"] = [NSMutableArray array];
        }

        NSMutableArray<NSDictionary<NSString *, id> *> *stages = [journal[@"stages"] isKindOfClass:[NSMutableArray class]]
            ? journal[@"stages"]
            : [NSMutableArray array];
        NSMutableDictionary<NSString *, id> *entry = [NSMutableDictionary dictionary];
        entry[@"timestamp"] = DiagnosticsISO8601StringFromDate([NSDate date]) ?: @"";
        entry[@"stage"] = stage;
        if (details.count != 0)
            entry[@"details"] = details;
        [stages addObject:entry];
        if (stages.count > 64) {
            NSRange overflow = NSMakeRange(0, stages.count - 64);
            [stages removeObjectsInRange:overflow];
        }
        journal[@"stages"] = stages;
        journal[@"lastStage"] = stage;
        journal[@"updatedAt"] = entry[@"timestamp"];
        journal[@"completed"] = @NO;
        [journal removeObjectForKey:@"completedAt"];
        [journal removeObjectForKey:@"completionDetails"];
    }];
}

+ (void)completeLaunchWithDetails:(NSDictionary<NSString *,id> *)details {
    [self updateLaunchJournal:^(NSMutableDictionary<NSString *,id> *journal) {
        if (journal.count == 0)
            return;
        journal[@"completed"] = @YES;
        journal[@"completedAt"] = DiagnosticsISO8601StringFromDate([NSDate date]) ?: @"";
        if (details.count != 0)
            journal[@"completionDetails"] = details;
        else
            [journal removeObjectForKey:@"completionDetails"];
    }];
}

+ (NSDictionary<NSString *,id> *)currentLaunchJournal {
    __block NSDictionary<NSString *, id> *result = nil;
    dispatch_sync(self.queue, ^{
        NSData *data = [NSData dataWithContentsOfURL:DiagnosticsLaunchJournalURL()];
        if (data.length == 0)
            return;
        id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if ([object isKindOfClass:[NSDictionary class]])
            result = object;
    });
    return result;
}

+ (NSDictionary<NSString *, id> *)currentGuestFatalEvent {
    __block NSDictionary<NSString *, id> *result = nil;
    dispatch_sync(self.queue, ^{
        NSData *data = [NSData dataWithContentsOfURL:DiagnosticsGuestFatalURL()];
        if (data.length == 0)
            return;
        id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if ([object isKindOfClass:[NSDictionary class]])
            result = object;
    });
    return result;
}

+ (NSArray<NSDictionary<NSString *, id> *> *)recentGuestExitsWithLimit:(NSUInteger)limit {
    __block NSArray<NSDictionary<NSString *, id> *> *result = @[];
    dispatch_sync(self.queue, ^{
        NSData *data = [NSData dataWithContentsOfURL:DiagnosticsGuestExitsURL()];
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

    NSDictionary<NSString *, id> *launchJournal = [self currentLaunchJournal];
    [report appendString:@"\nLast Launch Journal\n"];
    if (launchJournal.count == 0) {
        [report appendString:@"  none\n"];
    } else {
        [report appendFormat:@"  launchID: %@\n", launchJournal[@"launchID"] ?: @"unknown"];
        [report appendFormat:@"  pid: %@\n", launchJournal[@"processIdentifier"] ?: @"unknown"];
        [report appendFormat:@"  started: %@\n", launchJournal[@"startedAt"] ?: @"unknown"];
        [report appendFormat:@"  updated: %@\n", launchJournal[@"updatedAt"] ?: @"unknown"];
        [report appendFormat:@"  completed: %@\n", [launchJournal[@"completed"] boolValue] ? @"yes" : @"no"];
        if (launchJournal[@"completedAt"] != nil)
            [report appendFormat:@"  completedAt: %@\n", launchJournal[@"completedAt"]];
        if (launchJournal[@"lastStage"] != nil)
            [report appendFormat:@"  lastStage: %@\n", launchJournal[@"lastStage"]];
        NSDictionary *completionDetails = launchJournal[@"completionDetails"];
        if (completionDetails.count != 0)
            [report appendFormat:@"  completionDetails: %@\n",
             [[completionDetails description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "]];
        NSArray<NSDictionary<NSString *, id> *> *stages = launchJournal[@"stages"];
        for (NSDictionary<NSString *, id> *entry in stages) {
            [report appendFormat:@"    %@  %@",
             entry[@"timestamp"] ?: @"",
             entry[@"stage"] ?: @""];
            NSDictionary *details = entry[@"details"];
            if (details.count != 0)
                [report appendFormat:@"  %@",
                 [[details description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "]];
            [report appendString:@"\n"];
        }
    }

    NSDictionary<NSString *, id> *guestFatal = [self currentGuestFatalEvent];
    [report appendString:@"\nLast Guest Fatal Event\n"];
    if (guestFatal.count == 0) {
        [report appendString:@"  none\n"];
    } else {
        [report appendFormat:@"  timestamp: %@\n", guestFatal[@"timestamp"] ?: @"unknown"];
        [report appendFormat:@"  kind: %@\n", guestFatal[@"kind"] ?: @"unknown"];
        [report appendFormat:@"  summary: %@\n", guestFatal[@"summary"] ?: @"unknown"];
        if ([guestFatal[@"detail"] length] != 0) {
            NSString *detail = [guestFatal[@"detail"] stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
            for (NSString *line in [detail componentsSeparatedByString:@"\n"]) {
                if (line.length == 0)
                    continue;
                [report appendFormat:@"    %@\n", line];
            }
        }
    }

    NSArray<NSDictionary<NSString *, id> *> *guestExits = [self recentGuestExitsWithLimit:20];
    [report appendString:@"\nRecent Guest Exits\n"];
    if (guestExits.count == 0) {
        [report appendString:@"  none\n"];
    } else {
        for (NSDictionary<NSString *, id> *entry in guestExits) {
            NSMutableArray<NSString *> *parts = [NSMutableArray array];
            [parts addObject:[NSString stringWithFormat:@"pid=%@", entry[@"pid"] ?: @"?"]];
            if (entry[@"ppid"] != nil)
                [parts addObject:[NSString stringWithFormat:@"ppid=%@", entry[@"ppid"]]];
            if (entry[@"tgid"] != nil)
                [parts addObject:[NSString stringWithFormat:@"tgid=%@", entry[@"tgid"]]];
            if ([entry[@"comm"] length] != 0)
                [parts addObject:[NSString stringWithFormat:@"comm=%@", entry[@"comm"]]];
            NSString *kind = entry[@"kind"];
            if ([kind isEqualToString:@"exited"]) {
                [parts addObject:[NSString stringWithFormat:@"exit=%@", entry[@"exitCode"] ?: @"?"]];
            } else if ([kind isEqualToString:@"signaled"]) {
                [parts addObject:[NSString stringWithFormat:@"signal=%@", entry[@"signal"] ?: @"?"]];
                if ([entry[@"coreDumped"] boolValue])
                    [parts addObject:@"core=1"];
            } else if ([kind isEqualToString:@"stopped"]) {
                [parts addObject:[NSString stringWithFormat:@"stopped=%@", entry[@"signal"] ?: @"?"]];
            } else {
                [parts addObject:[NSString stringWithFormat:@"code=%@", entry[@"code"] ?: @"?"]];
            }
            if (entry[@"statusHex"] != nil)
                [parts addObject:[NSString stringWithFormat:@"status=%@", entry[@"statusHex"]]];
            [report appendFormat:@"  %@  %@\n",
             entry[@"timestamp"] ?: @"",
             [parts componentsJoinedByString:@" "]];
        }
    }

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
    NSURL *launchJournalURL = DiagnosticsLaunchJournalURL();
    if (launchJournalURL != nil && [NSFileManager.defaultManager fileExistsAtPath:launchJournalURL.path]) {
        [NSFileManager.defaultManager copyItemAtURL:launchJournalURL
                                              toURL:[baseDirectory URLByAppendingPathComponent:launchJournalURL.lastPathComponent]
                                              error:nil];
    }
    NSURL *guestFatalURL = DiagnosticsGuestFatalURL();
    if (guestFatalURL != nil && [NSFileManager.defaultManager fileExistsAtPath:guestFatalURL.path]) {
        [NSFileManager.defaultManager copyItemAtURL:guestFatalURL
                                              toURL:[baseDirectory URLByAppendingPathComponent:guestFatalURL.lastPathComponent]
                                              error:nil];
    }
    NSURL *guestExitsURL = DiagnosticsGuestExitsURL();
    if (guestExitsURL != nil && [NSFileManager.defaultManager fileExistsAtPath:guestExitsURL.path]) {
        [NSFileManager.defaultManager copyItemAtURL:guestExitsURL
                                              toURL:[baseDirectory URLByAppendingPathComponent:guestExitsURL.lastPathComponent]
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

    struct task *init = pid_get_task_ref(1);

    if (init != NULL) {
        bool usable = false;
        lock(&init->general_lock, 0);
        usable = init->mm != NULL && init->mem != NULL &&
                 init->files != NULL && init->fs != NULL;
        unlock(&init->general_lock);
        if (!usable) {
            task_ref_cnt_mod(init, -1);
            init = NULL;
        }
    }

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

+ (BOOL)pushUsableInitTaskAsCurrent:(struct task **)previousCurrent {
    return PushInitTaskAsCurrent(previousCurrent);
}

+ (void)popCurrentTask:(struct task *)previousCurrent {
    PopCurrentTask(previousCurrent);
}

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
        [ISHDiagnosticsStore recordLaunchStage:@"boot.ensure.begin"];
	        bootFailureTitle = nil;
	        bootFailureMessage = nil;
	        bootFailureOverlayText = nil;
	        bootFailureDetails = nil;
	        bootUsesConsoleSessionFallback = NO;
	        bootUsesNativeFakeInit = NO;
        AppDelegate *delegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
        if ([delegate isKindOfClass:AppDelegate.class]) {
            bootError = [delegate boot];
        } else {
            bootError = RecordBootFailure(_ESRCH,
                                          @"boot.delegate.missing",
                                          @"Boot failed before app setup",
                                          @"The application delegate was not available when boot was requested.",
                                          @"Restart iSH-AOK. If this repeats, open Diagnostics from recovery mode.",
                                          nil);
        }
        if (bootError < 0) {
            [ISHDiagnosticsStore recordLaunchStage:@"boot.ensure.failed"
                                           details:bootFailureDetails ?: @{@"error": @(bootError),
                                                                           @"errorDescription": ISHDescriptionForErrno(bootError)}];
        } else {
            [ISHDiagnosticsStore recordLaunchStage:@"boot.ensure.end"];
        }
    });
    return bootError;
}

- (intptr_t)boot {
#if !ISH_LINUX
    [ISHDiagnosticsStore recordLaunchStage:@"boot.begin"];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    if (defaultRoot == nil) {
        return RecordBootFailure(_ENOENT,
                                 @"boot.root.none",
                                 @"No boot filesystem is selected",
                                 @"iSH-AOK cannot boot because no active filesystem is configured.",
                                 @"Open Filesystems and choose or import a root filesystem.",
                                 @{@"rootCount": @(Roots.instance.roots.count)});
    }
    NSError *rootLockError = nil;
    int rootLockFd = ISHAppGroupAcquireNamedLock(@"root", defaultRoot, YES, &rootLockError);
    if (rootLockFd < 0) {
        [ISHDiagnosticsStore recordLaunchStage:@"boot.root.lock.failed"
                                       details:@{@"root": defaultRoot,
                                                 @"error": rootLockError.localizedDescription ?: @"unknown"}];
    } else {
        [ISHDiagnosticsStore recordLaunchStage:@"boot.root.locked"
                                       details:@{@"root": defaultRoot}];
    }
    @try {
    [ISHDiagnosticsStore recordLaunchStage:@"boot.root.selected"
                                   details:@{@"root": defaultRoot}];
    NSString *guestABI = [Roots.instance guestABIForRootNamed:defaultRoot];
    if ([guestABI isEqualToString:@"amd64"]) {
        NSLog(@"Attempting experimental amd64 guest boot for root %@", defaultRoot);
        [ISHDiagnosticsStore recordLaunchStage:@"boot.root.amd64"
                                       details:@{@"root": defaultRoot}];
    }

    NSURL *root = [Roots.instance rootUrl:defaultRoot];
    NSURL *rootData = [root URLByAppendingPathComponent:@"data" isDirectory:YES];
    NSURL *rootMetadata = [root URLByAppendingPathComponent:@"meta.db" isDirectory:NO];
    BOOL isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:root.path isDirectory:&isDirectory] || !isDirectory) {
        return RecordBootFailure(_ENOENT,
                                 @"boot.root.directory.missing",
                                 @"Selected filesystem is missing",
                                 @"The active filesystem points to a root directory that is not present on disk.",
                                 @"Choose another filesystem or reimport this one.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": root.path ?: @""});
    }
    isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:rootData.path isDirectory:&isDirectory] || !isDirectory) {
        return RecordBootFailure(_ENOENT,
                                 @"boot.root.data.missing",
                                 @"Selected filesystem is incomplete",
                                 @"The active filesystem directory exists, but its data directory is missing.",
                                 @"Choose another filesystem or reimport this one.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": rootData.path ?: @""});
    }
    isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:rootMetadata.path isDirectory:&isDirectory] || isDirectory) {
        return RecordBootFailure(_ENOENT,
                                 @"boot.root.metadata.missing",
                                 @"Selected filesystem metadata is missing",
                                 @"The active filesystem data exists, but its fakefs metadata database is missing.",
                                 @"Choose another filesystem or reimport this one.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": rootMetadata.path ?: @""});
    }

    intptr_t err = mount_root(&fakefs, rootData.fileSystemRepresentation);
    if (err < 0) {
        return RecordBootFailure(err,
                                 @"boot.root.mount.failed",
                                 @"Boot failed while mounting the filesystem",
                                 @"iSH-AOK found the selected filesystem, but fakefs could not mount it.",
                                 BootMountRecovery(err),
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": rootData.path ?: @""});
    }
    [ISHDiagnosticsStore recordLaunchStage:@"boot.root.mounted"
                                   details:@{@"root": defaultRoot}];

    fs_register(&iosfs);
    fs_register(&iosfs_unsafe);

    // need to do this first so that we can have a valid current for the generic_mknod calls
    err = become_first_process();
    if (err < 0) {
        return RecordBootFailure(err,
                                 @"boot.first_process.failed",
                                 @"Boot failed while creating init",
                                 @"The filesystem was mounted, but the emulator could not create the first guest process.",
                                 err == _ENOMEM
                                     ? @"Close other apps and restart iSH-AOK."
                                     : @"Restart iSH-AOK. If this repeats, open Diagnostics from recovery mode.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @""});
    }
    [ISHDiagnosticsStore recordLaunchStage:@"boot.first_process.ready"];

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
    
    // Permissions and type metadata on / have been broken for a while, let's fix them.
    generic_setattrat(AT_PWD, "/", (struct attr) {.type = attr_mode, .mode = S_IFDIR|0755}, false);
    
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
        return RecordBootFailure(err,
                                 @"boot.device.clipboard.failed",
                                 @"Boot failed while registering clipboard device",
                                 @"The filesystem was mounted, but iSH-AOK could not register /dev/clipboard.",
                                 @"Restart iSH-AOK. If this repeats, open Diagnostics from recovery mode.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": @"/dev/clipboard"});
    }
    EnsureCharacterDevice("/dev/clipboard", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR));
    
    err = dyn_dev_register(&location_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_LOCATION_MINOR);
    if (err != 0) {
        return RecordBootFailure(err,
                                 @"boot.device.location.failed",
                                 @"Boot failed while registering location device",
                                 @"The filesystem was mounted, but iSH-AOK could not register /dev/location.",
                                 @"Restart iSH-AOK. If this repeats, open Diagnostics from recovery mode.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": @"/dev/location"});
    }
    EnsureCharacterDevice("/dev/location", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_LOCATION_MINOR));

    err = dyn_dev_register((struct dev_ops *) &audio_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_DSP_MINOR);
    if (err != 0) {
        return RecordBootFailure(err,
                                 @"boot.device.audio.failed",
                                 @"Boot failed while registering audio device",
                                 @"The filesystem was mounted, but iSH-AOK could not register /dev/dsp.",
                                 @"Restart iSH-AOK. If this repeats, open Diagnostics from recovery mode.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": @"/dev/dsp"});
    }
    EnsureCharacterDevice("/dev/dsp", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_DSP_MINOR));
    
    // Emulate a RTC, read time only
    err = dyn_dev_register(&rtc_dev, DEV_CHAR, DEV_RTC_MAJOR, DEV_RTC_MINOR);
    if (err != 0) {
        return RecordBootFailure(err,
                                 @"boot.device.rtc.failed",
                                 @"Boot failed while registering clock device",
                                 @"The filesystem was mounted, but iSH-AOK could not register /dev/rtc0.",
                                 @"Restart iSH-AOK. If this repeats, open Diagnostics from recovery mode.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": @"/dev/rtc0"});
    }
    EnsureCharacterDevice("/dev/rtc0", S_IFCHR|0666, dev_make(DEV_RTC_MAJOR, DEV_RTC_MINOR));
    EnsureSymlink("/dev/rtc", "/dev/rtc0");

    do_mount(&aokfs, NSBundle.mainBundle.resourcePath.UTF8String, "/AOK", "", MS_READONLY_);
    NSURL *aokPersistURL = AOKPersistDirectoryURL();
    if (aokPersistURL != nil) {
        NSError *persistError = nil;
        if ([NSFileManager.defaultManager createDirectoryAtURL:aokPersistURL
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&persistError]) {
            int persistMountErr = do_mount(&realfs, aokPersistURL.fileSystemRepresentation, "/AOK/persist", "", 0);
            if (persistMountErr < 0)
                NSLog(@"Could not mount /AOK/persist: %d", persistMountErr);
        } else {
            NSLog(@"Could not create /AOK/persist backing directory: %@", persistError);
        }
    }
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
    if (err < 0) {
        return RecordBootFailure(err,
                                 @"boot.stdio.failed",
                                 @"Boot failed while opening console",
                                 @"The filesystem was mounted, but iSH-AOK could not attach init to /dev/console.",
                                 @"The root's /dev entries may be damaged. Choose another filesystem or reimport this one.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"path": @"/dev/console"});
    }
    
    NSArray<NSString *> *command;
    command = UserPreferences.shared.bootCommand;
    if (command.count == 0 || command[0].length == 0) {
        return RecordBootFailure(_EINVAL,
                                 @"boot.command.empty",
                                 @"Boot command is empty",
                                 @"iSH-AOK cannot start init because the configured boot command is empty.",
                                 @"Set a boot command in Settings. The default is /sbin/init.",
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @""});
    }
    command = BootCommandWithInitFallback(command,
                                          @{@"root": defaultRoot,
                                            @"guestABI": guestABI ?: @""});
    NSString *commandString = [command componentsJoinedByString:@" "];
    NSArray<NSString *> *argvCommand = bootUsesNativeFakeInit ? FakeInitLoginShellArgvForCommand(command) : command;
    char argv[4096];
    [Terminal convertCommand:argvCommand toArgs:argv limitSize:sizeof(argv)];
    NSData *envpData = BootEnvironmentForCommand(command.firstObject);
    if (bootUsesNativeFakeInit) {
        FakeInitPrepareGuestRoot();
        err = StartFallbackConsoleSupervisor(command, argvCommand, argv, sizeof(argv), envpData);
        if (err < 0) {
            return RecordBootFailure(err,
                                     @"boot.init.supervisor.failed",
                                     @"Boot failed while starting fallback console",
                                     @"The filesystem was mounted, but iSH-AOK could not start its fallback console supervisor.",
                                     @"Restart iSH-AOK. If this repeats, choose another filesystem or reimport this one.",
                                     @{@"root": defaultRoot,
                                       @"guestABI": guestABI ?: @"",
                                       @"command": commandString ?: @""});
        }
        [ISHDiagnosticsStore recordLaunchStage:@"boot.init.fake.started"
                                       details:@{@"root": defaultRoot,
                                                 @"guestABI": guestABI ?: @"",
                                                 @"command": commandString ?: @"",
                                                 @"argv": [argvCommand componentsJoinedByString:@" "] ?: @""}];
        return 0;
    }
    err = do_execve(command[0].UTF8String, command.count, argv, envpData.bytes);
    if (err < 0) {
        return RecordBootFailure(err,
                                 @"boot.init.exec.failed",
                                 @"Boot failed while starting init",
                                 @"The filesystem was mounted, but the configured boot command could not be executed.",
                                 BootExecRecovery(err, guestABI),
                                 @{@"root": defaultRoot,
                                   @"guestABI": guestABI ?: @"",
                                   @"command": commandString ?: @""});
    }
    [ISHDiagnosticsStore recordLaunchStage:@"boot.init.exec"];
    task_start(current);
    [ISHDiagnosticsStore recordLaunchStage:@"boot.init.started"];
    } @finally {
        ISHAppGroupReleaseLock(rootLockFd);
    }

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
    [ISHDiagnosticsStore recordBreadcrumb:@"dns.configure.begin"];
    [self scheduleDnsRefresh:@"manual"];
    if (!self.dnsNotifyRegistered) {
        ISHDnsConfigurationNotifyKeyFunc notifyKeyFunc = ISHDnsConfigurationNotifyKeySymbol();
        const char *notifyKey = notifyKeyFunc != NULL ? notifyKeyFunc() : NULL;
        if (notifyKey != NULL) {
            dispatch_queue_t queue = dispatch_get_main_queue();
#if __has_include(<Network/Network.h>)
            if (@available(iOS 12.0, *)) {
                if (self.pathMonitorQueue == nil)
                    self.pathMonitorQueue = dispatch_queue_create("app.ish.iSH-AOK.dns-monitor", DISPATCH_QUEUE_SERIAL);
                queue = self.pathMonitorQueue;
            }
#endif
            __weak typeof(self) weakSelf = self;
            uint32_t token = NOTIFY_TOKEN_INVALID;
            int err = notify_register_dispatch(notifyKey, &token, queue, ^(int tokenValue) {
                __strong typeof(weakSelf) self = weakSelf;
                if (self == nil)
                    return;
                [ISHDiagnosticsStore recordBreadcrumb:@"dns.notify"
                                              details:@{@"token": @(tokenValue)}];
                [self scheduleDnsRefresh:@"dnsnotify"];
            });
            if (err == NOTIFY_STATUS_OK) {
                self.dnsNotifyToken = (int) token;
                self.dnsNotifyRegistered = YES;
            }
        }
    }
#if __has_include(<Network/Network.h>)
    if (@available(iOS 12.0, *)) {
        if (self.pathMonitor == nil) {
            self.pathMonitor = nw_path_monitor_create();
            if (self.pathMonitorQueue == nil)
                self.pathMonitorQueue = dispatch_queue_create("app.ish.iSH-AOK.dns-monitor", DISPATCH_QUEUE_SERIAL);
            __weak typeof(self) weakSelf = self;
            nw_path_monitor_set_update_handler(self.pathMonitor, ^(nw_path_t path) {
                __strong typeof(weakSelf) self = weakSelf;
                if (self == nil)
                    return;
                const char *status = "unknown";
                switch (nw_path_get_status(path)) {
                    case nw_path_status_satisfied:
                        status = "satisfied";
                        break;
                    case nw_path_status_unsatisfied:
                        status = "unsatisfied";
                        break;
                    case nw_path_status_satisfiable:
                        status = "satisfiable";
                        break;
                }
                [ISHDiagnosticsStore recordBreadcrumb:@"dns.pathUpdate"
                                              details:@{@"status": [NSString stringWithUTF8String:status] ?: @"unknown"}];
                [self scheduleDnsRefresh:@"nwpath"];
            });
            nw_path_monitor_set_queue(self.pathMonitor, self.pathMonitorQueue);
            nw_path_monitor_start(self.pathMonitor);
        }
    }
#endif
#endif
}

- (void)scheduleDnsRefresh:(NSString *)reason {
#if !ISH_LINUX
    @synchronized (self) {
        if (self.dnsRefreshRunning) {
            self.dnsRefreshQueued = YES;
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
    NSString *dnsSource = @"dnsinfo";
    NSMutableString *resolvConf = (NSMutableString *) ISHResolvConfFromDnsConfiguration();
    if (resolvConf == nil) {
        dnsSource = @"libresolv";
        struct __res_state res;
        if (EXIT_SUCCESS != res_ninit(&res)) {
            [ISHDiagnosticsStore recordBreadcrumb:@"dns.refresh.failed"
                                          details:@{@"reason": reason ?: @"unknown",
                                                    @"source": dnsSource,
                                                    @"stage": @"res_ninit"}];
            [self finishDnsRefreshAndRescheduleIfNeeded:reason];
            return;
        }

        resolvConf = [NSMutableString new];
        if (res.dnsrch[0] != NULL) {
            [resolvConf appendString:@"search"];
            for (int i = 0; res.dnsrch[i] != NULL; i++) {
                [resolvConf appendFormat:@" %s", res.dnsrch[i]];
            }
            [resolvConf appendString:@"\n"];
        }
        union res_sockaddr_union servers[NI_MAXSERV];
        int serversFound = res_getservers(&res, servers, NI_MAXSERV);
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
                continue;
            }
            [resolvConf appendFormat:@"nameserver %s\n", address];
            usableServers++;
        }

        res_nclose(&res);
        if (usableServers == 0) {
            [ISHDiagnosticsStore recordBreadcrumb:@"dns.refresh.failed"
                                          details:@{@"reason": reason ?: @"unknown",
                                                    @"source": dnsSource,
                                                    @"stage": @"no-servers"}];
            [self finishDnsRefreshAndRescheduleIfNeeded:reason];
            return;
        }
    }

    [ISHDiagnosticsStore recordBreadcrumb:@"dns.refresh.generated"
                                  details:@{@"reason": reason ?: @"unknown",
                                            @"source": dnsSource,
                                            @"summary": ISHDnsBreadcrumbSummary(dnsSource, reason, resolvConf) ?: @""}];

    struct task *previousCurrent;
    if (!PushInitTaskAsCurrent(&previousCurrent)) {
        [ISHDiagnosticsStore recordBreadcrumb:@"dns.refresh.failed"
                                      details:@{@"reason": reason ?: @"unknown",
                                                @"source": dnsSource,
                                                @"stage": @"push-init"}];
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
        [ISHDiagnosticsStore recordBreadcrumb:@"dns.refresh.wrote"
                                      details:@{@"reason": reason ?: @"unknown",
                                                @"source": dnsSource,
                                                @"summary": ISHDnsBreadcrumbSummary(dnsSource, reason, resolvConf) ?: @""}];
    } else {
        [ISHDiagnosticsStore recordBreadcrumb:@"dns.refresh.failed"
                                      details:@{@"reason": reason ?: @"unknown",
                                                @"source": dnsSource,
                                                @"stage": @"open-resolv-conf",
                                                @"errno": @(PTR_ERR(fd))}];
    }
    PopCurrentTask(previousCurrent);
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

+ (NSString *)descriptionForISHErrno:(intptr_t)err {
    return ISHDescriptionForErrno(err);
}

+ (NSString *)bootFailureTitle {
    return bootFailureTitle;
}

+ (NSString *)bootFailureMessage {
    return bootFailureMessage;
}

+ (NSString *)bootFailureOverlayText {
    return bootFailureOverlayText;
}

+ (BOOL)bootUsesConsoleSessionFallback {
    return bootUsesConsoleSessionFallback;
}

+ (void)maybePresentStartupMessageOnViewController:(UIViewController *)vc {
    if ([NSUserDefaults.standardUserDefaults integerForKey:kSkipStartupMessage] >= 1)
        return;
    [NSUserDefaults.standardUserDefaults setInteger:1 forKey:kSkipStartupMessage];
}

- (BOOL)application:(UIApplication *)application willFinishLaunchingWithOptions:(NSDictionary<UIApplicationLaunchOptionsKey,id> *)launchOptions {
    [ISHDiagnosticsStore recordLaunchStage:@"application.willFinishLaunching"
                                   details:launchOptions.count != 0 ? @{@"launchOptions": launchOptions.description} : nil];
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
    [ISHDiagnosticsStore recordLaunchStage:@"roots.loaded"
                                   details:@{@"needsInitialRootSelection": @(Roots.instance.needsInitialRootSelection),
                                             @"defaultRoot": Roots.instance.defaultRoot ?: @""}];
    if (!Roots.instance.needsInitialRootSelection) {
        bootError = [AppDelegate ensureBooted];
        NSMutableDictionary<NSString *, id> *bootCheckDetails = [NSMutableDictionary dictionaryWithObject:@(bootError)
                                                                                                    forKey:@"bootError"];
        if (bootFailureDetails.count != 0)
            bootCheckDetails[@"failure"] = bootFailureDetails;
        [ISHDiagnosticsStore recordLaunchStage:@"application.boot.checked"
                                       details:bootCheckDetails];
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
    [ISHDiagnosticsStore recordLaunchStage:@"application.didFinishLaunching"
                                   details:launchOptions.count != 0 ? @{@"launchOptions": launchOptions.description} : nil];
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
    [self configureDns];

    self.metricKitSubscriber = [ISHMetricKitSubscriber new];
    [self.metricKitSubscriber registerIfAvailable];

    if (self.window != nil) {
        // For iOS <13, where the app delegate owns the window instead of the scene
        if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
            [ISHDiagnosticsStore recordLaunchStage:@"launch.rootController.diagnostics"];
            self.window.rootViewController = CreateAboutNavigationController(NO, YES);
            ISHScheduleLaunchJournalCompletion(@{@"rootController": @"diagnostics"});
            return YES;
        }
        if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
            [ISHDiagnosticsStore recordLaunchStage:@"launch.rootController.recovery"];
            self.window.rootViewController = CreateAboutNavigationController(YES, NO);
            ISHScheduleLaunchJournalCompletion(@{@"rootController": @"recovery"});
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
            [ISHDiagnosticsStore recordLaunchStage:@"launch.rootController.initialRootSelection"];
            self.window.rootViewController = CreateRootSelectionViewController();
            ISHScheduleLaunchJournalCompletion(@{@"rootController": @"initial-root-selection"});
            return YES;
        }
        if (ISHShouldLaunchWorkspaceAtStartup()) {
            [ISHDiagnosticsStore recordLaunchStage:@"launch.rootController.workspace"];
            self.window.rootViewController = ISHCreateWorkspaceNavigationController();
            ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace"});
            return YES;
        }
        TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
        currentTerminalViewController = vc;
        [ISHDiagnosticsStore recordLaunchStage:@"launch.terminal.startNewSession"];
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
    if (ISHShouldLaunchWorkspaceAtStartup()) {
        [ISHDiagnosticsStore recordLaunchStage:@"launch.rootController.workspace.afterInitialImport"];
        self.window.rootViewController = ISHCreateWorkspaceNavigationController();
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace"});
        return;
    }
    TerminalViewController *vc = CreateTerminalViewController();
    if (vc == nil)
        return;

    self.window.rootViewController = vc;
    currentTerminalViewController = vc;
    [ISHDiagnosticsStore recordLaunchStage:@"launch.terminal.startNewSession.afterInitialImport"];
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
    if (self.dnsNotifyRegistered) {
        notify_cancel(self.dnsNotifyToken);
        self.dnsNotifyRegistered = NO;
        self.dnsNotifyToken = NOTIFY_TOKEN_INVALID;
    }
#if __has_include(<Network/Network.h>)
    if (@available(iOS 12.0, *)) {
        if (self.pathMonitor != nil) {
            nw_path_monitor_cancel(self.pathMonitor);
            self.pathMonitor = nil;
            self.pathMonitorQueue = nil;
        }
    }
#endif
    if (self.reachability != NULL) {
        SCNetworkReachabilityUnscheduleFromRunLoop(self.reachability, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(self.reachability);
    }
}

- (void)exitApp {
    self.exiting = YES;
    UIApplication *application = UIApplication.sharedApplication;
    if (@available(iOS 13.0, *)) {
        for (UIScene *scene in application.connectedScenes) {
            UISceneSession *session = scene.session;
            if (session == nil)
                continue;
            [application requestSceneSessionDestruction:session
                                                options:nil
                                           errorHandler:^(__unused NSError *error) {
            }];
        }
    }
    id app = application;
    [app suspend];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (750 * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(), ^{
        if (self.exiting)
            exit(0);
    });
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
