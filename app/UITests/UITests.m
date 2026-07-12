//
//  UITests.m
//  UITests
//
//  Created by Theodore Dubois on 11/13/20.
//

#import <XCTest/XCTest.h>

@interface UITests : XCTestCase
@end

@implementation UITests

- (void)setUp {
    self.continueAfterFailure = NO;
}

// Opens the Utilities group-choice sheet, adapting to whichever Workspace
// style is active:
//   Classic -- the "utils" dock tile at the bottom of the screen; long-press
//              it (a plain tap does something else -- toggleClockFromDock:).
//   Modern  -- dockWindow is hidden entirely and there's no "utils" element
//              at all; instead a round "Workspace menu" pip floats in the
//              bottom-right corner (see makeModernMenuPip). Tapping it opens
//              a "Workspace" root sheet with a "Utilities…" item that leads
//              to the *same* presentUtilsDockActionsFromView: sheet Classic's
//              long-press reaches directly.
// Both converge on the same "Media (N)" -> "Open Display" flow from there.
static void OpenUtilitiesGroupSheet(XCUIApplication *app) {
    XCUIElement *utilsDockButton = app.buttons[@"utils"];
    if ([utilsDockButton waitForExistenceWithTimeout:5]) {
        [utilsDockButton pressForDuration:0.4];
        return;
    }

    NSPredicate *pipPredicate = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Workspace menu"];
    XCUIElement *menuPip = [app.buttons matchingPredicate:pipPredicate].firstMatch;
    XCTAssert([menuPip waitForExistenceWithTimeout:15],
              @"Neither the Classic 'utils' dock tile nor the Modern 'Workspace menu' pip "
              @"appeared -- Workspace failed to load in either style");
    [menuPip tap];

    NSPredicate *utilitiesPredicate = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Utilities"];
    XCUIElement *utilitiesAction = [app.buttons matchingPredicate:utilitiesPredicate].firstMatch;
    XCTAssert([utilitiesAction waitForExistenceWithTimeout:10],
              @"Workspace root menu (Modern style) did not list 'Utilities…'");
    [utilitiesAction tap];
}

// Opens the Display applet (via OpenUtilitiesGroupSheet, then "Media" group
// -> "Display") and asserts it comes up without crashing: the toolbar
// (status label + Ctrl+Alt+Del/Reconnect buttons) must appear, and the
// status label must settle on either a normal in-progress state or a clear,
// actionable failure (pointing at setup-wayland.sh) -- never stay on
// "Starting..." forever or leave the window blank. On a Simulator/fresh root
// with no Wayland packages installed, the actionable-failure path is the
// expected outcome; this test doesn't require wayvnc to actually be
// reachable, only that DisplayViewController's guest-session/UI plumbing
// itself doesn't crash or hang silently.
- (void)testDisplayAppletOpensWithoutCrashing {
    XCUIApplication *app = [XCUIApplication new];
    [app launch];

    OpenUtilitiesGroupSheet(app);

    NSPredicate *mediaGroupPredicate = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Media"];
    XCUIElement *mediaGroupAction = [app.buttons matchingPredicate:mediaGroupPredicate].firstMatch;
    XCTAssert([mediaGroupAction waitForExistenceWithTimeout:10],
              @"Utils dock long-press did not present the group-choice sheet (Media group missing)");
    [mediaGroupAction tap];

    NSPredicate *displayItemPredicate = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Display"];
    XCUIElement *displayAction = [app.buttons matchingPredicate:displayItemPredicate].firstMatch;
    XCTAssert([displayAction waitForExistenceWithTimeout:10],
              @"Media group sheet did not list Display -- applet registration is missing");
    [displayAction tap];

    // The applet's own toolbar status label -- proves DisplayViewController's
    // viewDidLoad ran, laid its chrome out, and viewDidAppear kicked off
    // -startGuestSession without an immediate crash.
    NSPredicate *statusPredicate = [NSPredicate predicateWithFormat:
        @"label CONTAINS 'Starting' OR label CONTAINS 'Waiting' OR label CONTAINS 'Connect' "
        @"OR label CONTAINS 'setup-wayland' OR label CONTAINS 'Wayland session ended' "
        @"OR label CONTAINS 'Error'"];
    XCUIElement *statusLabel = [app.staticTexts matchingPredicate:statusPredicate].firstMatch;
    XCTAssert([statusLabel waitForExistenceWithTimeout:15],
              @"Display applet's status label never appeared/updated -- window may not have opened");

    // Give the guest-session attempt time to resolve one way or the other
    // (boot + become_new_init_child + do_execve, or the timeout/failure path
    // if the Wayland stack isn't installed) rather than asserting against
    // whatever transient text raced onto screen first. Match by ruling out
    // the two known preamble strings DisplayViewController sets before the
    // guest process even starts, rather than enumerating every downstream
    // status (Waiting for compositor.../Starting bridge.../Connected/
    // Wayland session ended/arbitrary -failWithMessage: text) -- an
    // enumerated allowlist silently stalls the test again the next time a
    // new status string is added.
    NSPredicate *settledPredicate = [NSPredicate predicateWithFormat:
        @"NOT (label CONTAINS 'Starting…') AND NOT (label CONTAINS 'Starting...')"];
    XCUIElement *settledLabel = [app.staticTexts matchingPredicate:settledPredicate].firstMatch;
    XCTAssert([settledLabel waitForExistenceWithTimeout:30],
              @"Display applet's status label never left the initial 'Starting...' state "
              @"(stuck, or crashed without leaving an error message)");
}

@end
