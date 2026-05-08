//
//  AboutViewController.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "Diagnostics.h"
#import "UserPreferences.h"
#import "iOSFS.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"
#import "SceneDelegate.h"
#import "Terminal.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

NSString *const kPreferenceOpenDiagnosticsOnLaunchKey = @"openDiagnosticsOnLaunch";

UINavigationController *ISHCreateAboutNavigationController(BOOL recoveryMode, BOOL startInDiagnostics) {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
    aboutViewController.recoveryMode = recoveryMode;
    aboutViewController.startInDiagnostics = startInDiagnostics;
    return navigationController;
}

@interface DiagnosticsViewController : UIViewController
@end

@interface LLMClientViewController : UIViewController <UITextFieldDelegate>

@property (nonatomic, copy) NSString *initialPrompt;

@end

@interface LLMSettingsViewController : UITableViewController
@end

UIViewController *ISHCreateDiagnosticsViewController(void) {
    return [DiagnosticsViewController new];
}

UIViewController *ISHCreateLLMClientViewController(void) {
    return [LLMClientViewController new];
}

UIViewController *ISHCreateLLMClientViewControllerWithInitialPrompt(NSString *initialPrompt) {
    LLMClientViewController *viewController = [LLMClientViewController new];
    viewController.initialPrompt = initialPrompt;
    return viewController;
}

UIViewController *ISHCreateLLMSettingsViewController(void) {
    return [LLMSettingsViewController new];
}

BOOL ISHLLMClientEnabled(void) {
    return UserPreferences.shared.shouldEnableLLMClient;
}

static UISceneSession *ISHFindExistingWorkspaceSceneSession(UISceneSession *excludedSession) API_AVAILABLE(ios(13.0));
static UISceneSession *ISHFindExistingWorkspaceSceneSession(UISceneSession *excludedSession) {
    NSArray<NSString *> *forgottenHiddenSessions = [NSUserDefaults.standardUserDefaults arrayForKey:@"ISHWorkspaceForgottenHiddenSessions"];
    UISceneSession *bestSession = nil;
    for (UISceneSession *session in UIApplication.sharedApplication.openSessions) {
        if (session == nil || session == excludedSession)
            continue;
        if (![session.stateRestorationActivity.activityType isEqualToString:ISHSceneActivityTypeWorkspace])
            continue;
        UIScene *connectedScene = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (scene.session == session) {
                connectedScene = scene;
                break;
            }
        }
        if (connectedScene == nil && [forgottenHiddenSessions containsObject:session.persistentIdentifier])
            continue;
        if (connectedScene == nil && bestSession == nil) {
            bestSession = session;
            continue;
        }
        if (connectedScene == nil)
            continue;
        if (connectedScene.activationState == UISceneActivationStateForegroundActive)
            return session;
        if (bestSession == nil || connectedScene.activationState == UISceneActivationStateForegroundInactive)
            bestSession = session;
    }
    return bestSession;
}

@interface AboutViewController ()
@property (weak, nonatomic) IBOutlet UITableViewCell *capsLockMappingCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *themeCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *initialWindowCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *diagnosticsCell;
@property (weak, nonatomic) IBOutlet UISwitch *disableDimmingSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableExperimentalAmd64JitSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableMulticoreSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableExtraLockingSwitch;
@property (weak, nonatomic) IBOutlet UITextField *launchCommandField;
@property (weak, nonatomic) IBOutlet UITextField *bootCommandField;

@property (weak, nonatomic) IBOutlet UITableViewCell *sendFeedback;
@property (weak, nonatomic) IBOutlet UITableViewCell *openGithub;
@property (weak, nonatomic) IBOutlet UITableViewCell *openDiscord;

@property (weak, nonatomic) IBOutlet UITableViewCell *upgradeApkCell;
@property (weak, nonatomic) IBOutlet UILabel *upgradeApkLabel;
@property (weak, nonatomic) IBOutlet UIView *upgradeApkBadge;
@property (weak, nonatomic) IBOutlet UITableViewCell *exportContainerCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *resetMountsCell;

@property (weak, nonatomic) IBOutlet UILabel *versionLabel;

@property (nonatomic, strong) UISwitch *llmClientSwitch;

@end

@implementation DiagnosticsViewController {
    UITextView *_textView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Diagnostics";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _textView = [[UITextView alloc] initWithFrame:self.view.bounds];
    _textView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _textView.editable = NO;
    _textView.alwaysBounceVertical = YES;
    if (@available(iOS 13.0, *)) {
        _textView.backgroundColor = UIColor.systemBackgroundColor;
        _textView.textColor = UIColor.labelColor;
        _textView.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    } else {
        _textView.backgroundColor = UIColor.whiteColor;
        _textView.textColor = UIColor.blackColor;
        _textView.font = [UIFont fontWithName:@"Menlo-Regular" size:12] ?: [UIFont systemFontOfSize:12];
    }
    [self.view addSubview:_textView];

    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAction
                                                      target:self
                                                      action:@selector(exportDiagnostics:)],
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                      target:self
                                                      action:@selector(refreshDiagnostics:)],
    ];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshDiagnostics:)
                                               name:ISHDiagnosticsStoreDidUpdateNotification
                                             object:nil];
    [self refreshDiagnostics:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDiagnostics:nil];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
        [NSUserDefaults.standardUserDefaults setBool:NO forKey:kPreferenceOpenDiagnosticsOnLaunchKey];
    }
}

- (void)refreshDiagnostics:(id)sender {
    _textView.text = [ISHDiagnosticsStore diagnosticsReport];
    [_textView setContentOffset:CGPointZero animated:NO];
}

- (void)exportDiagnostics:(id)sender {
    NSError *error = nil;
    NSURL *bundleURL = [ISHDiagnosticsStore prepareExportBundle:&error];
    if (bundleURL == nil) {
        [self presentError:error title:@"Export failed"];
        return;
    }

    UIActivityViewController *activityViewController =
        [[UIActivityViewController alloc] initWithActivityItems:@[bundleURL] applicationActivities:nil];
    UIPopoverPresentationController *popover = activityViewController.popoverPresentationController;
    if (popover != nil) {
        popover.barButtonItem = sender;
    }
    [self presentViewController:activityViewController animated:YES completion:nil];
}

@end

