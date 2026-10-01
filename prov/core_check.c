/*
 * Compile-only check: uet_core.h must declare the same signatures as
 * uet_api.h. Built against the vendored headers, like the core itself.
 */
#include "uet_api.h"
#include "uet_core.h"

int uet_core_check_dummy;
