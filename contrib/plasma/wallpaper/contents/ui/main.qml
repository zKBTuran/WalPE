/* main.qml - the Plasma 6 wallpaper type "WalPE".
 *
 * It draws no wallpaper. walpe-daemon draws the wallpaper in a surface of its own; this type
 * makes Plasma's desktop window (the icons and widgets) see-through, and its KWin script
 * (../code/below.js, loaded by Script.qml) puts that window right over the daemon's surface.
 *
 * A see-through desktop with nothing under it would be black. So the desktop stays filled
 * with a stand-in (a colour, or a picture: the settings of this type) until the KWin script
 * says that a surface of the daemon lies under this very desktop (it writes the key Under),
 * and it is filled again when the script says the surface is gone, when the script is not
 * loaded, and while KWin shows the desktop (it hides the daemon's surface then). */
import QtQuick
import org.kde.plasma.plasmoid

WallpaperItem {
    id: root

    // only Plasma's desktop: the lock screen and the preview of the wallpaper settings load
    // this file too, and show the stand-in
    readonly property bool onDesktop: Qt.application.name === "plasmashell"
    // the desktop window this wallpaper is in (null while its activity is not the current one)
    readonly property var view: Window.window
    property var cleared: null       // the window that was made see-through
    property bool leaving: false     // this wallpaper is being taken down
    readonly property bool scriptLoaded: script.item !== null && script.item.loaded && !script.item.peek
    readonly property bool under: onDesktop && cleared !== null && cleared === view
                                  && root.configuration.Under && scriptLoaded

    // Qt puts the alpha of a window's colour into its surface format (QQuickWindow::setColor),
    // and a window that is on screen keeps the format it was made with: close() gives up the
    // platform window, show() makes a new one. Hiding and showing is not enough. For KWin that
    // is a new window: the rule its script made for the old one is gone with it.
    function remake(v) {
        if (v && v.visible) {
            v.close();
            v.show();
        }
    }

    // The window goes back to what it was: opaque, and made anew, so that KWin forgets that it
    // was to stay above the daemon's surface. Whoever shows in it next starts from there.
    function restore() {
        const v = cleared;
        cleared = null;
        if (v && v.color.a === 0) {
            v.color = "black";
            remake(v);
        }
    }

    function seeThrough() {
        if (cleared && cleared !== view)
            restore();               // this wallpaper left the window (another activity, another type)
        if (!onDesktop || !view || leaving)
            return;
        if (view.color.a !== 0) {
            const opaque = view.color.a === 1;
            view.color = "transparent";
            if (opaque)
                later.start();
        }
        cleared = view;
    }

    Timer {
        id: later
        interval: 0
        onTriggered: root.remake(root.cleared)
    }

    // a wallpaper of this type that leaves the window (another activity) hands it back opaque
    Connections {
        target: root.view
        function onColorChanged() { root.seeThrough(); }
    }

    onViewChanged: seeThrough()
    Component.onCompleted: {
        // what an earlier session left in the settings says nothing about this one
        if (onDesktop && root.configuration.Under)
            root.configuration.Under = false;
        seeThrough();
    }
    Component.onDestruction: {
        leaving = true;
        restore();
    }

    Loader {
        id: script
        active: root.onDesktop
        source: "Script.qml"    // fails where Plasma has no D-Bus module for QML: the stand-in stays
    }

    Rectangle {
        anchors.fill: parent
        visible: !root.under
        color: root.configuration.Color
        Image {
            anchors.fill: parent
            // no picture in memory while the daemon's wallpaper is what is seen
            source: parent.visible ? root.configuration.Image : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: false
            sourceSize.width: root.width * Screen.devicePixelRatio
            sourceSize.height: root.height * Screen.devicePixelRatio
        }
    }
}
