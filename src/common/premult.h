/* premult.h - RGBA premultiply for texture uploads (validated C path) */
#pragma once
#include <glib.h>
void walpe_premultiply_rgba8 (guint8 *buf, gsize n_bytes);
