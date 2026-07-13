#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

// Workspace "Display" applet: a headless Wayland desktop (labwc + foot +
// wayvnc, launched via /AOK/tools/start-wayland.sh) shown through a native
// Metal-rendered RFB client (DisplayRFBClient/DisplayRFBView) connected
// straight to wayvnc's loopback TCP port. See wayland_workspace_plan.md and
// the harmonic-giggling-aho plan for why this replaced the original vendored
// noVNC-in-WKWebView + WebSocket bridge design.
//
// The guest session is owned the same way TerminalViewController owns its
// shell session -- a real pseudo-terminal via +[Terminal createPseudoTerminal:]
// -- but its Terminal (a plain hterm-less instance) is never shown; only the
// DisplayRFBView is user-visible. Closing the applet hangs up that pty
// (SIGHUP), which start-wayland.sh's trap tears the whole guest stack down on.
@interface DisplayViewController : WorkspaceThemedToolViewController
@end

NS_ASSUME_NONNULL_END
