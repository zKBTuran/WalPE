/* Pointer.qml - hands the buttons and the wheel on to walpe-daemon while a wallpaper there
 * uses the pointer.
 *
 * Plasma's desktop window lies over the wallpaper and gets every pointer event. This item is
 * put on top of everything in that window and only looks: its handlers take a passive grab
 * (PointHandler) or do not block (WheelHandler), so the icons, the widgets and the desktop's
 * own menu get every event as before. What it sees goes to the daemon over D-Bus
 * (io.github.walpe.Pointer.Event on the daemon's connection; the daemon has the name
 * io.github.walpe.Pointer only while a wallpaper wants the pointer, and main.qml loads this
 * file only then, after Script.qml has made sure whose connection that is):
 *   the left button and the wheel, each with its place, where the pointer is over the bare
 *   desktop: over an icon or a widget they are Plasma's alone (found() below says what
 *   counts as one).
 * The right and the middle button stay Plasma's (the desktop's menu, paste). A press on the
 * bare desktop also starts Plasma's selection frame.
 *
 * Where the pointer is, is not this file's business any more. Nothing here asks for the
 * hover: an item that does, above Plasma's own, takes it from them - with a HoverHandler
 * here (and with a MouseArea in its place) Plasma's icons had no frame under the pointer
 * and its widgets no tool tips, whatever the handler's `blocking` said (Plasma 6.8.2,
 * Qt 6.12). The daemon gets the position from a script of its own in KWin instead
 * (src/signals/kwinscript.h in WalPE), also while a button is down. */
import QtQuick
import org.kde.plasma.workspace.dbus as DBus

Item {
    id: tap
    property var view: null              // the desktop window
    property string to                   // the daemon's connection on the bus (Script.qml has checked whose it is)
    parent: view ? view.contentItem : null
    anchors.fill: parent
    z: 1000000

    property bool down: false            // a press was handed on and its release not yet
    property point last                  // where: the release is sent with the place of the press or drag

    function send(what, x, y, a, b) {
        DBus.SessionBus.asyncCall({
            "service": to, "path": "/io/github/walpe/Pointer",
            "iface": "io.github.walpe.Pointer", "member": "Event",
            "arguments": [new DBus.string(Screen.name), new DBus.uint32(what), new DBus.double(x),
                          new DBus.double(y), new DBus.double(a), new DBus.double(b)],
            "signature": "(sudddd)"
        });
    }

    function release() {
        if (down) {
            down = false;
            send(2, last.x, last.y, 1, 0);
        }
    }

    // What of Plasma's is at the point: an icon, a widget, a button, or null on the bare desktop.
    // The layers of the desktop itself (the containment, the folder view) are as wide or as
    // high as the window; what is smaller both ways and can be seen is taken for a thing on it.
    // (Not QQuickItem.contains: the item that holds a view's icons answers for the whole view.)
    function found(p) {
        const w = tap.width * 0.9, h = tap.height * 0.9;
        function walk(item, depth) {
            if (!item || item === tap || !item.visible || item.opacity === 0 || depth > 64)
                return null;
            const q = item.mapFromItem(tap, p.x, p.y);
            const inside = q.x >= 0 && q.y >= 0 && q.x < item.width && q.y < item.height;
            if (inside && item.width < w && item.height < h &&
                !(item.parent && item.parent.contentItem === item))
                return item;
            if (item.clip && !inside)
                return null;
            const kids = item.children;
            for (let i = 0; i < kids.length; i++) {
                const hit = walk(kids[i], depth + 1);
                if (hit)
                    return hit;
            }
            return null;
        }
        return walk(tap.parent, 0);
    }

    // the same for the wheel, which asks many times at one place
    property point askedAt: Qt.point(-1, -1)
    property bool askedIs: false
    function plasmas(p) {
        if (p.x !== askedAt.x || p.y !== askedAt.y) {
            askedAt = Qt.point(p.x, p.y);
            askedIs = found(p) !== null;
        }
        return askedIs;
    }

    PointHandler {
        id: press
        acceptedButtons: Qt.LeftButton
        onActiveChanged: {
            if (!active) {
                tap.release();
            } else if (tap.found(point.position) === null) {
                tap.release();      // a release that went elsewhere (a menu took it)
                tap.last = point.position;
                tap.down = true;
                tap.send(1, point.position.x, point.position.y, 1, 0);
            }
        }
        onPointChanged: if (active) tap.last = point.position
        onCanceled: tap.release()
    }

    WheelHandler {
        blocking: false
        acceptedDevices: PointerDevice.AllDevices
        onWheel: (wheel) => {
            if (!tap.plasmas(Qt.point(wheel.x, wheel.y)))
                tap.send(3, wheel.x, wheel.y, wheel.angleDelta.x, wheel.angleDelta.y);
        }
    }

    Component.onDestruction: release()
}
