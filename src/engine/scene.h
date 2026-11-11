/* scene.h - Wallpaper Engine "scene" (scene.json) renderer */
#pragma once
#include "content.h"

WalpeContent *walpe_scene_new (const char *project_dir, const char *scene_file,
                               const WalpeLoadOpts *opts, GError **err);
