/* api.h - WALPE_API marks core functions the dlopen'd modules (walpe-web.so,
 * walpe-video.so) call back into. The project builds with
 * -fvisibility=hidden; these keep default visibility and the daemon exports
 * exactly them (src/daemon/modules.dynlist). */
#pragma once
#ifndef WALPE_API
#define WALPE_API __attribute__ ((visibility ("default")))
#endif
