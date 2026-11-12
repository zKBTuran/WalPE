# libgstwalpesound.so (walpesound.c): the GStreamer plugin that is the sound
# output of the page process of web wallpapers. Alone in this directory,
# here and installed (<moddir>/gst): the daemon puts the directory on
# GST_PLUGIN_PATH (src/daemon/main.c web_env_sound_sink), and GStreamer
# loads every module it finds below such a directory.
web_sound_sink = shared_module('gstwalpesound', 'walpesound.c',
  name_prefix: 'lib',
  c_args: ['-fvisibility=hidden'],
  dependencies: [gst_dep],
  install: true,
  install_dir: moddir / 'gst')
