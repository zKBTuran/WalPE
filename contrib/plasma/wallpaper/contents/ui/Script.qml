/* Script.qml - keeps the KWin script ../code/below.js loaded in KWin for as long as a
 * wallpaper of the type "WalPE" is alive in this Plasma shell.
 *
 * The script is no KWin package and is in no list of KWin's settings: the wallpaper type
 * loads it over D-Bus (org.kde.kwin.Scripting) and takes it out again when its last item
 * goes. So it cannot be switched off behind the type's back, and what the script told the
 * type (the key Under) is believed exactly while `loaded` is true. It is loaded under a
 * name of this shell's own (../code/shared.js): KWin takes a script out a moment after it
 * was asked to, and a fixed name would still be taken when the next shell, or the next
 * item, asks. KWin coming back on the bus (it was restarted) loads it again.
 *
 * It also listens to KWin's "showing the desktop" ("Peek at Desktop", the "Show Desktop"
 * shortcut): KWin then hides every window that is not part of the desktop, the daemon's
 * surface among them, and the desktop has to be filled for as long as that lasts.
 * And it watches for the name walpe-daemon takes on the bus while a wallpaper wants the
 * pointer (Pointer.qml). Anybody in the session can take a name, so the name only says
 * "wanted": the pointer goes to the connection that has it, by that connection's own
 * name, and only if the bus says it is the process whose surface KWin sees under this
 * desktop (the key Daemon, from the KWin script). Nothing here runs on a timer. */
import QtQuick
import org.kde.plasma.workspace.dbus as DBus
import "../code/shared.js" as Shared

Item {
    id: script
    property bool loaded: false
    property bool peek: false       // KWin is showing the desktop
    // the process that has a surface under this desktop (main.qml sets it; 0: none)
    property int daemon: 0
    // where the pointer goes: that process's connection while it has the name
    // io.github.walpe.Pointer (it has it only while a wallpaper uses the pointer); "": nowhere
    property string pointerTo: ""
    property int asked: 0
    readonly property string file: Qt.resolvedUrl("../code/below.js").toString().replace(/^file:\/\//, "")

    function call(path, iface, member, args, signature, then) {
        DBus.SessionBus.asyncCall({
            "service": "org.kde.KWin", "path": path, "iface": iface, "member": member,
            "arguments": args, "signature": signature
        }, then, () => { script.loaded = false; });
    }

    function load() {
        call("/Scripting", "org.kde.kwin.Scripting", "loadScript",
             [new DBus.string(file), new DBus.string(Shared.name)], "(ss)", (reply) => {
            if (Number(reply.value) < 0) {
                script.loaded = true;      // the name is taken: another item of this shell has loaded it
                return;
            }
            // "start" runs every loaded script that is not running yet. Not "run" on the script's
            // own object, /Scripting/Script<id>: the id KWin answers is the number of scripts it
            // has loaded, and after another script was unloaded that is the id, and the object,
            // of a script that is still there (KWin 6.8).
            call("/Scripting", "org.kde.kwin.Scripting", "start", [], "()", () => { script.loaded = true; });
        });
    }

    function findPointer() {
        pointerTo = "";
        const mine = ++asked;
        if (!pointer.registered || daemon <= 0)
            return;
        const bus = (member, arg, then) => DBus.SessionBus.asyncCall({
            "service": "org.freedesktop.DBus", "path": "/org/freedesktop/DBus",
            "iface": "org.freedesktop.DBus", "member": member,
            "arguments": [new DBus.string(arg)], "signature": "(s)"
        }, then, () => {});
        bus("GetNameOwner", "io.github.walpe.Pointer", (owner) => {
            const connection = String(owner.value);
            bus("GetConnectionUnixProcessID", connection, (pid) => {
                if (mine === script.asked && Number(pid.value) === script.daemon)
                    script.pointerTo = connection;
            });
        });
    }
    onDaemonChanged: findPointer()

    DBus.SignalWatcher {
        busType: DBus.BusType.Session
        service: "org.kde.KWin"
        path: "/KWin"
        iface: "org.kde.KWin"
        function dbusshowingDesktopChanged(showing) { script.peek = showing === true; }
    }

    DBus.DBusServiceWatcher {
        id: pointer
        busType: DBus.BusType.Session
        watchedService: "io.github.walpe.Pointer"
        onRegisteredChanged: script.findPointer()
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
        load();
    }
    Component.onDestruction: {
        if (--Shared.users > 0)
            return;
        DBus.SessionBus.asyncCall({
            "service": "org.kde.KWin", "path": "/Scripting", "iface": "org.kde.kwin.Scripting",
            "member": "unloadScript", "arguments": [new DBus.string(Shared.name)], "signature": "(s)"
        });
        Shared.name = Shared.fresh();       // whoever comes next does not meet the one that is leaving
    }
}
