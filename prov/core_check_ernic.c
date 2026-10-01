/*
 * The glue's view of the core (uet_core.h) must agree with libuet_ernic
 * (uet_ernic.h), the core of a CORE=ernic build: compiled together with
 * -Werror, a prototype or constant that differs fails the build. Both
 * already promise to match uet_api.h; this keeps that promise checked for
 * the pair actually linked.
 */

#include "uet_core.h"
#include "uet_ernic.h"
