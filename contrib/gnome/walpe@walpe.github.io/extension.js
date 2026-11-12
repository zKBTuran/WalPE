/* extension.js - the WalPE add-on for GNOME Shell (Wayland sessions).
 *
 * GNOME's compositor has no protocol with which a program can put a
 * surface on the desktop (no layer shell), so walpe-daemon cannot show a
 * wallpaper there by itself. With this extension it can: the daemon opens
 * one ordinary window per monitor, application id "io.github.walpe.wallpaper",
 * title "WalPE wallpaper: <connector>", and the extension
 *   - gives each such window the window type DESKTOP, which is all the
 *     shell needs to keep it below every other window, on all workspaces
 *     and out of the overview, the window switcher and the dash,
 *   - puts it on its monitor at the monitor's size, and keeps it below the
 *     windows of other programs that are of that type (desktop icons),
 *   - shows it behind the workspaces in the overview (a clone of the
 *     window's actor inside the shell's own background actors; the lock
 *     screen keeps GNOME's own picture),
 *   - tells the daemon, per monitor and only when it changes, whether
 *     windows cover the monitor and whether the focused window is an
 *     application's and fullscreen (the daemon's pause rules).
 * The daemon finds the extension by its name on the session bus and asks
 * there; the extension starts nothing by itself and keeps no timer running.
 *
 * GNOME Shell 49 and newer can change the type of any window. 46 to 48 can
 * do it only for windows of a Wayland client that the shell itself has
 * started: there the daemon asks for its connection to the compositor
 * (method Socket), and the shell starts a second walpe-daemon process whose
 * only work is to hand the connection it was started with to the first.
 *
 * Part of WalPE, BSD-3-Clause (LICENSE in the source tree). */
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

import * as Background from 'resource:///org/gnome/shell/ui/background.js';
import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

const PROTOCOL = 1;
const BUS_NAME = 'io.github.walpe.Gnome';
const OBJECT_PATH = '/io/github/walpe/Gnome';
const APP_ID = 'io.github.walpe.wallpaper';
const TITLE_PREFIX = 'WalPE wallpaper: ';
/* a monitor counts as covered when windows hide this much of it, looked
 * at on a grid (the numbers of the daemon's X11 backend) */
const GRID = 24;
const COVER = 0.85;
const LOOK_DELAY_MS = 40;

const INTERFACE = `
<node><interface name="io.github.walpe.Gnome">
  <method name="Hello"><arg type="a{sv}" direction="out" name="about"/></method>
  <method name="Socket"><arg type="h" direction="in" name="back"/></method>
  <method name="Windows">
    <arg type="s" direction="in" name="see_through"/>
    <arg type="s" direction="out" name="state"/>
  </method>
  <signal name="WindowsChanged"><arg type="s" name="state"/></signal>
</interface></node>`;

/* GNOME Shell 46 to 48: the Wayland clients the shell has started for a
 * daemon, as {client, process}. They are kept while their process lives,
 * also while the extension is disabled: the shell disables and enables
 * extensions whenever another one is switched on or off, and the compositor
 * closes a client's connection when its object here is let go. */
const donors = [];

function connectorOf(window) {
    if (window.get_client_type() !== Meta.WindowClientType.WAYLAND || window.get_wm_class() !== APP_ID)
        return null;
    const title = window.get_title();
    if (!title?.startsWith(TITLE_PREFIX) || title.length === TITLE_PREFIX.length)
        return null;
    return title.slice(TITLE_PREFIX.length);
}

export default class WalpeExtension extends Extension {
    enable() {
        this._canSetType = typeof Meta.Window.prototype.set_type === 'function';
        this._wallpapers = new Map();   /* window -> connector */
        this._backgrounds = new Map();  /* background actor -> {monitor, clone} */
        this._seeThrough = [];
        this._state = '';
        this._lookId = 0;
        this._lowered = [];

        this._object = Gio.DBusExportedObject.wrapJSObject(INTERFACE, this);
        this._object.export(Gio.DBus.session, OBJECT_PATH);
        this._nameId = Gio.bus_own_name_on_connection(Gio.DBus.session, BUS_NAME,
            Gio.BusNameOwnerFlags.NONE, null, null);

        global.display.connectObject(
            'window-created', (_d, window) => this._watch(window),
            'restacked', () => { this._keepLowest(); this._kick(); },
            'notify::focus-window', () => this._kick(),
            'in-fullscreen-changed', () => this._kick(),
            this);
        global.workspace_manager.connectObject('active-workspace-changed', () => this._kick(), this);
        Main.sessionMode.connectObject('updated', () => this._kick(), this);
        Main.layoutManager.connectObject('monitors-changed', () => {
            for (const window of this._wallpapers.keys())
                this._place(window);
            this._kick();
        }, this);

        /* every background the shell makes from now on: the one of a
         * workspace in the overview, of a workspace thumbnail, of the
         * workspaces that slide by when the workspace is changed */
        const extension = this;
        this._injections = new InjectionManager();
        this._injections.overrideMethod(Background.BackgroundManager.prototype, '_createBackgroundActor',
            original => function (...args) {
                const actor = original.apply(this, args);
                try {
                    extension._backgroundMade(this, actor);
                } catch (e) {
                    console.error(`walpe: ${e}`);
                }
                return actor;
            });

        for (const window of global.display.list_all_windows())
            this._watch(window);
    }

