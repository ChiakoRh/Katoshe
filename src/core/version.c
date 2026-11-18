#include "katoshe.h"

/* KATOSHE_VERSION comes from CMake (project VERSION) so there is one source of truth. */
const char *katoshe_version(void) {
#ifdef KATOSHE_VERSION
    return KATOSHE_VERSION;
#else
    return "dev";
#endif
}
