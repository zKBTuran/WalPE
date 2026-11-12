// SPDX-License-Identifier: BSD-3-Clause
// WalPE wallpaper for Plasma 6: draws nothing. It makes plasmashell's desktop window see-through,
// so the surface walpe-daemon draws by itself (kept under this window by the KWin script
// io.github.walpe.below) is the picture behind the icons and widgets.
import QtQuick
import org.kde.plasma.plasmoid

WallpaperItem {
    id: root

    // only the desktop: the lock screen and the wallpaper settings preview load this file too
    readonly property bool onDesktop: Qt.application.name === "plasmashell"
    // the desktop window this wallpaper is shown in (null while its activity is not the current one)
    readonly property var view: Window.window
    property var cleared: null       // the window whose colour was changed
    property color before: "black"   // its colour before that

    function restore() {
        if (cleared)
            cleared.color = before;
        cleared = null;
    }

    function seeThrough() {
        if (cleared === view)
            return;
        restore();
        if (!onDesktop || !view)
            return;
        before = view.color;
        view.color = "transparent";
        cleared = view;
        // Qt puts the colour's alpha into the window's surface format (QQuickWindow::setColor), and
        // a window that is on screen keeps the format it was made with: an opaque one is made anew
        // (close() gives up the platform window, show() makes a new one; hide and show is not enough)
        if (view.visible && before.a === 1)
            remake.start();
    }

    Timer {
        id: remake
        interval: 0
        onTriggered: {
            const v = root.cleared;
            if (v && v.visible) {
                v.close();
                v.show();
            }
        }
    }

    onViewChanged: seeThrough()
    Component.onCompleted: seeThrough()
    Component.onDestruction: restore()

    // anywhere else: plain black, never see-through
    Rectangle {
        anchors.fill: parent
        visible: !root.onDesktop
        color: "black"
    }
}