static NSURL *ISHLLMPersistDirectoryURL(void) {
    NSURL *containerURL = ContainerURL();
    if (containerURL == nil)
        return nil;
    return [[containerURL URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

static NSURL *ISHLLMTranscriptURL(void) {
    NSURL *directoryURL = ISHLLMPersistDirectoryURL();
    return [directoryURL URLByAppendingPathComponent:@"llm-chat.json" isDirectory:NO];
}

static NSURL *ISHLLMExtractsDirectoryURL(void) {
    return [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-extracts" isDirectory:YES];
}

static NSString *ISHLLMSanitizeFilenameComponent(NSString *value) {
    NSMutableString *result = [NSMutableString string];
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" ];
    for (NSUInteger i = 0; i < value.length; i++) {
        unichar ch = [value characterAtIndex:i];
        if ([allowed characterIsMember:ch])
            [result appendFormat:@"%C", ch];
    }
    return result.length > 0 ? result : @"snippet";
}

static NSString *ISHLLMExtensionForFenceLanguage(NSString *language) {
    NSString *lower = language.lowercaseString;
    if ([lower isEqualToString:@"sh"] || [lower isEqualToString:@"bash"] || [lower isEqualToString:@"zsh"] || [lower isEqualToString:@"shell"])
        return @"sh";
    if ([lower isEqualToString:@"python"] || [lower isEqualToString:@"py"])
        return @"py";
    if ([lower isEqualToString:@"javascript"] || [lower isEqualToString:@"js"])
        return @"js";
    if ([lower isEqualToString:@"typescript"] || [lower isEqualToString:@"ts"])
        return @"ts";
    if ([lower isEqualToString:@"objective-c"] || [lower isEqualToString:@"objc"] || [lower isEqualToString:@"m"])
        return @"m";
    if ([lower isEqualToString:@"c"])
        return @"c";
    if ([lower isEqualToString:@"cpp"] || [lower isEqualToString:@"c++"])
        return @"cpp";
    return @"txt";
}

static NSString *ISHLLMChatEndpoint(void) {
    NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"http://localhost:11434/v1";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    if ([base hasSuffix:@"/chat/completions"])
        return base;
    return [base stringByAppendingString:@"/chat/completions"];
}

static BOOL ISHLLMUsesAppleFoundationModels(void) {
    return [UserPreferences.shared.llmProvider.lowercaseString containsString:@"foundation models"];
}

static AOKLLMBackend ISHLLMCurrentBackend(void) {
    return ISHLLMUsesAppleFoundationModels()
        ? AOKLLMBackendAppleFoundationModels
        : AOKLLMBackendOpenAICompatibleEndpoint;
}

static NSString *ISHLLMAppleFoundationModelsUnavailableMessage(void) {
    return @"Apple Foundation Models is selected, but this build cannot call it yet. The current SDK does not expose FoundationModels.framework, and the app still targets older iOS/iPadOS. When built with the iOS 26 SDK, this provider should use Apple's on-device Foundation Models backend with no server URL or API key.";
}

static BOOL ISHLLMUsesGeminiAPI(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return NO;
    NSString *provider = UserPreferences.shared.llmProvider.lowercaseString;
    NSString *host = [NSURL URLWithString:UserPreferences.shared.llmServerURL].host.lowercaseString ?: @"";
    return [provider containsString:@"gemini"] || [host containsString:@"generativelanguage.googleapis.com"];
}

static NSString *ISHLLMGeminiGenerateEndpoint(void) {
    NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"https://generativelanguage.googleapis.com/v1beta";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (![model hasPrefix:@"models/"])
        model = [@"models/" stringByAppendingString:model];
    NSString *encodedModel = [model stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet] ?: model;
    NSString *endpoint = [base stringByAppendingFormat:@"/%@:generateContent", encodedModel];
    NSString *apiKey = [UserPreferences.shared.llmAPIKey stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet] ?: @"";
    return apiKey.length > 0 ? [endpoint stringByAppendingFormat:@"?key=%@", apiKey] : endpoint;
}

static NSString *ISHLLMModelsEndpoint(void) {
    if (ISHLLMUsesGeminiAPI()) {
        NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (base.length == 0)
            base = @"https://generativelanguage.googleapis.com/v1beta";
        while ([base hasSuffix:@"/"])
            base = [base substringToIndex:base.length - 1];
        NSString *apiKey = [UserPreferences.shared.llmAPIKey stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet] ?: @"";
        NSString *endpoint = [base stringByAppendingString:@"/models"];
        return apiKey.length > 0 ? [endpoint stringByAppendingFormat:@"?key=%@", apiKey] : endpoint;
    }
    NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"https://openrouter.ai/api/v1";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    if ([base hasSuffix:@"/chat/completions"])
        base = [base substringToIndex:base.length - @"/chat/completions".length];
    if ([base hasSuffix:@"/models"])
        return base;
    return [base stringByAppendingString:@"/models"];
}

static void ISHConfigureLLMSettingsNavigationController(UINavigationController *navigationController) {
    if (@available(iOS 13.0, *)) {
        navigationController.modalPresentationStyle = UIModalPresentationFormSheet;
    } else {
        navigationController.modalPresentationStyle = UIModalPresentationPageSheet;
    }
}

static NSArray<NSDictionary<NSString *, NSString *> *> *ISHLLMProviderPresets(void) {
    return @[
        @{@"name": @"Apple Foundation Models", @"url": @"", @"model": @"system-language-model", @"format": @"Apple on-device Foundation Models"},
        @{@"name": @"OpenRouter Free", @"url": @"https://openrouter.ai/api/v1", @"model": @"openrouter/free", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Groq Llama", @"url": @"https://api.groq.com/openai/v1", @"model": @"llama-3.1-8b-instant", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Gemini Flash", @"url": @"https://generativelanguage.googleapis.com/v1beta", @"model": @"gemini-2.5-flash", @"format": @"Google Gemini generateContent"},
        @{@"name": @"LM Studio", @"url": @"http://127.0.0.1:1234/v1", @"model": @"local-model", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Ollama", @"url": @"http://127.0.0.1:11434/v1", @"model": @"llama3.2", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"OpenAI", @"url": @"https://api.openai.com/v1", @"model": @"gpt-4o-mini", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Custom", @"url": @"", @"model": @"", @"format": @"OpenAI-compatible chat completions"},
    ];
}

static NSString *ISHLLMCurrentAPIFormat(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return @"Apple on-device Foundation Models";
    if (ISHLLMUsesGeminiAPI())
        return @"Google Gemini generateContent";
    return @"OpenAI-compatible chat completions";
}

static BOOL ISHLLMProviderRequiresAPIKey(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return NO;
    NSString *provider = UserPreferences.shared.llmProvider.lowercaseString;
    NSString *host = [NSURL URLWithString:UserPreferences.shared.llmServerURL].host.lowercaseString ?: @"";
    return [provider containsString:@"openrouter"] || [provider containsString:@"openai"] ||
        [provider containsString:@"groq"] || [provider containsString:@"gemini"] ||
        [host containsString:@"openrouter.ai"] || [host containsString:@"api.openai.com"] ||
        [host containsString:@"api.groq.com"] || [host containsString:@"generativelanguage.googleapis.com"];
}

static NSString *ISHLLMMissingAPIKeyMessage(void) {
    return @"This provider requires an API key. Add it in LLM Settings -> API Key. OpenRouter free, Groq, Gemini, and OpenAI all require API keys.";
}

static NSArray<NSString *> *ISHLLMModelIdentifiersFromResponseData(NSData *data) {
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![json isKindOfClass:NSDictionary.class])
        return @[];
    NSArray *models = ISHLLMUsesGeminiAPI() ? json[@"models"] : json[@"data"];
    if (![models isKindOfClass:NSArray.class])
        return @[];
    NSMutableArray<NSString *> *ids = [NSMutableArray array];
    for (id model in models) {
        if (![model isKindOfClass:NSDictionary.class])
            continue;
        if (ISHLLMUsesGeminiAPI()) {
            NSArray *methods = model[@"supportedGenerationMethods"];
            if ([methods isKindOfClass:NSArray.class] && ![methods containsObject:@"generateContent"])
                continue;
            NSString *name = [model[@"name"] isKindOfClass:NSString.class] ? model[@"name"] : nil;
            if ([name hasPrefix:@"models/"])
                name = [name substringFromIndex:@"models/".length];
            if (name.length > 0)
                [ids addObject:name];
        } else if ([model[@"id"] isKindOfClass:NSString.class]) {
            [ids addObject:model[@"id"]];
        }
    }
    return [ids sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
}

static NSString *ISHLLMSanitizedAssistantContent(NSString *content) {
    NSRange fileSeparator = [content rangeOfString:@"<file_sep>"];
    if (fileSeparator.location != NSNotFound)
        content = [content substringToIndex:fileSeparator.location];
    return [content stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

static NSData *ISHLLMDirectHTTPPost(NSURL *url, NSData *body, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return nil;
    }

    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return nil;
    }

    int fd = -1;
    for (struct addrinfo *addr = results; addr != NULL; addr = addr->ai_next) {
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return nil;
    }

    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *headers = [NSMutableString stringWithFormat:
        @"POST %@ HTTP/1.1\r\n"
        @"Host: %@\r\n"
        @"Content-Type: application/json\r\n"
        @"Content-Length: %lu\r\n"
        @"Connection: close\r\n",
        path, hostHeader, (unsigned long) body.length];
    if (apiKey.length > 0)
        [headers appendFormat:@"Authorization: Bearer %@\r\n", apiKey];
    [headers appendString:@"\r\n"];

    NSMutableData *requestData = [NSMutableData dataWithData:[headers dataUsingEncoding:NSUTF8StringEncoding]];
    [requestData appendData:body];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }

    NSMutableData *responseData = [NSMutableData data];
    uint8_t buffer[8192];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        if (nread == 0)
            break;
        [responseData appendBytes:buffer length:(NSUInteger) nread];
    }
    close(fd);

    NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSRange separatorRange = [responseData rangeOfData:separator options:0 range:NSMakeRange(0, responseData.length)];
    if (separatorRange.location == NSNotFound) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotParseResponse userInfo:nil];
        return nil;
    }

    NSData *headerData = [responseData subdataWithRange:NSMakeRange(0, separatorRange.location)];
    NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
    NSArray<NSString *> *headerLines = [headerText componentsSeparatedByString:@"\r\n"];
    NSArray<NSString *> *statusParts = [headerLines.firstObject componentsSeparatedByString:@" "];
    if (statusParts.count >= 2 && statusCodeOut != NULL)
        *statusCodeOut = statusParts[1].integerValue;

    NSUInteger bodyOffset = NSMaxRange(separatorRange);
    return [responseData subdataWithRange:NSMakeRange(bodyOffset, responseData.length - bodyOffset)];
}

static NSData *ISHLLMDirectHTTPGet(NSURL *url, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return nil;
    }
    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return nil;
    }
    int fd = -1;
    for (struct addrinfo *addr = results; addr != NULL; addr = addr->ai_next) {
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return nil;
    }
    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *requestText = [NSMutableString stringWithFormat:
        @"GET %@ HTTP/1.1\r\n"
        @"Host: %@\r\n"
        @"Accept: application/json\r\n"
        @"Connection: close\r\n",
        path, hostHeader];
    if (apiKey.length > 0)
        [requestText appendFormat:@"Authorization: Bearer %@\r\n", apiKey];
    [requestText appendString:@"\r\n"];
    NSData *requestData = [requestText dataUsingEncoding:NSUTF8StringEncoding];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }
    NSMutableData *responseData = [NSMutableData data];
    uint8_t buffer[8192];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        if (nread == 0)
            break;
        [responseData appendBytes:buffer length:(NSUInteger) nread];
    }
    close(fd);
    NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSRange separatorRange = [responseData rangeOfData:separator options:0 range:NSMakeRange(0, responseData.length)];
    if (separatorRange.location == NSNotFound)
        return responseData;
    NSData *headerData = [responseData subdataWithRange:NSMakeRange(0, separatorRange.location)];
    NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
    NSArray<NSString *> *statusParts = [[headerText componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
    if (statusParts.count >= 2 && statusCodeOut != NULL)
        *statusCodeOut = statusParts[1].integerValue;
    NSUInteger bodyOffset = NSMaxRange(separatorRange);
    return [responseData subdataWithRange:NSMakeRange(bodyOffset, responseData.length - bodyOffset)];
}

static NSString *ISHLLMContentFromStreamingPayload(NSString *payload) {
    NSData *data = [payload dataUsingEncoding:NSUTF8StringEncoding];
    if (data.length == 0)
        return nil;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:NSDictionary.class])
        return nil;
    NSArray *choices = json[@"choices"];
    NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
    NSDictionary *delta = [choice[@"delta"] isKindOfClass:NSDictionary.class] ? choice[@"delta"] : nil;
    NSString *content = [delta[@"content"] isKindOfClass:NSString.class] ? delta[@"content"] : nil;
    if (content.length == 0) {
        NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
        content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : nil;
    }
    return content;
}

