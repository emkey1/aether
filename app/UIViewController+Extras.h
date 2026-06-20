//
//  UIViewController+Extras.h
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface UIViewController (Extras)

- (void)presentError:(NSError *)error title:(NSString *)title;

// Anchors an action-sheet UIAlertController's popover so it can be presented on
// iPad without throwing "you must provide location information for this popover".
// `source` may be a UIBarButtonItem, a UIView (e.g. the tapped button or cell),
// or nil (falls back to the center of this controller's view). A no-op when the
// alert is not presented as a popover (e.g. iPhone, or a plain alert).
- (void)anchorPopoverForAlertController:(UIAlertController *)alertController toSource:(nullable id)source;

@end

NS_ASSUME_NONNULL_END
