#!/usr/bin/env python3
"""Mock MPRIS 2 player for WalPE media-integration tests.

usage: mockplayer.py NAME CONTROL_FIFO [--title T --artist A --album B --art URL --length S --state Playing]
Control lines (written to the FIFO):
  meta title=..|artist=..|album=..|art=URL|length=S   (fields separated by '|')
  state Playing|Paused|Stopped
  seek SECONDS
  quit
"""
import sys, time, os, argparse
import dbus, dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

IFACE = 'org.mpris.MediaPlayer2.Player'
ROOT = 'org.mpris.MediaPlayer2'
PATH = '/org/mpris/MediaPlayer2'

class Player(dbus.service.Object):
    def __init__(self, bus, name, a):
        self.busname = dbus.service.BusName('org.mpris.MediaPlayer2.' + name, bus)
        super().__init__(bus, PATH)
        self.state = a.state
        self.meta = {'title': a.title, 'artist': a.artist, 'album': a.album, 'art': a.art,
                     'length': str(a.length)}
        self.track = 1
        self.pos_us = 0
        self.stamp = time.monotonic()

    def position(self):
        p = self.pos_us
        if self.state == 'Playing':
            p += int((time.monotonic() - self.stamp) * 1e6)
        return p

    def metadata(self):
        m = {'mpris:trackid': dbus.ObjectPath('/org/walpe/mock/track/%d' % self.track),
             'xesam:title': self.meta['title'],
             'xesam:artist': dbus.Array([self.meta['artist']], signature='s'),
             'xesam:album': self.meta['album'],
             'xesam:albumArtist': dbus.Array([self.meta['artist']], signature='s'),
             'xesam:genre': dbus.Array(['Test'], signature='s'),
             'mpris:length': dbus.Int64(int(float(self.meta['length']) * 1e6))}
        if self.meta.get('art'):
            m['mpris:artUrl'] = self.meta['art']
        return dbus.Dictionary(m, signature='sv')

    def props(self):
        return {'PlaybackStatus': self.state, 'Metadata': self.metadata(),
                'Position': dbus.Int64(self.position()), 'Rate': 1.0, 'Volume': 1.0,
                'MinimumRate': 1.0, 'MaximumRate': 1.0, 'CanPlay': True, 'CanPause': True,
                'CanSeek': True, 'CanControl': True, 'CanGoNext': False, 'CanGoPrevious': False,
                'LoopStatus': 'None', 'Shuffle': False}

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='ss', out_signature='v')
    def Get(self, iface, prop):
        if iface == ROOT:
            return {'Identity': 'WalPE mock player', 'CanQuit': False, 'CanRaise': False,
                    'HasTrackList': False, 'SupportedUriSchemes': dbus.Array([], signature='s'),
                    'SupportedMimeTypes': dbus.Array([], signature='s')}[prop]
        return self.props()[prop]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        if iface == ROOT:
            return {'Identity': 'WalPE mock player', 'CanQuit': False, 'CanRaise': False,
                    'HasTrackList': False}
        return self.props()

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='ssv')
    def Set(self, iface, prop, val):
        pass

    @dbus.service.signal(dbus.PROPERTIES_IFACE, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    @dbus.service.signal(IFACE, signature='x')
    def Seeked(self, pos):
        pass

    @dbus.service.method(IFACE)
    def PlayPause(self):
        self.set_state('Paused' if self.state == 'Playing' else 'Playing')

    def set_state(self, st):
        self.pos_us = self.position()
        self.stamp = time.monotonic()
        self.state = st
        if st == 'Stopped':
            self.pos_us = 0
        self.PropertiesChanged(IFACE, {'PlaybackStatus': st}, [])

    def command(self, line):
        line = line.strip()
        if not line:
            return
        cmd, _, arg = line.partition(' ')
        if cmd == 'meta':
            for kv in arg.split('|'):
                k, _, v = kv.partition('=')
                self.meta[k] = v
            self.track += 1
            self.pos_us = 0
            self.stamp = time.monotonic()
            self.PropertiesChanged(IFACE, {'Metadata': self.metadata()}, [])
            self.Seeked(dbus.Int64(0))
        elif cmd == 'state':
            self.set_state(arg)
        elif cmd == 'seek':
            self.pos_us = int(float(arg) * 1e6)
            self.stamp = time.monotonic()
            self.Seeked(dbus.Int64(self.pos_us))
        elif cmd == 'quit':
            loop.quit()
        print('mock: %s' % line, flush=True)

ap = argparse.ArgumentParser()
ap.add_argument('name'); ap.add_argument('fifo')
ap.add_argument('--title', default='Test Track'); ap.add_argument('--artist', default='Test Artist')
ap.add_argument('--album', default='Test Album'); ap.add_argument('--art', default='')
ap.add_argument('--length', type=float, default=215.0); ap.add_argument('--state', default='Playing')
a = ap.parse_args()
DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
p = Player(bus, a.name, a)
if not os.path.exists(a.fifo):
    os.mkfifo(a.fifo)
fd = os.open(a.fifo, os.O_RDWR | os.O_NONBLOCK)
buf = b''
def on_fifo(src, cond):
    global buf
    try:
        buf += os.read(fd, 4096)
    except BlockingIOError:
        return True
    while b'\n' in buf:
        l, buf = buf.split(b'\n', 1)
        p.command(l.decode())
    return True
GLib.io_add_watch(fd, GLib.IO_IN, on_fifo)
loop = GLib.MainLoop()
print('mock: org.mpris.MediaPlayer2.%s on %s' % (a.name, os.environ.get('DBUS_SESSION_BUS_ADDRESS')), flush=True)
loop.run()
