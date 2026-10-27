// mk_env.h - debug/trace environment hooks (MK_DIAG, MK_SCR, MK_AUTONAV, ...).
// An env var counts as ON only when set to something other than "0": a plain
// presence test would make `MK_DIAG=0` mean "on", which contradicts every
// other tool's value semantics.
#ifndef MK_ENV_H
#define MK_ENV_H

#include <stdlib.h>
#include <string.h>

static int mk_env_on(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && strcmp(v, "0") != 0;
}

#endif // MK_ENV_H
