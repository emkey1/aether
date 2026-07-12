// iSH-AOK Display applet viewer glue.
//
// Derived from noVNC's vnc_lite.html example (Copyright (C) 2019 The noVNC
// authors, 2-Clause BSD license -- see deps/novnc/LICENSE.txt). Connects
// immediately to the native WebSocket-to-TCP bridge started by
// DisplayViewController, at ws://127.0.0.1:<port> (the port is passed in the
// URL query string). No on-screen chrome of its own -- the applet's native
// UI (toolbar, status) is the chrome; this is just the remote screen.
//
// Native <-> JS bridge, message handler name "ishDisplay":
//   JS -> native: postMessage({type: "status", state: "connecting"|"connected"
//                              |"disconnected"|"error"|"desktopname", detail})
//   native -> JS: window.ishDisplaySendCtrlAltDel()
//                 window.ishDisplaySetViewOnly(bool)
//                 window.ishDisplaySetScaling(bool)
// Path is bundle-relative, not source-tree-relative: deps/novnc is added to
// the Xcode project as a folder reference (see iSH-AOK.xcodeproj), which
// copies it into the app bundle as a "novnc/" sibling of this file, not at
// its source-tree path.
let rfb = null;

function post(type, fields) {
    const msg = Object.assign({ type }, fields || {});
    if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.ishDisplay) {
        window.webkit.messageHandlers.ishDisplay.postMessage(msg);
    }
}

// This file is loaded as a plain classic script, not <script type="module">
// -- WKWebView loading a module script tag via file:// silently never
// executes it at all (confirmed on-device: none of this file's code ran,
// not even these error listeners, since they'd have been inside the
// never-started module body). noVNC's rfb.js is still an ES module
// internally; dynamic import() works from a classic script and lets a
// resolution failure be caught and reported like any other error instead
// of vanishing the same way.
window.addEventListener('error', (e) => {
    post('status', { state: 'error', detail: 'script error: ' + (e.message || e) });
});
window.addEventListener('unhandledrejection', (e) => {
    post('status', { state: 'error', detail: 'unhandled rejection: ' + (e.reason && e.reason.message || e.reason) });
});

function readQueryVariable(name, defaultValue) {
    const re = new RegExp('[?&]' + name + '=([^&#]*)');
    const match = document.location.search.match(re);
    return match ? decodeURIComponent(match[1]) : defaultValue;
}

const port = readQueryVariable('port', '');
const scale = readQueryVariable('scale', 'true') === 'true';

if (!port) {
    post('status', { state: 'error', detail: 'no bridge port supplied' });
} else {
    post('status', { state: 'connecting' });

    import('./novnc/core/rfb.js').then(({ default: RFB }) => {
        const url = 'ws://127.0.0.1:' + port + '/';

        rfb = new RFB(document.getElementById('screen'), url, {});
        rfb.scaleViewport = scale;
        rfb.resizeSession = false;
        rfb.clipViewport = false;

        rfb.addEventListener('connect', () => post('status', { state: 'connected' }));
        rfb.addEventListener('disconnect', (e) => {
            post('status', { state: 'disconnected', detail: e.detail && e.detail.clean ? 'clean' : 'unclean' });
        });
        rfb.addEventListener('securityfailure', (e) => {
            post('status', { state: 'error', detail: (e.detail && e.detail.reason) || 'security failure' });
        });
        rfb.addEventListener('desktopname', (e) => post('status', { state: 'desktopname', detail: e.detail.name }));
    }).catch((err) => {
        post('status', { state: 'error', detail: 'failed to load noVNC module: ' + (err && err.message ? err.message : err) });
    });
}

// Native -> JS entry points, called from DisplayViewController via
// evaluateJavaScript:.
window.ishDisplaySendCtrlAltDel = function () {
    if (rfb) rfb.sendCtrlAltDel();
};
window.ishDisplaySetViewOnly = function (viewOnly) {
    if (rfb) rfb.viewOnly = !!viewOnly;
};
window.ishDisplaySetScaling = function (enabled) {
    if (rfb) rfb.scaleViewport = !!enabled;
};