    disable() {
        /* The wallpaper windows stay as they are, desktop windows the
         * daemon draws on. What ends: the name on the bus (a daemon that
         * starts now is told that there is no extension), the wallpaper
         * in the overview, the reports. */
        if (this._lookId)
            GLib.source_remove(this._lookId);
        this._lookId = 0;
        this._injections.clear();
        this._injections = null;
        for (const [actor, entry] of this._backgrounds) {
            actor.disconnectObject(this);
            entry.clone?.destroy();
        }
        this._backgrounds = null;
        for (const window of global.display.list_all_windows())
            window.disconnectObject(this);
        this._wallpapers = null;
        global.display.disconnectObject(this);
        global.workspace_manager.disconnectObject(this);
        Main.sessionMode.disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        Gio.bus_unown_name(this._nameId);
        this._nameId = 0;
        this._object.unexport();
        this._object = null;
    }

    /* ---- the wallpaper windows */

    _watch(window) {
        /* a new window has neither title nor application id yet; both are
         * there before it is shown. Its place and size matter to the
         * reports whoever it belongs to. */
        window.connectObject(
            'notify::title', () => this._consider(window),
            'notify::wm-class', () => this._consider(window),
            'shown', () => { this._place(window); this._kick(); },
            'position-changed', () => this._kick(),
            'size-changed', () => this._kick(),
            'notify::minimized', () => this._kick(),
            'workspace-changed', () => this._kick(),
            'unmanaged', () => this._gone(window),
            this);
        this._consider(window);
    }