static BOOL ISHLLMDirectHTTPPostStreaming(NSURL *url,
                                          NSData *body,
                                          NSString *apiKey,
                                          void (^chunkHandler)(NSString *chunk),
                                          NSInteger *statusCodeOut,
                                          NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return NO;
    }

    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return NO;
    }

    int fd = -1;
    for (struct addrinfo *addr = results; addr != NULL; addr = addr->ai_next) {
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return NO;
    }

    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *headers = [NSMutableString stringWithFormat:
        @"POST %@ HTTP/1.0\r\n"
        @"Host: %@\r\n"
        @"Content-Type: application/json\r\n"
        @"Accept: text/event-stream\r\n"
        @"Content-Length: %lu\r\n",
        path, hostHeader, (unsigned long) body.length];
    if (apiKey.length > 0)
        [headers appendFormat:@"Authorization: Bearer %@\r\n", apiKey];
    [headers appendString:@"\r\n"];

    NSMutableData *requestData = [NSMutableData dataWithData:[headers dataUsingEncoding:NSUTF8StringEncoding]];
    [requestData appendData:body];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return NO;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }

    NSMutableData *bufferedData = [NSMutableData data];
    NSMutableString *eventBuffer = [NSMutableString string];
    BOOL parsedHeaders = NO;
    uint8_t buffer[4096];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return NO;
        }
        if (nread == 0)
            break;
        [bufferedData appendBytes:buffer length:(NSUInteger) nread];

        if (!parsedHeaders) {
            NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
            NSRange separatorRange = [bufferedData rangeOfData:separator options:0 range:NSMakeRange(0, bufferedData.length)];
            if (separatorRange.location == NSNotFound)
                continue;
            NSData *headerData = [bufferedData subdataWithRange:NSMakeRange(0, separatorRange.location)];
            NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
            NSArray<NSString *> *statusParts = [[headerText componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
            if (statusParts.count >= 2 && statusCodeOut != NULL)
                *statusCodeOut = statusParts[1].integerValue;
            NSUInteger bodyOffset = NSMaxRange(separatorRange);
            NSData *remainingBody = [bufferedData subdataWithRange:NSMakeRange(bodyOffset, bufferedData.length - bodyOffset)];
            [bufferedData setData:remainingBody];
            parsedHeaders = YES;
        }

        NSString *text = [[NSString alloc] initWithData:bufferedData encoding:NSUTF8StringEncoding];
        if (text.length == 0)
            continue;
        [bufferedData setLength:0];
        [eventBuffer appendString:text];
        for (;;) {
            NSRange newlineRange = [eventBuffer rangeOfString:@"\n"];
            if (newlineRange.location == NSNotFound)
                break;
            NSString *line = [[eventBuffer substringToIndex:newlineRange.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            [eventBuffer deleteCharactersInRange:NSMakeRange(0, NSMaxRange(newlineRange))];
            if (![line hasPrefix:@"data:"])
                continue;
            NSString *payload = [[line substringFromIndex:5] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if ([payload isEqualToString:@"[DONE]"])
                continue;
            NSString *content = ISHLLMContentFromStreamingPayload(payload);
            if (content.length > 0 && chunkHandler != nil)
                chunkHandler(content);
        }
    }
    close(fd);
    return YES;
}

@implementation LLMClientViewController {
    UIStackView *_toolbarStackView;
    UITextView *_transcriptView;
    UITextField *_promptField;
    UIButton *_sendButton;
    NSMutableArray<NSDictionary<NSString *, id> *> *_messages;
    NSURLSessionDataTask *_activeTask;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"LLM Chat";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _messages = [NSMutableArray array];
    NSArray *storedMessages = [NSArray arrayWithContentsOfURL:ISHLLMTranscriptURL()];
    if ([storedMessages isKindOfClass:NSArray.class]) {
        for (id message in storedMessages) {
            if ([message isKindOfClass:NSDictionary.class] && [message[@"role"] isKindOfClass:NSString.class] && [message[@"content"] isKindOfClass:NSString.class])
                [_messages addObject:message];
        }
    }

    _transcriptView = [[UITextView alloc] initWithFrame:CGRectZero];
    _transcriptView.translatesAutoresizingMaskIntoConstraints = NO;
    _transcriptView.editable = NO;
    _transcriptView.alwaysBounceVertical = YES;
    _transcriptView.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    if (@available(iOS 13.0, *)) {
        _transcriptView.backgroundColor = UIColor.systemBackgroundColor;
        _transcriptView.textColor = UIColor.labelColor;
    }
    [self.view addSubview:_transcriptView];

    UIView *inputBar = [UIView new];
    inputBar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:inputBar];

    _promptField = [UITextField new];
    _promptField.translatesAutoresizingMaskIntoConstraints = NO;
    _promptField.borderStyle = UITextBorderStyleRoundedRect;
    _promptField.placeholder = @"Ask the configured model";
    _promptField.returnKeyType = UIReturnKeySend;
    _promptField.autocorrectionType = UITextAutocorrectionTypeDefault;
    _promptField.delegate = self;
    [inputBar addSubview:_promptField];

    _sendButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _sendButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_sendButton setTitle:@"Send" forState:UIControlStateNormal];
    [_sendButton addTarget:self action:@selector(sendPrompt:) forControlEvents:UIControlEventTouchUpInside];
    [inputBar addSubview:_sendButton];

    _toolbarStackView = [UIStackView new];
    _toolbarStackView.translatesAutoresizingMaskIntoConstraints = NO;
    _toolbarStackView.axis = UILayoutConstraintAxisHorizontal;
    _toolbarStackView.alignment = UIStackViewAlignmentCenter;
    _toolbarStackView.distribution = UIStackViewDistributionFillEqually;
    _toolbarStackView.spacing = 6.0;
    [self.view addSubview:_toolbarStackView];
    NSArray<NSDictionary<NSString *, NSString *> *> *toolbarButtons = @[
        @{@"title": @"Settings", @"selector": NSStringFromSelector(@selector(showLLMSettings:))},
        @{@"title": @"Actions", @"selector": NSStringFromSelector(@selector(showPromptActions:))},
        @{@"title": @"Save", @"selector": NSStringFromSelector(@selector(showExtractActions:))},
        @{@"title": @"Clear", @"selector": NSStringFromSelector(@selector(clearTranscript:))},
    ];
    for (NSDictionary<NSString *, NSString *> *descriptor in toolbarButtons) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
        [button setTitle:descriptor[@"title"] forState:UIControlStateNormal];
        [button addTarget:self action:NSSelectorFromString(descriptor[@"selector"]) forControlEvents:UIControlEventTouchUpInside];
        [_toolbarStackView addArrangedSubview:button];
    }

    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithTitle:@"Settings" style:UIBarButtonItemStylePlain target:self action:@selector(showLLMSettings:)],
        [[UIBarButtonItem alloc] initWithTitle:@"Actions" style:UIBarButtonItemStylePlain target:self action:@selector(showPromptActions:)],
        [[UIBarButtonItem alloc] initWithTitle:@"Save" style:UIBarButtonItemStylePlain target:self action:@selector(showExtractActions:)],
        [[UIBarButtonItem alloc] initWithTitle:@"Clear" style:UIBarButtonItemStylePlain target:self action:@selector(clearTranscript:)],
    ];

    UILayoutGuide *safeArea = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_toolbarStackView.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:6.0],
        [_toolbarStackView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:10.0],
        [_toolbarStackView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-10.0],
        [_toolbarStackView.heightAnchor constraintGreaterThanOrEqualToConstant:32.0],

        [_transcriptView.topAnchor constraintEqualToAnchor:_toolbarStackView.bottomAnchor constant:4.0],
        [_transcriptView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [_transcriptView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [_transcriptView.bottomAnchor constraintEqualToAnchor:inputBar.topAnchor],

        [inputBar.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:10.0],
        [inputBar.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-10.0],
        [inputBar.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-8.0],
        [inputBar.heightAnchor constraintGreaterThanOrEqualToConstant:44.0],

        [_promptField.leadingAnchor constraintEqualToAnchor:inputBar.leadingAnchor],
        [_promptField.topAnchor constraintEqualToAnchor:inputBar.topAnchor constant:4.0],
        [_promptField.bottomAnchor constraintEqualToAnchor:inputBar.bottomAnchor constant:-4.0],
        [_sendButton.leadingAnchor constraintEqualToAnchor:_promptField.trailingAnchor constant:8.0],
        [_sendButton.trailingAnchor constraintEqualToAnchor:inputBar.trailingAnchor],
        [_sendButton.centerYAnchor constraintEqualToAnchor:_promptField.centerYAnchor],
        [_sendButton.widthAnchor constraintEqualToConstant:56.0],
    ]];

    [self refreshTranscript];
    if (self.initialPrompt.length > 0)
        _promptField.text = self.initialPrompt;
}

