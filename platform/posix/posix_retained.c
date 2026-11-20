/* SPDX-License-Identifier: MIT */
/*
 * The retained-RAM cell of the host build: plat_retained.h says what it holds and why, and
 * posix_retained.h what each function does. This comment says how the file stands in for RAM.
 *
 * THE FILE IS THE RAM. The cell is mapped with MAP_SHARED, so every store to it lands in the
 * kernel's page cache at once and is seen by the next process that opens the file, whether or
 * not this one exits normally: _exit() in plat_reset() loses nothing, and no msync() is needed.
 * Only a crash of the host's kernel could lose the stores, and that is a loss of supply anyway.
 *
 * A TORN WRITE IS A POWER-ON. plat_reset() writes the reason, the count and then the CRC. A
 * process killed between those stores (SIGKILL from the bench) leaves a cell whose CRC does not
 * match, which the next start takes as a power-on, as garbage in RAM after a power cycle would
 * be.
 *
 * ONE BOARD, ONE CELL. The file is locked with flock() while it is open, and a second node
 * that names it fails to start: two nodes writing one cell would make each other's cause up.
 */
#include "posix_retained.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc/crc.h"

static plat_retained_cell_t *s_cell; /* the mapping, or NULL */
static int s_fd = -1;                /* open, and locked, while the cell is mapped */
static posix_retained_info_t s_info = {
    .open = false, .cause = PLAT_RESET_POWER_ON, .resets = 0U, .verdict = "no cell"};

bool posix_retained_reason_recorded(uint32_t reason)
{
    bool recorded = false;

    switch (reason) {
    case (uint32_t)PLAT_RESET_REQUESTED:
    case (uint32_t)PLAT_RESET_WATCHDOG:
    case (uint32_t)PLAT_RESET_IMAGE_FAULT:
    case (uint32_t)PLAT_RESET_ASSERT:
    case (uint32_t)PLAT_RESET_STACK_OVERFLOW:
    case (uint32_t)PLAT_RESET_MALLOC_FAILED:
        recorded = true;
        break;
    default:
        recorded = false;
        break;
    }

    return recorded;
}

bool posix_reset_reason_valid(plat_reset_reason_t reason)
{
    return posix_retained_reason_recorded((uint32_t)reason) || (reason == PLAT_RESET_POWER_LOSS);
}

const char *posix_reset_reason_name(plat_reset_reason_t reason)
{
    const char *name;

    switch (reason) {
    case PLAT_RESET_POWER_ON:
        name = "POWER_ON";
        break;
    case PLAT_RESET_REQUESTED:
        name = "REQUESTED";
        break;
    case PLAT_RESET_WATCHDOG:
        name = "WATCHDOG";
        break;
    case PLAT_RESET_POWER_LOSS:
        name = "POWER_LOSS";
        break;
    case PLAT_RESET_IMAGE_FAULT:
        name = "IMAGE_FAULT";
        break;
    case PLAT_RESET_ASSERT:
        name = "ASSERT";
        break;
    case PLAT_RESET_STACK_OVERFLOW:
        name = "STACK_OVERFLOW";
        break;
    case PLAT_RESET_MALLOC_FAILED:
        name = "MALLOC_FAILED";
        break;
    default:
        name = "unknown"; /* a value outside the enumeration, from damaged memory */
        break;
    }

    return name;
}

uint32_t posix_retained_crc(const plat_retained_cell_t *cell)
{
    /* The CRC covers the members before it, which are all the bytes before it (no padding:
     * plat_retained.h checks the offsets). */
    return crc32_compute((const uint8_t *)cell, offsetof(plat_retained_cell_t, crc));
}

/* Why a cell that was read is not a reset, or NULL if it records one. */
static const char *judge(const plat_retained_cell_t *cell)
{
    const char *verdict = NULL;

    if (cell->magic != PLAT_RETAINED_MAGIC) {
        verdict = "magic";
    } else if ((cell->version != PLAT_RETAINED_VERSION) ||
               (cell->size != (uint16_t)sizeof(plat_retained_cell_t))) {
        verdict = "version";
    } else if (cell->crc != posix_retained_crc(cell)) {
        verdict = "crc";
    } else if (cell->reset_reason == (uint32_t)PLAT_RESET_POWER_ON) {
        verdict = "no reset";
    } else if (!posix_retained_reason_recorded(cell->reset_reason)) {
        verdict = "reason";
    } else {
        verdict = NULL;
    }

    return verdict;
}

