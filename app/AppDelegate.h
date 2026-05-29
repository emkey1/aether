//
//  AppDelegate.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>

FOUNDATION_EXPORT void ISHScheduleLaunchJournalCompletion(NSDictionary<NSString *, id> * _Nullable details);

struct task;

@interface AppDelegate : UIResponder <UIApplicationDelegate>

@property (strong, nonatomic) UIWindow *window;
- (void)exitApp;

#if !ISH_LINUX
+ (intptr_t)bootError;
+ (NSString * _Nonnull)descriptionForISHErrno:(intptr_t)err;
+ (NSString * _Nullable)bootFailureTitle;
+ (NSString * _Nullable)bootFailureMessage;
+ (NSString * _Nullable)bootFailureOverlayText;
+ (BOOL)bootUsesConsoleSessionFallback;
+ (intptr_t)ensureBooted;
+ (BOOL)pushUsableInitTaskAsCurrent:(struct task * _Nullable * _Nonnull)previousCurrent;
+ (void)popCurrentTask:(struct task * _Nullable)previousCurrent;
#endif

+ (void)maybePresentStartupMessageOnViewController:(UIViewController *)vc;

@end

#if !ISH_LINUX
extern NSString *const ProcessExitedNotification;
#else
extern NSString *const KernelPanicNotification;
#endif
