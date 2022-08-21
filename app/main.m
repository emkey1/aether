//
//  main.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "AppDelegate.h"
extern void run_at_boot(void);

int main(int argc, char * argv[]) {
    @autoreleasepool {
        run_at_boot();
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