- (void)dealloc {
    [_activeTask cancel];
}

- (void)showLLMSettings:(id)sender {
    (void) sender;
    UIViewController *settingsViewController = ISHCreateLLMSettingsViewController();
    if (self.navigationController != nil) {
        [self.navigationController pushViewController:settingsViewController animated:YES];
    } else {
        UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:settingsViewController];
        ISHConfigureLLMSettingsNavigationController(navigationController);
        [self presentViewController:navigationController animated:YES completion:nil];
    }
}

- (void)clearTranscript:(id)sender {
    (void) sender;
    [_messages removeAllObjects];
    [self saveTranscript];
    [self refreshTranscript];
}

- (NSString *)latestAssistantMessage {
    for (NSDictionary<NSString *, id> *message in _messages.reverseObjectEnumerator) {
        if ([message[@"role"] isEqualToString:@"assistant"])
            return [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
    }
    return @"";
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)extractCodeBlocksFromText:(NSString *)text {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *blocks = [NSMutableArray array];
    NSUInteger searchStart = 0;
    while (searchStart < text.length) {
        NSRange fenceStart = [text rangeOfString:@"```" options:0 range:NSMakeRange(searchStart, text.length - searchStart)];
        if (fenceStart.location == NSNotFound)
            break;
        NSUInteger languageStart = NSMaxRange(fenceStart);
        NSRange languageLine = [text rangeOfString:@"\n" options:0 range:NSMakeRange(languageStart, text.length - languageStart)];
        if (languageLine.location == NSNotFound)
            break;
        NSString *language = [[text substringWithRange:NSMakeRange(languageStart, languageLine.location - languageStart)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSUInteger codeStart = NSMaxRange(languageLine);
        NSRange fenceEnd = [text rangeOfString:@"```" options:0 range:NSMakeRange(codeStart, text.length - codeStart)];
        if (fenceEnd.location == NSNotFound)
            break;
        NSString *code = [text substringWithRange:NSMakeRange(codeStart, fenceEnd.location - codeStart)];
        [blocks addObject:@{@"language": language ?: @"", @"code": code ?: @""}];
        searchStart = NSMaxRange(fenceEnd);
    }
    return blocks;
}

- (void)showExtractActions:(id)sender {
    (void) sender;
    NSArray<NSDictionary<NSString *, NSString *> *> *blocks = [self extractCodeBlocksFromText:self.latestAssistantMessage];
    NSString *selectedText = [self selectedTranscriptText];
    NSString *savePath = @"/AOK/persist/llm-extracts";
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Save From Chat"
                                                                   message:(blocks.count > 0 || selectedText.length > 0) ? [@"Save destination: " stringByAppendingString:savePath] : [@"No highlighted text or fenced code blocks found. Save destination: " stringByAppendingString:savePath]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    if (selectedText.length > 0) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Save Highlighted Text" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [self saveExtractText:selectedText extension:@"txt" label:@"highlighted"];
        }]];
    }
    for (NSUInteger i = 0; i < blocks.count; i++) {
        NSDictionary<NSString *, NSString *> *block = blocks[i];
        NSString *language = block[@"language"].length > 0 ? block[@"language"] : @"text";
        NSString *title = [NSString stringWithFormat:@"Save block %lu (%@)", (unsigned long) i + 1, language];
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [self saveCodeBlock:block index:i + 1];
        }]];
    }
    if (blocks.count > 1) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Save All Blocks" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            for (NSUInteger i = 0; i < blocks.count; i++)
                [self saveCodeBlock:blocks[i] index:i + 1];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil)
        popover.barButtonItem = sender;
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)selectedTranscriptText {
    NSRange selectedRange = _transcriptView.selectedRange;
    if (selectedRange.length == 0 || NSMaxRange(selectedRange) > _transcriptView.text.length)
        return @"";
    return [_transcriptView.text substringWithRange:selectedRange];
}

- (void)saveCodeBlock:(NSDictionary<NSString *, NSString *> *)block index:(NSUInteger)index {
    NSString *language = block[@"language"] ?: @"";
    NSString *ext = ISHLLMExtensionForFenceLanguage(language);
    [self saveExtractText:block[@"code"] extension:ext label:[NSString stringWithFormat:@"block-%lu", (unsigned long) index]];
}

