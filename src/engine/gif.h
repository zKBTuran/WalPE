/* gif.h - animated gif/webp/apng backend via GdkPixbufAnimation */
#pragma once
#include "content.h"

WalpeContent *walpe_gif_new (const char *path, GError **err);
