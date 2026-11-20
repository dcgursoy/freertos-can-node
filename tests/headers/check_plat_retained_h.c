/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "plat_retained.h"

/* A translation unit has to declare something, and this also uses the header: the word for
 * phase 4's boot attempts is one of the reserved words, and the cause of a power-on is no exit
 * status of a reset. */
_Static_assert(PLAT_RETAINED_BOOT_ATTEMPTS_WORD < PLAT_RETAINED_RESERVED_WORDS,
               "the boot attempts are a reserved word");
_Static_assert((int)PLAT_RESET_POWER_ON < (int)PLAT_RESET_REQUESTED,
               "a power-on is not a reset reason");