- (void)saveExtractText:(NSString *)text extension:(NSString *)ext label:(NSString *)label {
    NSURL *directoryURL = ISHLLMExtractsDirectoryURL();
    [NSFileManager.defaultManager createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *filename = [NSString stringWithFormat:@"llm-%@-%@.%@", [formatter stringFromDate:NSDate.date], label, ext ?: @"txt"];
    NSURL *url = [directoryURL URLByAppendingPathComponent:ISHLLMSanitizeFilenameComponent(filename) isDirectory:NO];
    NSError *error = nil;
    BOOL ok = [text writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error];
    NSString *ishPath = [@"/AOK/persist/llm-extracts" stringByAppendingPathComponent:url.lastPathComponent];
    NSString *message = ok ? [NSString stringWithFormat:@"Saved %@", ishPath] : (error.localizedDescription ?: @"Save failed");
    [self appendRole:@"assistant" content:message];
}

- (NSString *)terminalContextPromptWithInstruction:(NSString *)instruction {
    Terminal *terminal = Terminal.activeTerminals.firstObject;
    NSString *context = terminal != nil ? Terminal_debugReadRows(terminal.type, terminal.number, 80) : @"";
    if (context.length == 0)
        context = @"No active terminal output was available.";
    return [NSString stringWithFormat:@"%@\n\nTerminal output:\n```text\n%@\n```", instruction, context];
}

- (void)showPromptActions:(id)sender {
    (void) sender;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Prompt Actions" message:@"Use terminal context or saved prompt templates." preferredStyle:UIAlertControllerStyleActionSheet];
    NSArray<NSDictionary<NSString *, NSString *> *> *actions = @[
        @{@"title": @"Explain terminal output", @"instruction": @"Explain the important details in this terminal output. If there is an error, identify the likely cause."},
        @{@"title": @"Suggest fix for error", @"instruction": @"Find the most likely error in this terminal output and suggest concrete commands or edits to fix it."},
        @{@"title": @"Draft shell command", @"instruction": @"Based on this terminal context, draft the next safe shell command. Explain briefly before the command."},
    ];
    for (NSDictionary<NSString *, NSString *> *descriptor in actions) {
        [alert addAction:[UIAlertAction actionWithTitle:descriptor[@"title"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            self->_promptField.text = [self terminalContextPromptWithInstruction:descriptor[@"instruction"]];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Load Prompt Template" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self showPromptTemplatePickerFromSender:sender];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil)
        popover.barButtonItem = sender;
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showPromptTemplatePickerFromSender:(id)sender {
    NSURL *templatesURL = [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-prompts" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:templatesURL withIntermediateDirectories:YES attributes:nil error:nil];
    NSArray<NSURL *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtURL:templatesURL includingPropertiesForKeys:nil options:0 error:nil];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Prompt Templates" message:@"Templates are text files in /AOK/persist/llm-prompts." preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSURL *fileURL in files) {
        if (fileURL.lastPathComponent.length == 0)
            continue;
        [alert addAction:[UIAlertAction actionWithTitle:fileURL.lastPathComponent style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            NSString *template = [NSString stringWithContentsOfURL:fileURL encoding:NSUTF8StringEncoding error:nil];
            if (template.length > 0)
                self->_promptField.text = template;
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Create Examples" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [@"Review this code for correctness, portability, and security.\n\n```\nPASTE_CODE_HERE\n```\n" writeToURL:[templatesURL URLByAppendingPathComponent:@"code-review.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [@"Turn this into a robust shell script with error handling:\n\n" writeToURL:[templatesURL URLByAppendingPathComponent:@"make-script.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [self appendRole:@"assistant" content:@"Created example prompt templates in /AOK/persist/llm-prompts."];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil)
        popover.barButtonItem = sender;
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)appendRole:(NSString *)role content:(NSString *)content {
    if (content.length == 0)
        return;
    [_messages addObject:@{@"role": role, @"content": content}];
    [self saveTranscript];
    [self refreshTranscript];
}

- (void)appendLocalRole:(NSString *)role content:(NSString *)content {
    if (content.length == 0)
        return;
    [_messages addObject:@{@"role": role, @"content": content, @"local": @"1"}];
    [self saveTranscript];
    [self refreshTranscript];
}

- (BOOL)messageIsLocalOnly:(NSDictionary<NSString *, id> *)message {
    if ([message[@"local"] isEqual:@"1"])
        return YES;
    NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
    NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
    if ([role isEqualToString:@"user"] && [content hasPrefix:@"/"])
        return YES;
    if ([role isEqualToString:@"assistant"]) {
        if ([content hasPrefix:@"Model set to "] || [content hasPrefix:@"Current model:"] ||
            [content hasPrefix:@"Model query failed:"] || [content hasPrefix:@"No models found"] ||
            [content hasPrefix:@"Invalid models URL"] || [content containsString:@" models returned by "])
            return YES;
    }
    return NO;
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)providerMessages {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *messages = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *message in _messages) {
        if ([self messageIsLocalOnly:message])
            continue;
        NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : nil;
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : nil;
        if (role.length > 0 && content.length > 0)
            [messages addObject:@{@"role": role, @"content": content}];
    }
    return messages;
}

- (void)saveTranscript {
    NSURL *directoryURL = ISHLLMPersistDirectoryURL();
    if (directoryURL != nil)
        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:nil];
    [_messages writeToURL:ISHLLMTranscriptURL() atomically:YES];
}

- (void)refreshTranscript {
    NSMutableString *text = [NSMutableString string];
    for (NSDictionary<NSString *, id> *message in _messages) {
        NSString *messageRole = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
        NSString *role = [messageRole isEqualToString:@"assistant"] ? @"Assistant" : @"You";
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        [text appendFormat:@"%@: %@\n\n", role, content];
    }
    if (text.length == 0) {
        [text appendFormat:@"Configure an OpenAI-compatible server in Settings, then send a prompt.\n\nServer: %@\nModel: %@\n",
         UserPreferences.shared.llmServerURL, UserPreferences.shared.llmModel];
    }
    _transcriptView.text = text;
    NSRange bottom = NSMakeRange(_transcriptView.text.length, 0);
    [_transcriptView scrollRangeToVisible:bottom];
}

- (void)setSending:(BOOL)sending {
    _sendButton.enabled = !sending;
    _promptField.enabled = !sending;
    [_sendButton setTitle:(sending ? @"..." : @"Send") forState:UIControlStateNormal];
}

- (void)handleLLMResponseData:(NSData *)data response:(NSURLResponse *)response error:(NSError *)error {
    [self setSending:NO];
    _activeTask = nil;
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        return;
    }
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *content = nil;
    if ([json isKindOfClass:NSDictionary.class]) {
        NSDictionary *dict = json;
        NSArray *choices = dict[@"choices"];
        NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
        NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
        content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : nil;
        content = ISHLLMSanitizedAssistantContent(content ?: @"");
        if (content.length == 0 && [dict[@"error"] isKindOfClass:NSDictionary.class])
            content = dict[@"error"][@"message"];
    }
    if (content.length == 0) {
        NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
        content = [NSString stringWithFormat:@"Unexpected response%@%@", http != nil ? [NSString stringWithFormat:@" (%ld)", (long) http.statusCode] : @"", raw.length > 0 ? [@": " stringByAppendingString:raw] : @"."];
    }
    [self appendRole:@"assistant" content:ISHLLMSanitizedAssistantContent(content)];
}

- (void)handleGeminiResponseData:(NSData *)data response:(NSURLResponse *)response error:(NSError *)error {
    [self setSending:NO];
    _activeTask = nil;
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        return;
    }
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *content = nil;
    if ([json isKindOfClass:NSDictionary.class]) {
        NSDictionary *dict = json;
        NSArray *candidates = dict[@"candidates"];
        NSDictionary *candidate = candidates.count > 0 && [candidates[0] isKindOfClass:NSDictionary.class] ? candidates[0] : nil;
        NSDictionary *candidateContent = [candidate[@"content"] isKindOfClass:NSDictionary.class] ? candidate[@"content"] : nil;
        NSArray *parts = [candidateContent[@"parts"] isKindOfClass:NSArray.class] ? candidateContent[@"parts"] : nil;
        NSMutableString *text = [NSMutableString string];
        for (id part in parts) {
            if ([part isKindOfClass:NSDictionary.class] && [part[@"text"] isKindOfClass:NSString.class])
                [text appendString:part[@"text"]];
        }
        content = ISHLLMSanitizedAssistantContent(text);
        if (content.length == 0 && [dict[@"error"] isKindOfClass:NSDictionary.class])
            content = dict[@"error"][@"message"];
    }
    if (content.length == 0) {
        NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
        content = [NSString stringWithFormat:@"Unexpected Gemini response%@%@", http != nil ? [NSString stringWithFormat:@" (%ld)", (long) http.statusCode] : @"", raw.length > 0 ? [@": " stringByAppendingString:raw] : @"."];
    }
    [self appendRole:@"assistant" content:ISHLLMSanitizedAssistantContent(content)];
}

- (void)appendStreamingAssistantChunk:(NSString *)chunk toMessageAtIndex:(NSUInteger)index {
    if (index >= _messages.count || chunk.length == 0)
        return;
    NSMutableDictionary<NSString *, NSString *> *message = [_messages[index] mutableCopy];
    NSString *content = ISHLLMSanitizedAssistantContent([message[@"content"] ?: @"" stringByAppendingString:chunk]);
    message[@"content"] = content;
    _messages[index] = message;
    [self refreshTranscript];
}

- (void)appendModelListFromData:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error {
    [self setSending:NO];
    if (error != nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model query failed: %@", error.localizedDescription]];
        return;
    }
    NSArray<NSString *> *models = ISHLLMModelIdentifiersFromResponseData(data);
    if (models.count == 0) {
        NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
        NSString *message = statusCode > 0 ? [NSString stringWithFormat:@"No models found. HTTP %ld", (long) statusCode] : @"No models found.";
        if (raw.length > 0)
            message = [message stringByAppendingFormat:@"\n%@", raw.length > 480 ? [raw substringToIndex:480] : raw];
        [self appendLocalRole:@"assistant" content:message];
        return;
    }
    NSUInteger limit = MIN(models.count, 80);
    NSMutableString *message = [NSMutableString stringWithFormat:@"%lu models returned by %@:", (unsigned long) models.count, ISHLLMModelsEndpoint()];
    for (NSUInteger i = 0; i < limit; i++)
        [message appendFormat:@"\n- %@", models[i]];
    if (models.count > limit)
        [message appendFormat:@"\nShowing first %lu of %lu.", (unsigned long) limit, (unsigned long) models.count];
    [self appendLocalRole:@"assistant" content:message];
}

- (void)queryModelsInTranscript {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Current on-device model: %@\n%@", UserPreferences.shared.llmModel.length > 0 ? UserPreferences.shared.llmModel : @"system-language-model", ISHLLMAppleFoundationModelsUnavailableMessage()]];
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
    if (url == nil) {
        [self appendLocalRole:@"assistant" content:@"Invalid models URL."];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self appendLocalRole:@"assistant" content:ISHLLMMissingAPIKeyMessage()];
        return;
    }
    [self setSending:YES];
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil)
                    [self appendModelListFromData:data statusCode:statusCode error:error];
            });
        });
        return;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    __weak typeof(self) weakSelf = self;
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self appendModelListFromData:data statusCode:http.statusCode error:error];
        });
    }];
    [_activeTask resume];
}

- (void)appendModelLoadResultWithModel:(NSString *)model data:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error {
    [self setSending:NO];
    _activeTask = nil;
    if (error != nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@, but load failed: %@", model, error.localizedDescription]];
        return;
    }
    if (statusCode >= 200 && statusCode < 300) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. Provider accepted a warm-up request.", model]];
        return;
    }
    NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
    NSString *message = [NSString stringWithFormat:@"Model set to %@, but provider returned HTTP %ld.", model, (long) statusCode];
    if (raw.length > 0)
        message = [message stringByAppendingFormat:@"\n%@", raw.length > 480 ? [raw substringToIndex:480] : raw];
    [self appendLocalRole:@"assistant" content:message];
}

