//
//  AboutViewController.h
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const kPreferenceOpenDiagnosticsOnLaunchKey;

@interface AboutViewController : UITableViewController

@property BOOL includeDebugPanel;
@property BOOL recoveryMode;
@property BOOL startInDiagnostics;

@end

NS_ASSUME_NONNULL_END