    _consider(window) {
        if (!this._wallpapers || this._wallpapers.has(window) || !connectorOf(window))
            return;
        /* The title is told while the window is being made. Its type is
         * changed when the compositor has dealt with the rest of what the
         * client sent with it, and long before the window is shown. */
        GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._adopt(window);
            return GLib.SOURCE_REMOVE;
        });
    }

    _adopt(window) {
        if (!this._wallpapers || this._wallpapers.has(window))
            return;
        const connector = connectorOf(window);
        if (!connector)
            return;
        if (window.get_window_type() !== Meta.WindowType.DESKTOP) {
            if (this._canSetType) {
                window.set_type(Meta.WindowType.DESKTOP);
            } else {
                const donor = donors.find(d => d.client.owns_window(window));
                if (!donor) {
                    /* (a walpe-daemon that did not ask for its connection, or
                     * some other program using the name: an ordinary window) */
                    console.warn(`walpe: "${window.get_title()}" is not a window of a client this shell started: left alone`);
                    return;
                }
                donor.client.make_desktop(window);
            }
        }
        this._wallpapers.set(window, connector);
        if (window.get_compositor_private() && !window.is_hidden())
            this._place(window);
        this._syncClones();
        this._kick();
    }

    _gone(window) {
        window.disconnectObject(this);
        if (this._wallpapers?.delete(window))
            this._syncClones();
        this._kick();
    }

    _monitorOf(connector) {
        return global.backend.get_monitor_manager().get_monitor_for_connector(connector);
    }

    _place(window) {
        const connector = this._wallpapers?.get(window);
        if (!connector)
            return;
        const monitor = this._monitorOf(connector);
        if (monitor < 0)
            return;     /* (the daemon makes its windows anew when the monitors change) */
        const want = global.display.get_monitor_geometry(monitor);
        const have = window.get_frame_rect();
        if (have.x !== want.x || have.y !== want.y || have.width !== want.width || have.height !== want.height)
            window.move_resize_frame(false, want.x, want.y, want.width, want.height);
        window.lower();
        this._syncClones();
    }

    /* Other programs have desktop windows too (the icons of "Desktop Icons
     * NG" are one, see-through where there is no icon): the wallpaper
     * belongs below them. Not more often than a few times a second, in
     * case such a program insists on the lowest place as well. */
    _keepLowest() {
        if (!this._wallpapers?.size)
            return;
        const lower = [];
        let others = false;
        /* (the display's own list, lowest first: extensions change what
         * global.get_window_actors() returns to hide their windows) */
        for (const window of global.display.sort_windows_by_stacking(global.display.list_all_windows())) {
            if (window.get_window_type() !== Meta.WindowType.DESKTOP)
                continue;
            if (!this._wallpapers.has(window))
                others = true;
            else if (others)
                lower.push(window);
        }
        if (!lower.length)
            return;
        const now = GLib.get_monotonic_time();
        this._lowered = this._lowered.filter(t => now - t < 1000000);
        if (this._lowered.length >= 5)
            return;
        this._lowered.push(now);
        for (const window of lower.reverse())
            window.lower();
    }

    /* ---- the wallpaper in the shell's own backgrounds */

    _backgroundMade(manager, actor) {
        /* not the desktop itself (the wallpaper window lies right above
         * that background), not the lock screen, and not the workspaces
         * that slide by when the workspace is changed: there the shell
         * shows desktop windows by itself */
        const container = manager._container;
        if (container === Main.layoutManager._backgroundGroup ||
            container?.has_style_class_name?.('screen-shield-background') ||
            container?.get_parent()?.constructor.name === 'WorkspaceGroup')
            return;
        this._backgrounds.set(actor, {monitor: manager._monitorIndex, clone: null});
        actor.connectObject('destroy', () => this._backgrounds?.delete(actor), this);
        this._syncClones();
    }

    _syncClones() {
        if (!this._backgrounds)
            return;
        const actors = new Map();   /* monitor -> the window actor of its wallpaper */
        for (const [window, connector] of this._wallpapers) {
            const actor = window.get_compositor_private();
            const monitor = this._monitorOf(connector);
            if (actor && monitor >= 0)
                actors.set(monitor, actor);
        }
        for (const [background, entry] of this._backgrounds) {
            const source = actors.get(entry.monitor) ?? null;
            if ((entry.clone?.source ?? null) === source)
                continue;
            entry.clone?.destroy();
            entry.clone = null;
            if (!source)
                continue;
            /* as large as the background, without a size of its own: the
             * shell lays its backgrounds out by what they ask for */
            entry.clone = new Clutter.Clone({source, width: 0, height: 0});
            entry.clone.add_constraint(new Clutter.BindConstraint({
                source: background,
                coordinate: Clutter.BindCoordinate.SIZE,
            }));
            background.add_child(entry.clone);
            source.connectObject('destroy', () => {
                if (entry.clone?.source === source) {
                    entry.clone.destroy();
                    entry.clone = null;
                }
            }, entry.clone);
        }
    }

    /* ---- what the daemon's pause rules ask about: src/signals/gnome.c */

    /* an application's window: not the wallpaper, a panel, a menu, and not
     * one that is kept out of the window lists (the window "Desktop Icons
     * NG" covers the monitor with is an ordinary one of that kind) */
    _isApp(window) {
        if (!window || this._wallpapers.has(window) || window.is_override_redirect() ||
            window.is_skip_taskbar())
            return false;
        switch (window.get_window_type()) {
        case Meta.WindowType.NORMAL:
        case Meta.WindowType.DIALOG:
        case Meta.WindowType.MODAL_DIALOG:
        case Meta.WindowType.UTILITY:
            return true;
        default:
            return false;
        }
    }

    _isSeeThrough(window) {
        return [window.get_wm_class(), window.get_wm_class_instance(), window.get_gtk_application_id()]
            .some(name => name && this._seeThrough.includes(name.toLowerCase()));
    }

    /* One line per monitor: "NAME<tab>FLAGS", FLAGS of "c" (windows cover
     * it), "a" (the focused window is an application's and is on it), "f"
     * (that window is fullscreen). NAME is the connector where a wallpaper
     * window has told it, else "monitor-<index>". */
    _look() {
        const workspace = global.workspace_manager.get_active_workspace();
        const apps = global.display.list_all_windows().filter(w =>
            this._isApp(w) && w.showing_on_its_workspace() && w.located_on_workspace(workspace));
        const rects = apps.filter(w => !this._isSeeThrough(w)).map(w => w.get_frame_rect());
        const focus = global.display.focus_window;
        const focused = apps.includes(focus) ? focus : null;
        const names = new Map();
        for (const connector of this._wallpapers.values())
            names.set(this._monitorOf(connector), connector);
        const need = Math.ceil(COVER * GRID * GRID);
        const lines = [];
        for (let i = 0; i < global.display.get_n_monitors(); i++) {
            const g = global.display.get_monitor_geometry(i);
            let marked = 0;
            for (let cy = 0; cy < GRID && rects.length; cy++) {
                for (let cx = 0; cx < GRID; cx++) {
                    const px = g.x + (cx + 0.5) * g.width / GRID, py = g.y + (cy + 0.5) * g.height / GRID;
                    if (rects.some(r => px >= r.x && px < r.x + r.width && py >= r.y && py < r.y + r.height))
                        marked++;
                }
            }
            /* (a locked screen shows no window, the wallpaper's neither) */
            let flags = marked >= need || !Main.sessionMode.hasWindows ? 'c' : '';
            if (focused?.get_monitor() === i)
                flags += focused.is_fullscreen() ? 'af' : 'a';
            lines.push(`${names.get(i) ?? `monitor-${i}`}\t${flags}`);
        }
        return lines.join('\n');
    }

    /* something about the windows changed. While a window is dragged that
     * is all the time: the look waits until it rests. Nothing is looked
     * at while no wallpaper window exists. */
    _kick() {
        if (this._lookId || !this._wallpapers?.size)
            return;
        this._lookId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, LOOK_DELAY_MS, () => {
            this._lookId = 0;
            const state = this._look();
            if (state !== this._state) {
                this._state = state;
                this._object.emit_signal('WindowsChanged', new GLib.Variant('(s)', [state]));
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    /* ---- io.github.walpe.Gnome */

    Hello() {
        return {
            protocol: new GLib.Variant('u', PROTOCOL),
            extension: new GLib.Variant('s', this.metadata['version-name'] ?? ''),
            shell: new GLib.Variant('s', Config.PACKAGE_VERSION),
            /* true: only windows of a client this shell started can be
             * desktop windows; the daemon asks for its connection (Socket) */
            socket: new GLib.Variant('b', !this._canSetType),
        };
    }

    /* see_through: the daemon's "see-through windows" setting, application
     * ids separated by commas */
    Windows(seeThrough) {
        this._seeThrough = seeThrough.split(',').map(s => s.trim().toLowerCase()).filter(s => s);
        this._state = this._look();
        return this._state;
    }

    /* GNOME Shell 46 to 48. back: one end of a socket pair of the caller.
     * The shell starts the caller's own program again - the file behind
     * /proc/<pid>/exe, nothing the caller can name - as a Wayland client,
     * with `back` as descriptor 4; that process sends the connection it was
     * given through `back` and stays until `back` is closed. */
    SocketAsync(params, invocation) {
        const fail = text => invocation.return_dbus_error('io.github.walpe.Gnome.Error', text);
        if (this._canSetType) {
            fail('this GNOME Shell makes any window a desktop window: no connection is handed out');
            return;
        }
        const fds = invocation.get_message().get_unix_fd_list();
        const [index] = params;
        if (!fds || index < 0 || index >= fds.get_length()) {
            fail('no descriptor came with the call');
            return;
        }
        const back = fds.get(index);
        Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetConnectionUnixProcessID', new GLib.Variant('(s)', [invocation.get_sender()]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, 1000, null, (bus, result) => {
                const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.NONE});
                launcher.take_fd(back, 4);
                try {
                    const [pid] = bus.call_finish(result).deepUnpack();
                    const exe = GLib.file_read_link(`/proc/${pid}/exe`);
                    if (!GLib.path_is_absolute(exe) || exe.endsWith(' (deleted)'))
                        throw new Error(`the program of process ${pid} is not a file any more (${exe})`);
                    const client = Meta.WaylandClient.new(global.context, launcher);
                    const process = client.spawnv(global.display, [exe, '--wayland-socket-to', '4']);
                    const donor = {client, process};
                    donors.push(donor);
                    process.wait_async(null, () => donors.splice(donors.indexOf(donor), 1));
                    invocation.return_value(null);
                } catch (e) {
                    launcher.close();
                    fail(`${e.message ?? e}`);
                }
            });
    }
}