- (void)setAndLoadModelFromCommand:(NSString *)command {
    NSString *model = [[command substringFromIndex:@"/model".length] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (model.length == 0) {
        NSString *current = UserPreferences.shared.llmModel.length > 0 ? UserPreferences.shared.llmModel : @"not set";
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Current model: %@\nUsage: /model <model-name>", current]];
        return;
    }
    UserPreferences.shared.llmModel = model;
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. Apple Foundation Models uses the system on-device model when available. %@", model, ISHLLMAppleFoundationModelsUnavailableMessage()]];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. %@", model, ISHLLMMissingAPIKeyMessage()]];
        return;
    }

    NSURL *url = [NSURL URLWithString:ISHLLMUsesGeminiAPI() ? ISHLLMGeminiGenerateEndpoint() : ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@, but the provider URL is invalid.", model]];
        return;
    }
    NSDictionary *body = ISHLLMUsesGeminiAPI()
        ? @{@"contents": @[@{@"role": @"user", @"parts": @[@{@"text": @"Reply with ok."}]}]}
        : @{
            @"model": model,
            @"messages": @[@{@"role": @"user", @"content": @"Reply with ok."}],
            @"stream": @NO,
            @"max_tokens": @1,
        };
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    [self setSending:YES];
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPPost(url, bodyData, apiKey, &statusCode, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil)
                    [self appendModelLoadResultWithModel:model data:data statusCode:statusCode error:error];
            });
        });
        return;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    request.HTTPBody = bodyData;
    __weak typeof(self) weakSelf = self;
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self appendModelLoadResultWithModel:model data:data statusCode:http.statusCode error:error];
        });
    }];
    [_activeTask resume];
}

- (void)sendPrompt:(id)sender {
    (void) sender;
    NSString *prompt = [_promptField.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (prompt.length == 0)
        return;
    if ([prompt isEqualToString:@"/models"]) {
        _promptField.text = @"";
        [self appendLocalRole:@"user" content:prompt];
        [self queryModelsInTranscript];
        return;
    }
    if ([prompt isEqualToString:@"/model"] || [prompt hasPrefix:@"/model "]) {
        _promptField.text = @"";
        [self appendLocalRole:@"user" content:prompt];
        [self setAndLoadModelFromCommand:prompt];
        return;
    }
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        _promptField.text = @"";
        [self appendLocalRole:@"user" content:prompt];
        [self appendLocalRole:@"assistant" content:ISHLLMAppleFoundationModelsUnavailableMessage()];
        return;
    }
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (model.length == 0) {
        [self appendRole:@"assistant" content:@"Set an LLM model in Settings before sending a prompt."];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self appendRole:@"assistant" content:ISHLLMMissingAPIKeyMessage()];
        return;
    }

    _promptField.text = @"";
    [self appendRole:@"user" content:prompt];
    [self setSending:YES];

    if (ISHLLMUsesGeminiAPI()) {
        NSURL *geminiURL = [NSURL URLWithString:ISHLLMGeminiGenerateEndpoint()];
        if (geminiURL == nil) {
            [self appendRole:@"assistant" content:@"Invalid Gemini server URL."];
            [self setSending:NO];
            return;
        }
        NSMutableArray<NSDictionary<NSString *, id> *> *contents = [NSMutableArray array];
        for (NSDictionary<NSString *, NSString *> *message in [self providerMessages]) {
            NSString *role = [message[@"role"] isEqualToString:@"assistant"] ? @"model" : @"user";
            NSString *content = message[@"content"] ?: @"";
            if (content.length > 0)
                [contents addObject:@{@"role": role, @"parts": @[@{@"text": content}]}];
        }
        NSDictionary *body = @{@"contents": contents};
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:geminiURL];
        request.HTTPMethod = @"POST";
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
        __weak typeof(self) weakSelf = self;
        _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil)
                    [self handleGeminiResponseData:data response:response error:error];
            });
        }];
        [_activeTask resume];
        return;
    }

    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendRole:@"assistant" content:@"Invalid LLM server URL."];
        [self setSending:NO];
        return;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    if (apiKey.length > 0)
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];

    BOOL useDirectHTTP = [[url.scheme lowercaseString] isEqualToString:@"http"];
    NSDictionary *body = @{
        @"model": model,
        @"messages": [self providerMessages],
        @"stream": @(useDirectHTTP),
        @"stop": @[@"<file_sep>"],
    };
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];

    if (useDirectHTTP) {
        NSData *requestBody = request.HTTPBody;
        NSData *fallbackRequestBody = [NSJSONSerialization dataWithJSONObject:@{
            @"model": model,
            @"messages": [self providerMessages],
            @"stream": @NO,
            @"stop": @[@"<file_sep>"],
        } options:0 error:nil];
        NSString *requestAPIKey = apiKey;
        [_messages addObject:@{@"role": @"assistant", @"content": @""}];
        NSUInteger streamingIndex = _messages.count - 1;
        [self refreshTranscript];
        __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *directError = nil;
            __block BOOL receivedChunk = NO;
            BOOL streamed = ISHLLMDirectHTTPPostStreaming(url, requestBody, requestAPIKey, ^(NSString *chunk) {
                receivedChunk = YES;
                dispatch_async(dispatch_get_main_queue(), ^{
                    typeof(self) self = weakSelf;
                    if (self != nil)
                        [self appendStreamingAssistantChunk:chunk toMessageAtIndex:streamingIndex];
                });
            }, &statusCode, &directError);
            NSData *responseBody = nil;
            if (!streamed || !receivedChunk)
                responseBody = ISHLLMDirectHTTPPost(url, fallbackRequestBody, requestAPIKey, &statusCode, &directError);
            NSHTTPURLResponse *directResponse = nil;
            if (statusCode > 0) {
                directResponse = [[NSHTTPURLResponse alloc] initWithURL:url
                                                             statusCode:statusCode
                                                            HTTPVersion:@"HTTP/1.1"
                                                           headerFields:nil];
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self == nil)
                    return;
                if (streamed && receivedChunk && directError == nil) {
                    [self setSending:NO];
                    [self saveTranscript];
                    return;
                }
                if (streamingIndex < self->_messages.count)
                    [self->_messages removeObjectAtIndex:streamingIndex];
                [self handleLLMResponseData:responseBody response:directResponse error:directError];
            });
        });
        return;
    }

    __weak typeof(self) weakSelf = self;
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            [self handleLLMResponseData:data response:response error:error];
        });
    }];
    [_activeTask resume];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    (void) textField;
    [self sendPrompt:nil];
    return YES;
}

@end

@implementation LLMSettingsViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"LLM Client";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                           target:self
                                                                                           action:@selector(done:)];
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                          target:self
                                                                                          action:@selector(done:)];
}

