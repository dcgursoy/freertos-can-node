/* SPDX-License-Identifier: MIT */
/*
 * The retained-RAM cell of the host build (platform/posix/posix_retained.c) without a kernel:
 * the test plays the start-up code and plat_reset() of successive runs of the node. Each
 * "start" is a posix_retained_open() and each "reset" a posix_retained_note_reset() followed by
 * posix_retained_close(), which is what the process's exit does to the mapping. Between them
 * the test reads and writes the file itself, as the bench does, from its own reading of the
 * layout in plat_retained.h.
 *
 * What a real reset of the node does to the cell (plat_reset() from a task, and from the
 * hardware watchdog's thread) is tested by tests/platform/wdg_selftest.c, and a cell written by
 * Python from its own CRC by tests/system/test_retained.py.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc/crc.h"
#include "plat_retained.h"
#include "posix_retained.h"
#include "unity.h"

static char s_dir[256];
static char s_path[300];
static char s_error[512];

/* ------------------------------------------------------------------------------ the file */

static void write_bytes(const void *data, size_t length)
{
    FILE *file = fopen(s_path, "wb");

    if (file == NULL) {
        TEST_FAIL_MESSAGE("cannot create the cell's file");
    } else {
        const size_t written = fwrite(data, 1U, length, file);
        const int closed = fclose(file);

        TEST_ASSERT_EQUAL_size_t(length, written);
        TEST_ASSERT_EQUAL_INT(0, closed);
    }
}

static size_t file_size(void)
{
    struct stat info;

    TEST_ASSERT_EQUAL_INT(0, stat(s_path, &info));
    return (size_t)info.st_size;
}

static plat_retained_cell_t read_cell(void)
{
    plat_retained_cell_t cell = {0};
    FILE *file = fopen(s_path, "rb");

    if (file == NULL) {
        TEST_FAIL_MESSAGE("cannot open the cell's file");
    } else {
        const size_t read = fread(&cell, sizeof(cell), 1U, file);
        const int closed = fclose(file);

        TEST_ASSERT_EQUAL_size_t(1U, read);
        TEST_ASSERT_EQUAL_INT(0, closed);
    }

    return cell;
}

/* The CRC from its definition in plat_retained.h: CRC-32/ISO-HDLC over the 32 bytes before
 * it, here through the one-shot helper of lib/crc on a byte copy. */
static uint32_t expected_crc(const plat_retained_cell_t *cell)
{
    uint8_t bytes[32];

    (void)memcpy(bytes, cell, sizeof(bytes));
    return crc32_compute(bytes, sizeof(bytes));
}

/* A valid cell with the given contents. */
static plat_retained_cell_t make_cell(uint32_t reason, uint32_t resets)
{
    plat_retained_cell_t cell;

    (void)memset(&cell, 0, sizeof(cell));
    cell.magic = 0x524E4346U; /* the bytes 'F' 'C' 'N' 'R' */
    cell.version = 1U;
    cell.size = 36U;
    cell.reset_reason = reason;
    cell.resets = resets;
    cell.reserved[0] = 0x11111111U;
    cell.reserved[1] = 0x22222222U;
    cell.reserved[2] = 0x33333333U;
    cell.reserved[3] = 0x44444444U;
    cell.crc = expected_crc(&cell);
    return cell;
}

/* A cell as after power-on: header, reason POWER_ON (0), every other word 0, CRC right. */
static void assert_fresh_cell(void)
{
    const plat_retained_cell_t cell = read_cell();

    TEST_ASSERT_EQUAL_size_t(36U, file_size());
    TEST_ASSERT_EQUAL_HEX32(0x524E4346U, cell.magic);
    TEST_ASSERT_EQUAL_UINT16(1U, cell.version);
    TEST_ASSERT_EQUAL_UINT16(36U, cell.size);
    TEST_ASSERT_EQUAL_UINT32(0U, cell.reset_reason);
    TEST_ASSERT_EQUAL_UINT32(0U, cell.resets);

    for (uint32_t i = 0U; i < PLAT_RETAINED_RESERVED_WORDS; i++) {
        TEST_ASSERT_EQUAL_HEX32(0U, cell.reserved[i]);
    }

    TEST_ASSERT_EQUAL_HEX32(expected_crc(&cell), cell.crc);
}

/* One start of the node: open, and return what it found. */
static posix_retained_info_t start(void)
{
    posix_retained_info_t info;

    TEST_ASSERT_TRUE_MESSAGE(posix_retained_open(s_path, s_error, sizeof(s_error)), s_error);
    posix_retained_get_info(&info);
    TEST_ASSERT_EQUAL_INT((int)info.cause, (int)plat_reset_cause());
    return info;
}

