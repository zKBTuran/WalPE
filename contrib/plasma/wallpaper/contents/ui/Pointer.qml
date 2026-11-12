/* Pointer.qml - hands the pointer on to walpe-daemon while a wallpaper there uses it.
 *
 * Plasma's desktop window lies over the wallpaper and gets every pointer event. This item is
 * put on top of everything in that window and only looks: its handlers take passive grabs
 * (PointHandler, HoverHandler) or do not block (WheelHandler), so the icons, the widgets and
 * the desktop's own menu get every event as before. What it sees goes to the daemon over
 * D-Bus (io.github.walpe.Pointer; the daemon has that name only while a wallpaper wants the
 * pointer, and main.qml loads this file only then):
 *   the position, at most once per frame of the screen;
 *   the left button and the wheel, where the pointer is over the bare desktop: over an
 *   icon or a widget they are Plasma's alone (found() below says what counts as one);
 *   that the pointer has left the desktop.
 * The right and the middle button stay Plasma's (the desktop's menu, paste). A press on the
 * bare desktop also starts Plasma's selection frame; the wallpaper gets the drag as well. */
import QtQuick
import org.kde.plasma.workspace.dbus as DBus

Item {
    id: tap
    property var view: null              // the desktop window
    parent: view ? view.contentItem : null
    anchors.fill: parent
    z: 1000000

    property bool down: false            // a press was handed on and its release not yet
    property bool pending: false         // the position changed since the last one sent
    property point last

    function send(what, x, y, a, b) {
        DBus.SessionBus.asyncCall({
            "service": "io.github.walpe.Pointer", "path": "/io/github/walpe/Pointer",
            "iface": "io.github.walpe.Pointer", "member": "Event",
            "arguments": [new DBus.string(Screen.name), new DBus.uint32(what), new DBus.double(x),
                          new DBus.double(y), new DBus.double(a), new DBus.double(b)],
            "signature": "(sudddd)"
        });
    }

    function motion(p) {
        if (p.x === last.x && p.y === last.y)
            return;         // the handlers also fire when only the time of the point changed
        last = p;
        if (pace.running) {
            pending = true;
        } else {
            send(0, p.x, p.y, 0, 0);
            pace.start();
        }
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

    Timer {
        id: pace
        interval: Math.max(4, Math.round(1000 / Math.max(30, Screen.refreshRate || 60)))
        onTriggered: {
            if (tap.pending) {
                tap.pending = false;
                tap.send(0, tap.last.x, tap.last.y, 0, 0);
                start();
            }
        }
    }

    PointHandler {
        id: press
        acceptedButtons: Qt.LeftButton
        onActiveChanged: {
            if (!active) {
                tap.release();
            } else if (tap.found(point.position) === null) {
                tap.last = point.position;
                tap.down = true;
                tap.send(1, point.position.x, point.position.y, 1, 0);
            }
        }
        onPointChanged: if (active) tap.motion(point.position)
        onCanceled: tap.release()
    }

    HoverHandler {
        blocking: false
        onPointChanged: {
            // no button is down while the pointer hovers: a release went elsewhere (a menu took it)
            if (!press.active)
                tap.release();
            tap.motion(point.position);
        }
        onHoveredChanged: {
            if (!hovered) {
                tap.release();
                tap.send(4, 0, 0, 0, 0);
            }
        }
    }

    WheelHandler {
        blocking: false
        acceptedDevices: PointerDevice.AllDevices
        onWheel: (wheel) => {
            if (!tap.plasmas(Qt.point(wheel.x, wheel.y)))
                tap.send(3, wheel.x, wheel.y, wheel.angleDelta.x, wheel.angleDelta.y);
        }
    }

    Component.onDestruction: {
        release();
        send(4, 0, 0, 0, 0);
    }
}
