/* scenenode.h - renderer for the modern WE scene format ("objects" array) */
#pragma once
#include "content.h"

/* takes ownership of the parsed scene json root (unrefs it on free);
 * project_dir = the dir the scene json was found in (pkg cache or plain) */
WalpeContent *walpe_scenenode_new_from_root (JsonNode *root, const char *project_dir,
                                             const WalpeLoadOpts *opts, GError **err);

#define SCENENODE_MAX_DEPTH 8
#define SCENENODE_MAX_PARENTS 256   /* longest "parent" chain kept (scenenode.c) */

/* WE install assets locator (walpe_we_assets_dir / _hint) */
#include "common/weassets.h"