/* One reset of the node: what plat_reset() does to the cell, then the end of the process. */
static void reset(plat_reset_reason_t reason)
{
    posix_retained_note_reset(reason);
    posix_retained_close();
}

#define ASSERT_START(cause_name, verdict_text, reset_count, info)                                  \
    do {                                                                                           \
        TEST_ASSERT_EQUAL_STRING((cause_name), posix_reset_reason_name((info).cause));             \
        TEST_ASSERT_EQUAL_STRING((verdict_text), (info).verdict);                                  \
        TEST_ASSERT_EQUAL_UINT32((reset_count), (info).resets);                                    \
        TEST_ASSERT_TRUE((info).open);                                                             \
    } while (0)

void setUp(void)
{
    (void)unlink(s_path);
}

void tearDown(void)
{
    posix_retained_close();
    (void)unlink(s_path);
}

/* ------------------------------------------------------------------------------ the tests */

static void test_without_a_file_every_start_is_a_power_on(void)
{
    posix_retained_info_t info;

    TEST_ASSERT_TRUE(posix_retained_open(NULL, s_error, sizeof(s_error)));
    posix_retained_get_info(&info);
    TEST_ASSERT_FALSE(info.open);
    TEST_ASSERT_EQUAL_STRING("no cell", info.verdict);
    TEST_ASSERT_EQUAL_INT((int)PLAT_RESET_POWER_ON, (int)plat_reset_cause());
    posix_retained_note_reset(PLAT_RESET_WATCHDOG); /* nothing to write: no effect */
    posix_retained_close();
    TEST_ASSERT_TRUE(posix_retained_open(NULL, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_INT((int)PLAT_RESET_POWER_ON, (int)plat_reset_cause());
}

static void test_a_missing_file_is_a_power_on_and_becomes_a_fresh_cell(void)
{
    const posix_retained_info_t info = start();

    ASSERT_START("POWER_ON", "missing", 0U, info);
    assert_fresh_cell();
}

/* A cell that records a reset: its reason is the cause of this start, once. The count and the
 * reserved words are kept; the reason goes back to POWER_ON, so that a run ending without a
 * reset makes the next start a power-on. */
static void test_a_valid_cell_gives_its_reason_once_and_keeps_its_words(void)
{
    const plat_retained_cell_t before = make_cell((uint32_t)PLAT_RESET_WATCHDOG, 3U);
    posix_retained_info_t info;
    plat_retained_cell_t after;

    write_bytes(&before, sizeof(before));
    info = start();
    ASSERT_START("WATCHDOG", "reset", 3U, info);
    TEST_ASSERT_EQUAL_INT((int)PLAT_RESET_WATCHDOG, (int)plat_reset_cause());

    after = read_cell();
    TEST_ASSERT_EQUAL_UINT32(0U, after.reset_reason);
    TEST_ASSERT_EQUAL_UINT32(3U, after.resets);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.reserved, after.reserved, PLAT_RETAINED_RESERVED_WORDS);
    TEST_ASSERT_EQUAL_HEX32(expected_crc(&after), after.crc);

    /* An end without a reset (an orderly stop, a killed process) and the next start. */
    posix_retained_close();
    info = start();
    ASSERT_START("POWER_ON", "no reset", 0U, info);
    assert_fresh_cell();
}

/* Every way a cell can be wrong is a power-on, and the cell is written afresh. */
static void test_a_cell_that_is_not_valid_is_a_power_on(void)
{
    static const struct {
        const char *what;
        const char *verdict;
    } cases[] = {{"empty file", "size"},
                 {"short file", "size"},
                 {"long file", "size"},
                 {"magic", "magic"},
                 {"version", "version"},
                 {"size member", "version"},
                 {"a bit of a reserved word", "crc"},
                 {"the CRC", "crc"},
                 {"reason POWER_LOSS", "reason"},
                 {"reason 99", "reason"},
                 {"reason POWER_ON", "no reset"}};
    uint8_t bytes[64];

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        plat_retained_cell_t cell = make_cell((uint32_t)PLAT_RESET_ASSERT, 5U);
        size_t length = sizeof(cell);

        switch (i) {
        case 0U:
            length = 0U;
            break;
        case 1U:
            length = sizeof(cell) - 1U;
            break;
        case 2U:
            length = sizeof(cell) + 1U;
            break;
        case 3U:
            cell.magic ^= 1U;
            cell.crc = expected_crc(&cell);
            break;
        case 4U:
            cell.version = 2U;
            cell.crc = expected_crc(&cell);
            break;
        case 5U:
            cell.size = 40U;
            cell.crc = expected_crc(&cell);
            break;
        case 6U:
            cell.reserved[2] ^= 0x00010000U;
            break;
        case 7U:
            cell.crc ^= 0x80000000U;
            break;
        case 8U:
            cell.reset_reason = (uint32_t)PLAT_RESET_POWER_LOSS;
            cell.crc = expected_crc(&cell);
            break;
        case 9U:
            cell.reset_reason = 99U;
            cell.crc = expected_crc(&cell);
            break;
        default:
            cell.reset_reason = (uint32_t)PLAT_RESET_POWER_ON;
            cell.crc = expected_crc(&cell);
            break;
        }

        (void)memset(bytes, 0xEE, sizeof(bytes));
        (void)memcpy(bytes, &cell, sizeof(cell));
        write_bytes(bytes, length);

        const posix_retained_info_t info = start();

        TEST_ASSERT_EQUAL_STRING_MESSAGE("POWER_ON", posix_reset_reason_name(info.cause),
                                         cases[i].what);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].verdict, info.verdict, cases[i].what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, info.resets, cases[i].what);
        assert_fresh_cell();
        posix_retained_close();
    }
}

