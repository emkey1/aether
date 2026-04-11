//
//  UpgradeRootViewController.m
//  iSH
//
//  Created by Theodore Dubois on 11/27/21.
//

#import "UpgradeRootViewController.h"
#import "AppDelegate.h"
#import "TerminalView.h"
#import "CurrentRoot.h"
#include "kernel/calls.h"
#include "kernel/init.h"
#include "fs/devices.h"
#include "util/sync.h"

@interface UpgradeRootViewController ()

@property (weak, nonatomic) IBOutlet TerminalView *terminalView;
@property (weak, nonatomic) IBOutlet UIBarButtonItem *upgradeButton;
@property (nonatomic) Terminal *terminal;
@property (nonatomic) struct tty *tty;
@property (nonatomic) int upgradePid;

@end

static bool UpgradePushInitTaskAsCurrent(struct task **previousCurrent) {
    *previousCurrent = current;

    complex_lockt(&pids_lock, 0);
    struct task *init = pid_get_task(1);
    if (init != NULL && !init->exiting)
        task_ref_cnt_mod(init, 1);
    else
        init = NULL;
    unlock(&pids_lock);

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

static void UpgradePopCurrentTask(struct task *previousCurrent) {
    struct task *borrowedCurrent = current;
    current = previousCurrent;
    if (borrowedCurrent != NULL)
        task_ref_cnt_mod(borrowedCurrent, -1);
}

@implementation UpgradeRootViewController

- (void)viewDidLoad {
    [super viewDidLoad];
#if !ISH_LINUX
    if ([AppDelegate ensureBooted] < 0)
        return;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(processExited:) name:ProcessExitedNotification object:nil];

    struct task *previousCurrent = NULL;
    if (UpgradePushInitTaskAsCurrent(&previousCurrent)) {
        self.terminal = [Terminal createPseudoTerminal:&self->_tty];
        UpgradePopCurrentTask(previousCurrent);
    }
    
    self.terminalView.terminal = self.terminal;
#endif
    self.upgradeButton.enabled = NO;
    if (FsNeedsRepositoryUpdate()) {
        self.upgradeButton.enabled = YES;
        [self printToTerminal:@"# /sbin/apk upgrade"];
    } else {
        [self showAlertWithTitle:@"fuck" message:@"No update needed. If you're seeing this message, there's a bug."];
    }
}

- (void)printToTerminal:(NSString *)message, ... {
    va_list args;
    va_start(args, message);
    message = [[NSString alloc] initWithFormat:message arguments:args];
    [self.terminal sendOutput:message.UTF8String length:(int)[message lengthOfBytesUsingEncoding:NSUTF8StringEncoding]];
}

- (void)showAlertWithTitle:(NSString *)title message:(NSString *)message, ... {
    va_list args;
    va_start(args, message);
    message = [[NSString alloc] initWithFormat:message arguments:args];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#if !ISH_LINUX
- (void)processExited:(NSNotification *)notif {
    int pid = [notif.userInfo[@"pid"] intValue];
    if (pid != self.upgradePid)
        return;
    self.upgradePid = 0;
    [self setDismissable:YES];
    int code = [notif.userInfo[@"code"] intValue];
    if (code != 0) {
        [self showAlertWithTitle:@"Upgrade failed" message:@"exit status %d", code];
    } else {
        struct task *previousCurrent = NULL;
        if (UpgradePushInitTaskAsCurrent(&previousCurrent)) {
            FsUpdateRepositories();
            UpgradePopCurrentTask(previousCurrent);
        }
        [self showAlertWithTitle:@"Upgrade succeeded" message:@""];
    }
    [self.terminal destroy];
    self.terminal = nil;
}
#endif

- (intptr_t)startUpgrade {
    if (self.upgradePid != 0)
        return _EEXIST;
#if !ISH_LINUX
    intptr_t err = [AppDelegate ensureBooted];
    if (err < 0)
        return err;
    err = become_new_init_child();
    if (err < 0)
        return err;
    FsUpdateOnlyRepositoriesFile();
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", self.tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, self.tty->num);
    if (err < 0)
        return err;
    err = do_execve("/sbin/apk", 2, "/sbin/apk\0upgrade\0", "TERM=xterm-256color\0");
    if (err < 0)
        return err;
    self.upgradePid = current->pid;
    task_start(current);
    current = NULL;
    return 0;
#else
    return _ENOSYS;
#endif
}

- (IBAction)upgrade:(id)sender {
    self.upgradeButton.enabled = NO;
    [self setDismissable:NO];
    [self printToTerminal:@"\r\n"];
    intptr_t err = [self startUpgrade];
    if (err < 0) {
        [self showAlertWithTitle:@"Failed to start upgrade" message:@"error %d", err];
    }
}

- (void)setDismissable:(BOOL)dismissable {
    [self.navigationItem setHidesBackButton:!dismissable animated:YES];
    self.navigationController.interactivePopGestureRecognizer.enabled = dismissable;
    if (@available(iOS 13, *)) {
        self.modalInPresentation = !dismissable;
    }
}

- (void)dealloc {
    [self.terminal destroy];
    if (self.tty != NULL)
        tty_release(self.tty);
}

@end