static void seal(plat_retained_cell_t *cell)
{
    cell->crc = posix_retained_crc(cell);
}

/* A cell as after power-on: every word 0 but the header, nothing recorded. */
static void initialise(plat_retained_cell_t *cell)
{
    (void)memset(cell, 0, sizeof(*cell));
    cell->magic = PLAT_RETAINED_MAGIC;
    cell->version = (uint16_t)PLAT_RETAINED_VERSION;
    cell->size = (uint16_t)sizeof(plat_retained_cell_t);
    cell->reset_reason = (uint32_t)PLAT_RESET_POWER_ON;
    seal(cell);
}

static bool fail(char *error, size_t error_len, const char *what, const char *path, int fd)
{
    const int saved = errno;

    if (fd >= 0) {
        (void)close(fd);
    }

    (void)snprintf(error, error_len, "cannot %s --retained-file %s: %s", what, path,
                   strerror(saved));
    return false;
}

bool posix_retained_open(const char *path, char *error, size_t error_len)
{
    struct stat info;
    bool existed;
    const char *size_verdict = NULL;
    int fd;
    void *map;

    if (s_cell != NULL) {
        (void)snprintf(error, error_len, "a retained cell is already open");
        return false;
    }

    if (path == NULL) {
        s_info = (posix_retained_info_t){
            .open = false, .cause = PLAT_RESET_POWER_ON, .resets = 0U, .verdict = "no cell"};
        return true;
    }

    existed = (stat(path, &info) == 0);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

    if (fd < 0) {
        return fail(error, error_len, "open", path, -1);
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            (void)close(fd);
            (void)snprintf(error, error_len, "--retained-file %s is in use by another node", path);
            return false;
        }

        return fail(error, error_len, "lock", path, fd);
    }

    if (fstat(fd, &info) != 0) {
        return fail(error, error_len, "inspect", path, fd);
    }

    if (!S_ISREG(info.st_mode)) {
        errno = EINVAL;
        return fail(error, error_len, "use (not a regular file)", path, fd);
    }

    if (info.st_size != (off_t)sizeof(plat_retained_cell_t)) {
        /* Not a cell of this layout: give the file the cell's size; its content is garbage
         * and is replaced below. */
        size_verdict = existed ? "size" : "missing";

        if (ftruncate(fd, (off_t)sizeof(plat_retained_cell_t)) != 0) {
            return fail(error, error_len, "size", path, fd);
        }
    }

    map = mmap(NULL, sizeof(plat_retained_cell_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (map == MAP_FAILED) {
        return fail(error, error_len, "map", path, fd);
    }

    s_cell = (plat_retained_cell_t *)map;
    s_fd = fd;

    {
        const char *verdict = (size_verdict != NULL) ? size_verdict : judge(s_cell);

        if (verdict == NULL) {
            /* A reset: report its reason once, and keep the count and the reserved words. */
            s_info = (posix_retained_info_t){.open = true,
                                             .cause = (plat_reset_reason_t)s_cell->reset_reason,
                                             .resets = s_cell->resets,
                                             .verdict = "reset"};
            s_cell->reset_reason = (uint32_t)PLAT_RESET_POWER_ON;
            seal(s_cell);
        } else {
            s_info = (posix_retained_info_t){
                .open = true, .cause = PLAT_RESET_POWER_ON, .resets = 0U, .verdict = verdict};
            initialise(s_cell);
        }
    }

    return true;
}

void posix_retained_note_reset(plat_reset_reason_t reason)
{
    plat_retained_cell_t *const cell = s_cell;

    if (cell != NULL) {
        if (reason == PLAT_RESET_POWER_LOSS) {
            (void)memset(cell, 0, sizeof(*cell)); /* RAM does not survive a loss of supply */
        } else {
            cell->reset_reason = (uint32_t)reason;
            cell->resets++;
            seal(cell);
        }
    }
}

void posix_retained_close(void)
{
    if (s_cell != NULL) {
        (void)munmap(s_cell, sizeof(plat_retained_cell_t));
        s_cell = NULL;
        (void)close(s_fd); /* releases the lock */
        s_fd = -1;
    }
}

void posix_retained_get_info(posix_retained_info_t *out)
{
    *out = s_info;
}

plat_reset_reason_t plat_reset_cause(void)
{
    return s_info.cause;
}