/* What plat_reset() writes: the reason, one more reset, a CRC over both; the next start
 * reports the reason. Resets accumulate until a run ends without one. */
static void test_resets_are_recorded_and_counted_until_a_run_ends_without_one(void)
{
    static const plat_reset_reason_t reasons[] = {
        PLAT_RESET_REQUESTED, PLAT_RESET_WATCHDOG,       PLAT_RESET_IMAGE_FAULT,
        PLAT_RESET_ASSERT,    PLAT_RESET_STACK_OVERFLOW, PLAT_RESET_MALLOC_FAILED};
    posix_retained_info_t info = start();

    ASSERT_START("POWER_ON", "missing", 0U, info);

    for (uint32_t i = 0U; i < (sizeof(reasons) / sizeof(reasons[0])); i++) {
        plat_retained_cell_t cell;

        reset(reasons[i]);
        cell = read_cell();
        TEST_ASSERT_EQUAL_UINT32((uint32_t)reasons[i], cell.reset_reason);
        TEST_ASSERT_EQUAL_UINT32(i + 1U, cell.resets);
        TEST_ASSERT_EQUAL_HEX32(expected_crc(&cell), cell.crc);

        info = start();
        TEST_ASSERT_EQUAL_INT((int)reasons[i], (int)info.cause);
        TEST_ASSERT_EQUAL_STRING("reset", info.verdict);
        TEST_ASSERT_EQUAL_UINT32(i + 1U, info.resets);
    }

    posix_retained_close();
    info = start();
    ASSERT_START("POWER_ON", "no reset", 0U, info);
}

/* plat_reset(PLAT_RESET_POWER_LOSS) takes the cell with it: the next start is a power-on. */
static void test_a_power_loss_destroys_the_cell(void)
{
    posix_retained_info_t info;
    plat_retained_cell_t cell;
    const plat_retained_cell_t zero = {0};

    (void)start();
    reset(PLAT_RESET_REQUESTED);
    info = start();
    ASSERT_START("REQUESTED", "reset", 1U, info);
    reset(PLAT_RESET_POWER_LOSS);
    cell = read_cell();
    TEST_ASSERT_EQUAL_MEMORY(&zero, &cell, sizeof(cell));
    info = start();
    ASSERT_START("POWER_ON", "magic", 0U, info);
}

/* The bench's way to cut the power: remove the file. */
static void test_a_removed_file_is_a_power_on(void)
{
    posix_retained_info_t info;

    (void)start();
    reset(PLAT_RESET_ASSERT);
    TEST_ASSERT_EQUAL_INT(0, unlink(s_path));
    info = start();
    ASSERT_START("POWER_ON", "missing", 0U, info);
}

/* One board, one cell: while a node has the file open, nobody else can lock it, and a second
 * open in the same process is refused. */
static void test_the_cell_is_locked_while_it_is_open(void)
{
    int fd;

    (void)start();
    fd = open(s_path, O_RDWR | O_CLOEXEC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(-1, flock(fd, LOCK_EX | LOCK_NB));
    TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);
    TEST_ASSERT_FALSE(posix_retained_open(s_path, s_error, sizeof(s_error)));
    TEST_ASSERT_EQUAL_STRING("a retained cell is already open", s_error);
    posix_retained_close();
    TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_EX | LOCK_NB));

    /* Now the test holds the lock: the node cannot start on this file. */
    TEST_ASSERT_FALSE(posix_retained_open(s_path, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL(strstr(s_error, "is in use by another node"));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}

static void test_a_file_that_cannot_be_used_is_an_error_with_a_reason(void)
{
    char path[400];

    (void)snprintf(path, sizeof(path), "%s/no/such/dir/cell", s_dir);
    TEST_ASSERT_FALSE(posix_retained_open(path, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "cannot open --retained-file"), s_error);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, "No such file or directory"), s_error);

    TEST_ASSERT_FALSE(posix_retained_open(s_dir, s_error, sizeof(s_error)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_error, s_dir), s_error);
}

