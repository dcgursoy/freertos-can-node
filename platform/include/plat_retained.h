/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The retained-RAM cell: a few words that survive a reset but not a loss of supply, and the
 * reset cause of this start that the platform reads from them.
 *
 * A microcontroller keeps its RAM through a reset, so a small struct placed in a section that
 * the start-up code does not zero (".noinit") still holds after a reset what was written into
 * it before. After power-on it holds garbage. A magic number, a layout version and a CRC tell
 * the two apart: a cell that does not carry all three correctly is taken as a power-on and
 * written afresh. Together with the device's reset-cause flags (on an STM32 the RCC_CSR bits:
 * IWDGRSTF, SFTRSTF, PORRSTF, PINRSTF and others) it answers why the device started.
 *
 * WHAT IT HOLDS. The reason given to the last plat_reset(), the number of resets since
 * power-on, and words kept for phase 4 (the bootloader's count of boot attempts). The platform
 * reads it once, at start, before the application runs: plat_reset_cause() then returns the
 * reason, or PLAT_RESET_POWER_ON if the cell was not valid or recorded no reset. Right after
 * reading, the platform sets the recorded reason back to PLAT_RESET_POWER_ON, so that a run
 * that ends without plat_reset() (on the host: an orderly stop, or a killed process) leads to
 * a power-on at the next start, with every word reset to 0. plat_reset() writes its reason and
 * counts the reset; plat_reset(PLAT_RESET_POWER_LOSS) destroys the cell instead, as a loss of
 * supply would.
 *
 * ON A TARGET. A software reset (assert, stack overflow, a requested reset) writes its reason
 * into the cell, then triggers the reset (SCB AIRCR SYSRESETREQ). A watchdog reset writes
 * nothing, because no code runs before it: the start-up code finds IWDGRSTF set in RCC_CSR and
 * reports PLAT_RESET_WATCHDOG, and reads the cell for the software reasons; PORRSTF set means
 * power-on whatever the cell says. Then it clears the flags (RMVF).
 *
 * ON THE HOST (platform/posix/posix_retained.c). The cell is a file of exactly
 * sizeof(plat_retained_cell_t) bytes that the node maps shared (mmap MAP_SHARED), named with
 * `--retained-file PATH`. It outlives the process as RAM outlives a reset; a test bench removes
 * it to cut the power. A missing file, one of another size or one with a bad magic, version or
 * CRC is a power-on. The watchdog model calls plat_reset(PLAT_RESET_WATCHDOG) like any other
 * reset, so on the host the cell carries the watchdog's reason as well. Without the option
 * there is no cell and every start is a power-on.
 *
 * LAYOUT. Native byte order, little-endian on every machine the project builds for (x86-64,
 * Cortex-M); 36 bytes, no padding (checked below). The CRC is CRC-32/ISO-HDLC (lib/crc,
 * equal to zlib.crc32) over the 32 bytes before it, so a test in Python can check a cell with
 * nothing but the standard library.
 */
#ifndef FCN_PLAT_RETAINED_H
#define FCN_PLAT_RETAINED_H

#include <stddef.h>
#include <stdint.h>

#include "platform.h"

/** "FCNR" in a little-endian word: the bytes 'F', 'C', 'N', 'R'. */
#define PLAT_RETAINED_MAGIC 0x524E4346U

/** The layout below; a cell of another version is taken as a power-on. */
#define PLAT_RETAINED_VERSION 1U

/** Words kept for later phases; 0 after power-on, unchanged by a reset. */
#define PLAT_RETAINED_RESERVED_WORDS 4U

/** Index of the reserved word that phase 4's bootloader will use for its boot attempts. */
#define PLAT_RETAINED_BOOT_ATTEMPTS_WORD 0U

typedef struct {
    uint32_t magic;        /**< PLAT_RETAINED_MAGIC */
    uint16_t version;      /**< PLAT_RETAINED_VERSION */
    uint16_t size;         /**< sizeof(plat_retained_cell_t) */
    uint32_t reset_reason; /**< what the next start reports: a plat_reset_reason_t */
    uint32_t resets;       /**< plat_reset() calls since power-on */
    uint32_t reserved[PLAT_RETAINED_RESERVED_WORDS]; /**< for phase 4, see above */
    uint32_t crc; /**< CRC-32/ISO-HDLC of the bytes before this member */
} plat_retained_cell_t;

_Static_assert(sizeof(plat_retained_cell_t) == 36U, "the cell has no padding");
_Static_assert((size_t)offsetof(plat_retained_cell_t, reset_reason) == 8U, "layout version 1");
_Static_assert((size_t)offsetof(plat_retained_cell_t, reserved) == 16U, "layout version 1");
_Static_assert((size_t)offsetof(plat_retained_cell_t, crc) == 32U, "the CRC is the last word");

/**
 * The reset cause of this start: the reason given to the plat_reset() that ended the previous
 * run, or PLAT_RESET_POWER_ON if there was none (no cell, a cell that was not valid, or a
 * previous run that did not end with a reset). Constant while the image runs. Any task, any
 * time after the platform has started.
 */
plat_reset_reason_t plat_reset_cause(void);

#endif /* FCN_PLAT_RETAINED_H */
