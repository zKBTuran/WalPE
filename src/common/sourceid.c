/* sourceid.c - see sourceid.h */
#include "common/sourceid.h"

#include "walpe-source-id.h"   /* WALPE_SOURCE_ID: meson.build, scripts/source-id.sh */

const char *walpe_source_id (void)
{
    return WALPE_SOURCE_ID;
}
