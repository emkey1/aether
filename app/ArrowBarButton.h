//
//  ArrowBarButton.h
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef enum : NSUInteger {
    ArrowNone = 0,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
} ArrowDirection;

@interface ArrowBarButton : UIControl

@property (nonatomic, readonly) ArrowDirection direction;
@property (nonatomic) UIKeyboardAppearance keyAppearance;

// Fires if the touch is held in place (no directional drag past the deadzone) for a beat.
// A plain touch-and-release with no drag is otherwise a no-op for this control, so this
// piggybacks on that dead gesture rather than competing with the drag-to-repeat behavior.
@property (nonatomic, copy, nullable) void (^longPressHandler)(void);

@end

NS_ASSUME_NONNULL_END
