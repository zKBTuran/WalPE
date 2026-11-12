/* Script.qml - keeps the KWin script ../code/below.js loaded in KWin for as long as a
 * wallpaper of the type "WalPE" is alive in this Plasma shell.
 *
 * The script is no KWin package and is in no list of KWin's settings: the wallpaper type
 * loads it over D-Bus (org.kde.kwin.Scripting) and takes it out again when its last item
 * goes. So it cannot be switched off behind the type's back, and what the script told the
 * type (the key Under) is believed exactly while `loaded` is true. The first item of a
 * shell replaces a script that an earlier shell left in KWin; KWin coming back on the bus
 * (it was restarted) loads it again.
 *
 * It also listens to KWin's "showing the desktop" ("Peek at Desktop", the "Show Desktop"
 * shortcut): KWin then hides every window that is not part of the desktop, the daemon's
 * surface among them, and the desktop has to be filled for as long as that lasts.
 * Nothing here runs on a timer. */
import QtQuick
import org.kde.plasma.workspace.dbus as DBus
import "../code/shared.js" as Shared

Item {
    id: script
    property bool loaded: false
    property bool peek: false       // KWin is showing the desktop
    readonly property string name: "io.github.walpe.below"
    readonly property string file: Qt.resolvedUrl("../code/below.js").toString().replace(/^file:\/\//, "")

    function call(path, iface, member, args, signature, then) {
        DBus.SessionBus.asyncCall({
            "service": "org.kde.KWin", "path": path, "iface": iface, "member": member,
            "arguments": args, "signature": signature
        }, then, () => { script.loaded = false; });
    }

    function load() {
        call("/Scripting", "org.kde.kwin.Scripting", "loadScript",
             [new DBus.string(file), new DBus.string(name)], "(ss)", (reply) => {
            const id = Number(reply.value);
            if (id < 0) {
                script.loaded = true;      // another item of this shell has loaded it
                return;
            }
            call("/Scripting/Script" + id, "org.kde.kwin.Script", "run", [], "()", () => { script.loaded = true; });
        });
    }

    function start() {
        loaded = false;
        if (Shared.replaced) {
            load();
            return;
        }
        Shared.replaced = true;
        call("/Scripting", "org.kde.kwin.Scripting", "unloadScript", [new DBus.string(name)], "(s)", load);
    }

    DBus.SignalWatcher {
        busType: DBus.BusType.Session
        service: "org.kde.KWin"
        path: "/KWin"
        iface: "org.kde.KWin"
        function dbusshowingDesktopChanged(showing) { script.peek = showing === true; }
    }

    DBus.DBusServiceWatcher {
        busType: DBus.BusType.Session
        watchedService: "org.kde.KWin"
        onRegisteredChanged: {
            script.loaded = false;
            script.peek = false;
            if (registered)
                script.load();
        }
    }

    Component.onCompleted: {
        Shared.users++;
        start();
    }
    Component.onDestruction: {
        if (--Shared.users > 0)
            return;
        Shared.replaced = false;
        DBus.SessionBus.asyncCall({
            "service": "org.kde.KWin", "path": "/Scripting", "iface": "org.kde.kwin.Scripting",
            "member": "unloadScript", "arguments": [new DBus.string(name)], "signature": "(s)"
        });
    }
}