- (void)done:(id)sender {
    (void) sender;
    if (self.navigationController.viewControllers.firstObject == self) {
        UIViewController *presenter = self.navigationController.presentingViewController ?: self.presentingViewController;
        if (presenter != nil)
            [presenter dismissViewControllerAnimated:YES completion:nil];
    } else {
        [self.navigationController popViewControllerAnimated:YES];
    }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void) tableView;
    return 2;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return 1;
    return 7;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return nil;
    if (ISHLLMUsesAppleFoundationModels())
        return @"Apple Foundation Models is an iOS/iPadOS 26+ on-device backend. This build exposes the provider setting, but runtime calls require building with an SDK that includes FoundationModels.framework. Chat history is saved in /AOK/persist/llm-chat.json.";
    return @"Use a /v1 OpenAI-compatible server, or the Gemini preset. Hosted providers require API keys.\nChat history is saved in /AOK/persist/llm-chat.json.";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    if (indexPath.section == 0) {
        cell.textLabel.text = @"Back to Chat";
        cell.detailTextLabel.text = @"Done";
        cell.accessoryType = UITableViewCellAccessoryNone;
        return cell;
    }
    if (indexPath.row == 0) {
        cell.textLabel.text = @"Provider";
        cell.detailTextLabel.text = UserPreferences.shared.llmProvider;
    } else if (indexPath.row == 1) {
        cell.textLabel.text = @"Server URL";
        cell.detailTextLabel.text = ISHLLMUsesAppleFoundationModels() ? @"On-device" : UserPreferences.shared.llmServerURL;
        if (ISHLLMUsesAppleFoundationModels())
            cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.row == 2) {
        cell.textLabel.text = @"Model";
        cell.detailTextLabel.text = UserPreferences.shared.llmModel;
    } else if (indexPath.row == 3) {
        cell.textLabel.text = @"API Format";
        cell.detailTextLabel.text = ISHLLMCurrentAPIFormat();
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.row == 4) {
        cell.textLabel.text = @"API Key";
        cell.detailTextLabel.text = ISHLLMUsesAppleFoundationModels() ? @"Not used" : (UserPreferences.shared.llmAPIKey.length > 0 ? @"Set" : (ISHLLMProviderRequiresAPIKey() ? @"Required" : @"Optional"));
        if (ISHLLMUsesAppleFoundationModels())
            cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.row == 5) {
        cell.textLabel.text = @"Query Models";
        cell.detailTextLabel.text = @"/models";
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else {
        cell.textLabel.text = @"Test Connection";
        cell.detailTextLabel.text = @"";
        cell.accessoryType = UITableViewCellAccessoryNone;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 0) {
        [self done:nil];
        return;
    }
    if (indexPath.row == 0) {
        [self showProviderPicker];
        return;
    }
    if (indexPath.row == 3)
        return;
    if (ISHLLMUsesAppleFoundationModels() && (indexPath.row == 1 || indexPath.row == 4))
        return;
    if (indexPath.row == 5) {
        [self queryAvailableModels];
        return;
    }
    if (indexPath.row == 6) {
        [self testConnection];
        return;
    }
    NSString *title = indexPath.row == 1 ? @"Server URL" : (indexPath.row == 2 ? @"Model" : @"API Key");
    NSString *current = indexPath.row == 1 ? UserPreferences.shared.llmServerURL : (indexPath.row == 2 ? UserPreferences.shared.llmModel : UserPreferences.shared.llmAPIKey);
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = current;
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        if (indexPath.row == 4)
            textField.secureTextEntry = YES;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *value = alert.textFields.firstObject.text ?: @"";
        if (indexPath.row == 1)
            UserPreferences.shared.llmServerURL = value;
        else if (indexPath.row == 2)
            UserPreferences.shared.llmModel = value;
        else
            UserPreferences.shared.llmAPIKey = value;
        [self.tableView reloadData];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showProviderPicker {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"LLM Provider"
                                                                   message:@"Choose a provider preset. Custom values can still be edited afterward."
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *preset in ISHLLMProviderPresets()) {
        NSString *name = preset[@"name"];
        NSString *title = [name isEqualToString:UserPreferences.shared.llmProvider]
            ? [name stringByAppendingString:@"  Current"]
            : name;
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmProvider = name;
            UserPreferences.shared.llmServerURL = preset[@"url"] ?: @"";
            if (preset[@"model"].length > 0)
                UserPreferences.shared.llmModel = preset[@"model"];
            [self.tableView reloadData];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSArray<NSString *> *)modelIdentifiersFromResponseData:(NSData *)data {
    return ISHLLMModelIdentifiersFromResponseData(data);
}

- (void)presentModelPickerWithModels:(NSArray<NSString *> *)models statusCode:(NSInteger)statusCode error:(NSError *)error {
    if (error != nil) {
        [self showConnectionResult:error.localizedDescription title:@"Model Query Failed"];
        return;
    }
    if (models.count == 0) {
        NSString *message = statusCode > 0 ? [NSString stringWithFormat:@"No models found. HTTP %ld", (long) statusCode] : @"No models found.";
        [self showConnectionResult:message title:@"Model Query Failed"];
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Choose Model"
                                                                   message:[NSString stringWithFormat:@"%lu models returned by %@", (unsigned long) models.count, ISHLLMModelsEndpoint()]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    NSUInteger limit = MIN(models.count, 80);
    for (NSUInteger i = 0; i < limit; i++) {
        NSString *model = models[i];
        NSString *title = [model isEqualToString:UserPreferences.shared.llmModel] ? [model stringByAppendingString:@"  Current"] : model;
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmModel = model;
            [self.tableView reloadData];
        }]];
    }
    if (models.count > limit) {
        [alert addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Showing first %lu of %lu", (unsigned long) limit, (unsigned long) models.count]
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)queryAvailableModels {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self showConnectionResult:[NSString stringWithFormat:@"Current on-device model: %@\n%@", UserPreferences.shared.llmModel.length > 0 ? UserPreferences.shared.llmModel : @"system-language-model", ISHLLMAppleFoundationModelsUnavailableMessage()] title:@"Apple Foundation Models"];
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
    if (url == nil) {
        [self showConnectionResult:@"Invalid models URL." title:@"Model Query Failed"];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
            NSArray<NSString *> *models = [self modelIdentifiersFromResponseData:data];
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentModelPickerWithModels:models statusCode:statusCode error:error];
            });
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSArray<NSString *> *models = [self modelIdentifiersFromResponseData:data];
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentModelPickerWithModels:models statusCode:http.statusCode error:error];
        });
    }];
    [task resume];
}

- (void)testConnection {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self showConnectionResult:ISHLLMAppleFoundationModelsUnavailableMessage() title:@"Apple Foundation Models"];
        return;
    }
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSURL *url = [NSURL URLWithString:ISHLLMUsesGeminiAPI() ? ISHLLMGeminiGenerateEndpoint() : ISHLLMChatEndpoint()];
    if (model.length == 0 || url == nil) {
        [self showConnectionResult:@"Set a valid server URL and model first." title:@"LLM Test Failed"];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self showConnectionResult:ISHLLMMissingAPIKeyMessage() title:@"LLM Test Failed"];
        return;
    }
    NSDictionary *body = ISHLLMUsesGeminiAPI()
        ? @{@"contents": @[@{@"role": @"user", @"parts": @[@{@"text": @"Reply with exactly: ok"}]}]}
        : @{
            @"model": model,
            @"messages": @[@{@"role": @"user", @"content": @"Reply with exactly: ok"}],
            @"stream": @NO,
            @"max_tokens": @8,
        };
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPPost(url, bodyData, apiKey, &statusCode, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (error != nil) {
                    [self showConnectionResult:error.localizedDescription title:@"LLM Test Failed"];
                } else {
                    NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
                    NSString *message = [NSString stringWithFormat:@"HTTP %ld\n%@", (long) statusCode, raw.length > 240 ? [raw substringToIndex:240] : raw];
                    [self showConnectionResult:message title:(statusCode >= 200 && statusCode < 300 ? @"LLM Test OK" : @"LLM Test Failed")];
                }
            });
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    request.HTTPBody = bodyData;
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error != nil) {
                [self showConnectionResult:error.localizedDescription title:@"LLM Test Failed"];
            } else {
                NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
                NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
                NSString *message = [NSString stringWithFormat:@"HTTP %ld\n%@", (long) http.statusCode, raw.length > 240 ? [raw substringToIndex:240] : raw];
                [self showConnectionResult:message title:(http.statusCode >= 200 && http.statusCode < 300 ? @"LLM Test OK" : @"LLM Test Failed")];
            }
        });
    }];
    [task resume];
}

- (void)showConnectionResult:(NSString *)message title:(NSString *)title {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

@implementation AboutViewController
{
    BOOL _didPresentInitialDiagnostics;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self _updateUI];
    UIBarButtonItem *workspaceButton = [[UIBarButtonItem alloc] initWithTitle:@"Workspace"
                                                                        style:UIBarButtonItemStylePlain
                                                                       target:self
                                                                       action:@selector(showWorkspace:)];
    if (self.recoveryMode) {
        self.includeDebugPanel = YES;
        self.navigationItem.title = @"Recovery Mode";
        self.navigationItem.leftBarButtonItem = workspaceButton;
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Exit"
                                                                                  style:UIBarButtonItemStyleDone
                                                                                 target:self
                                                                                 action:@selector(exitRecovery:)];
    } else {
        self.navigationItem.rightBarButtonItem = workspaceButton;
    }
    _versionLabel.text = [NSString stringWithFormat:@"iSH-AOK %@ (Build %@)",
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"]];

    [UserPreferences.shared observe:@[@"capsLockMapping", @"fontSize", @"launchCommand", @"bootCommand", @"shouldLockSleepNanoseconds"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateUI];
        });
    }];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(_updateUI:) name:FsUpdatedNotification object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self _updateUI];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (self.startInDiagnostics && !_didPresentInitialDiagnostics) {
        _didPresentInitialDiagnostics = YES;
        [self showDiagnostics:self.diagnosticsCell ?: self];
    }
}

- (IBAction)dismiss:(id)sender {
    [self dismissViewControllerAnimated:self completion:nil];
}

- (void)exitRecovery:(id)sender {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"recovery"];
    exit(0);
}

- (void)showDiagnostics:(id)sender {
    UIViewController *viewController = ISHCreateDiagnosticsViewController();
    [self.navigationController pushViewController:viewController animated:YES];
}

- (void)showWorkspace:(id)sender {
    if (@available(iOS 13.0, *)) {
        UISceneSession *currentSession = self.view.window.windowScene.session;
        UISceneSession *workspaceSession = ISHFindExistingWorkspaceSceneSession(currentSession);
        if (workspaceSession != nil) {
            [UIApplication.sharedApplication requestSceneSessionActivation:workspaceSession
                                                             userActivity:nil
                                                                  options:nil
                                                             errorHandler:^(__unused NSError *error) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    UINavigationController *navigationController = ISHCreateWorkspaceNavigationController();
                    [self presentViewController:navigationController animated:YES completion:nil];
                });
            }];
            return;
        }
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace];
        [UIApplication.sharedApplication requestSceneSessionActivation:nil
                                                         userActivity:activity
                                                              options:nil
                                                         errorHandler:^(__unused NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                UINavigationController *navigationController = ISHCreateWorkspaceNavigationController();
                [self presentViewController:navigationController animated:YES completion:nil];
            });
        }];
        return;
    }

    UINavigationController *navigationController = ISHCreateWorkspaceNavigationController();
    [self presentViewController:navigationController animated:YES completion:nil];
}

