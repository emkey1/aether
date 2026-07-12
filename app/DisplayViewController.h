#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

// Workspace "Display" applet: a headless Wayland desktop (labwc + foot +
// wayvnc, launched via /AOK/tools/start-wayland.sh) shown through a vendored
// noVNC client in a WKWebView, connected over a native WebSocket-to-TCP
// bridge (DisplayNetworkBridge). See wayland_workspace_plan.md phase 2.
//
// The guest session is owned the same way TerminalViewController owns its
// shell session -- a real pseudo-terminal via +[Terminal createPseudoTerminal:]
// -- but its WKWebView (a plain hterm-less Terminal instance) is never shown;
// only the noVNC WKWebView is user-visible. Closing the applet hangs up that
// pty (SIGHUP), which start-wayland.sh's trap tears the whole guest stack
// down on.
@interface DisplayViewController : WorkspaceThemedToolViewController
@end

NS_ASSUME_NONNULL_END