static void test_the_reasons_and_their_names(void)
{
    static const struct {
        const char *name;
        plat_reset_reason_t reason;
        bool valid;
        bool recorded;
    } cases[] = {{"POWER_ON", PLAT_RESET_POWER_ON, false, false},
                 {"REQUESTED", PLAT_RESET_REQUESTED, true, true},
                 {"WATCHDOG", PLAT_RESET_WATCHDOG, true, true},
                 {"POWER_LOSS", PLAT_RESET_POWER_LOSS, true, false},
                 {"IMAGE_FAULT", PLAT_RESET_IMAGE_FAULT, true, true},
                 {"ASSERT", PLAT_RESET_ASSERT, true, true},
                 {"STACK_OVERFLOW", PLAT_RESET_STACK_OVERFLOW, true, true},
                 {"MALLOC_FAILED", PLAT_RESET_MALLOC_FAILED, true, true},
                 /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): on purpose */
                 {"unknown", (plat_reset_reason_t)14, false, false},
                 /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): on purpose */
                 {"unknown", (plat_reset_reason_t)63, false, false}};

    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        TEST_ASSERT_EQUAL_STRING(cases[i].name, posix_reset_reason_name(cases[i].reason));
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].valid, posix_reset_reason_valid(cases[i].reason),
                                  cases[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].recorded,
                                  posix_retained_reason_recorded((uint32_t)cases[i].reason),
                                  cases[i].name);
    }

    /* The values the DBC's ResetCause carries (dbc/fcn.dbc), and POWER_ON outside them. */
    TEST_ASSERT_EQUAL_INT(0, (int)PLAT_RESET_POWER_ON);
    TEST_ASSERT_EQUAL_INT(10, (int)PLAT_RESET_REQUESTED);
    TEST_ASSERT_EQUAL_INT(11, (int)PLAT_RESET_WATCHDOG);
    TEST_ASSERT_EQUAL_INT(22, (int)PLAT_RESET_MALLOC_FAILED);
}

/* The CRC of the layout's definition, and the check value of CRC-32/ISO-HDLC, so that the
 * Python bench's zlib.crc32 is the same function. */
static void test_the_crc_is_crc32_over_the_bytes_before_it(void)
{
    const plat_retained_cell_t cell = make_cell((uint32_t)PLAT_RESET_ASSERT, 7U);
    static const uint8_t check[] = "123456789";

    TEST_ASSERT_EQUAL_HEX32(expected_crc(&cell), posix_retained_crc(&cell));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, crc32_compute(check, 9U));
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");

    (void)snprintf(s_dir, sizeof(s_dir), "%s/fcn-retained-XXXXXX",
                   ((tmp != NULL) && (tmp[0] != '\0')) ? tmp : "/tmp");

    if (mkdtemp(s_dir) == NULL) {
        (void)printf("cannot create a directory for the test: %s\n", strerror(errno));
        return 1;
    }

    (void)snprintf(s_path, sizeof(s_path), "%s/cell", s_dir);

    UNITY_BEGIN();
    RUN_TEST(test_without_a_file_every_start_is_a_power_on);
    RUN_TEST(test_a_missing_file_is_a_power_on_and_becomes_a_fresh_cell);
    RUN_TEST(test_a_valid_cell_gives_its_reason_once_and_keeps_its_words);
    RUN_TEST(test_a_cell_that_is_not_valid_is_a_power_on);
    RUN_TEST(test_resets_are_recorded_and_counted_until_a_run_ends_without_one);
    RUN_TEST(test_a_power_loss_destroys_the_cell);
    RUN_TEST(test_a_removed_file_is_a_power_on);
    RUN_TEST(test_the_cell_is_locked_while_it_is_open);
    RUN_TEST(test_a_file_that_cannot_be_used_is_an_error_with_a_reason);
    RUN_TEST(test_the_reasons_and_their_names);
    RUN_TEST(test_the_crc_is_crc32_over_the_bytes_before_it);

    const int status = UNITY_END();

    (void)unlink(s_path);
    (void)rmdir(s_dir);
    return status;
}