- (void)_updateUI:(NSNotification *)notification {
    [self _updateUI];
}

- (void)_updateUI {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    self.disableDimmingSwitch.on = UserPreferences.shared.shouldDisableDimming;
    self.enableExperimentalAmd64JitSwitch.on = UserPreferences.shared.shouldEnableExperimentalAmd64Jit;
    self.enableMulticoreSwitch.on = UserPreferences.shared.shouldEnableMulticore;
    self.enableExtraLockingSwitch.on = UserPreferences.shared.shouldEnableExtraLocking;
    self.initialWindowCell.textLabel.text = @"Startup Mode";
    self.initialWindowCell.detailTextLabel.text = [self _initialWindowTitle];
    self.launchCommandField.text = [UserPreferences.shared.launchCommand componentsJoinedByString:@" "];
    self.bootCommandField.text = [UserPreferences.shared.bootCommand componentsJoinedByString:@" "];

    self.upgradeApkCell.userInteractionEnabled = FsNeedsRepositoryUpdate();
    self.upgradeApkLabel.enabled = FsNeedsRepositoryUpdate();
    self.upgradeApkBadge.hidden = !FsNeedsRepositoryUpdate();
    self.upgradeApkCell.accessibilityValue = FsNeedsRepositoryUpdate() ? @"Update available" : nil;
    [self.tableView reloadData];
}

- (NSInteger)_visibleStoryboardSectionCount {
    NSInteger sections = [super numberOfSectionsInTableView:self.tableView];
    if (!self.includeDebugPanel)
        sections--;
    return sections;
}

- (NSInteger)_llmSectionIndex {
    return [self _visibleStoryboardSectionCount];
}

- (UITableViewCell *)_llmEnabledCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = @"LLM Client";
    UISwitch *enabledSwitch = [UISwitch new];
    enabledSwitch.on = UserPreferences.shared.shouldEnableLLMClient;
    [enabledSwitch addTarget:self action:@selector(llmClientEnabledChanged:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = enabledSwitch;
    self.llmClientSwitch = enabledSwitch;
    return cell;
}

- (UITableViewCell *)_llmSettingsCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.textLabel.text = @"LLM Settings";
    cell.detailTextLabel.text = UserPreferences.shared.llmModel;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _llmSectionIndex]) {
        if (indexPath.row == 1) {
            UIViewController *settingsViewController = ISHCreateLLMSettingsViewController();
            if (self.navigationController != nil) {
                [self.navigationController pushViewController:settingsViewController animated:YES];
            } else {
                UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:settingsViewController];
                ISHConfigureLLMSettingsNavigationController(navigationController);
                [self presentViewController:navigationController animated:YES completion:nil];
            }
        }
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        return;
    }
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    if (cell == self.sendFeedback) {
        [UIApplication openURL:@"mailto:ish_aok_emkey1@icloud.com?subject=Feedback%20for%20iSH"];
    } else if (cell == self.diagnosticsCell) {
        [self showDiagnostics:cell];
    } else if (cell == self.initialWindowCell) {
        [self _showInitialWindowPickerFromCell:cell];
    } else if (cell == self.openGithub) {
        [UIApplication openURL:@"https://github.com/emkey1/ish-AOK"];
    } else if (cell == self.openDiscord) {
        [UIApplication openURL:@"https://discord.com/channels/776432683302649866/776432683302649870"];
    } else if (cell == self.exportContainerCell) {
        // copy the files to the app container so they can be extracted from iTunes file sharing
        NSURL *container = ContainerURL();
        NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
        [NSFileManager.defaultManager removeItemAtURL:[documents URLByAppendingPathComponent:@"roots copy"] error:nil];
        [NSFileManager.defaultManager copyItemAtURL:[container URLByAppendingPathComponent:@"roots"]
                                              toURL:[documents URLByAppendingPathComponent:@"roots copy"]
                                              error:nil];
    } else if (cell == self.resetMountsCell) {
        iosfs_clear_all_bookmarks();
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (NSString *)_initialWindowPreferenceValue {
    NSString *value = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([value isEqualToString:ISHInitialWindowWorkspaceValue])
        return value;
    if ([value isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return value;
    if ([value isEqualToString:@"session-shell"])
        return value;
    return @"terminal";
}

- (NSString *)_initialWindowTitle {
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowWorkspaceValue])
        return @"Workspace";
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return @"Choose Filesystem";
    if ([[self _initialWindowPreferenceValue] isEqualToString:@"session-shell"])
        return @"Session Shell (pts/1)";
    return @"Plain Terminal";
}

- (void)_setInitialWindowPreferenceValue:(NSString *)value {
    [NSUserDefaults.standardUserDefaults setObject:value forKey:kPreferenceInitialWindowKey];
    [self _updateUI];
}

- (void)_showInitialWindowPickerFromCell:(UITableViewCell *)cell {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Startup Mode"
                                            message:@"Choose whether new app launches open the Workspace, show a filesystem chooser, or open a terminal."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    NSString *currentValue = [self _initialWindowPreferenceValue];
    NSString *workspaceTitle = [currentValue isEqualToString:ISHInitialWindowWorkspaceValue]
        ? @"Workspace  Current"
        : @"Workspace";
    NSString *chooseFilesystemTitle = [currentValue isEqualToString:ISHInitialWindowChooseFilesystemValue]
        ? @"Choose Filesystem  Current"
        : @"Choose Filesystem";
    NSString *terminalTitle = [currentValue isEqualToString:@"terminal"]
        ? @"Plain Terminal  Current"
        : @"Plain Terminal";
    NSString *sessionTitle = [currentValue isEqualToString:@"session-shell"]
        ? @"Session Shell (pts/1)  Current"
        : @"Session Shell (pts/1)";

    [alert addAction:[UIAlertAction actionWithTitle:workspaceTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowWorkspaceValue];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:chooseFilesystemTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowChooseFilesystemValue];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:terminalTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"terminal"];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:sessionTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"session-shell"];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = cell;
        popover.sourceRect = cell.bounds;
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return UserPreferences.shared.shouldEnableLLMClient
            ? @"When enabled, LLM Chat appears in Switch Terminal and Workspace menus."
            : @"Enable to show an OpenAI-compatible LLM client in terminal and Workspace menus.";
    if (section == 1) { // filesystems / upgrade
        if (!FsIsManaged()) {
            return @"The current filesystem is not managed by iSH.";
        } else if (!FsNeedsRepositoryUpdate()) {
            return [NSString stringWithFormat:@"The current filesystem is using %s, which is the latest version.", NEWEST_APK_VERSION];
        } else {
            return [NSString stringWithFormat:@"An upgrade to %s is available.", NEWEST_APK_VERSION];
        }
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return @"LLM Client";
    return [super tableView:tableView titleForHeaderInSection:section];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return [self _visibleStoryboardSectionCount] + 1;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return UserPreferences.shared.shouldEnableLLMClient ? 2 : 1;
    return [super tableView:tableView numberOfRowsInSection:section];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _llmSectionIndex]) {
        if (indexPath.row == 0)
            return [self _llmEnabledCell];
        return [self _llmSettingsCell];
    }
    return [super tableView:tableView cellForRowAtIndexPath:indexPath];
}

- (IBAction)disableDimmingChanged:(id)sender {
    UserPreferences.shared.shouldDisableDimming = self.disableDimmingSwitch.on;
}

- (IBAction)enableExperimentalAmd64JitChanged:(id)sender {
    UserPreferences.shared.shouldEnableExperimentalAmd64Jit = self.enableExperimentalAmd64JitSwitch.on;
}

- (IBAction)enableMulticoreChanged:(id)sender {
    UserPreferences.shared.shouldEnableMulticore = self.enableMulticoreSwitch.on;
}

- (IBAction)enableExtraLockingChanged:(id)sender {
    UserPreferences.shared.shouldEnableExtraLocking = self.enableExtraLockingSwitch.on;
}

- (void)llmClientEnabledChanged:(UISwitch *)sender {
    UserPreferences.shared.shouldEnableLLMClient = sender.on;
    [self.tableView reloadData];
}

//- (IBAction)shouldLockSleepNanoseconds:(id)sender {
//    UserPreferences.shared.shouldLockSleepNanoseconds = self.shouldLockSleepNanosecondsSwitch.on;
//}

- (IBAction)textBoxSubmit:(id)sender {
    [sender resignFirstResponder];
}

- (IBAction)launchCommandChanged:(id)sender {
    UserPreferences.shared.launchCommand = [self.launchCommandField.text componentsSeparatedByString:@" "];
}

- (IBAction)bootCommandChanged:(id)sender {
    UserPreferences.shared.bootCommand = [self.bootCommandField.text componentsSeparatedByString:@" "];
}

@end
