/* SPDX-License-Identifier: MIT */
/*
 * app/update.c: what the hand-off means for the node, and the confirm of a trial (update.h).
 * Every test drives the monitor as the health task does, one period at a time, with 10 periods
 * a second (APP_HEALTH_PERIOD_MS 100), so that the window is UPDATE_CONFIRM_AFTER_S * 10
 * periods.
 */
#include <string.h>

#include "unity.h"
#include "update.h"

#define PER_SECOND 10U
#define WINDOW     (UPDATE_CONFIRM_AFTER_S * PER_SECOND)

static update_monitor_t s_monitor;

void setUp(void)
{
    (void)memset(&s_monitor, 0xA5, sizeof(s_monitor));
}

void tearDown(void)
{
}

static plat_boot_handoff_t handoff(plat_boot_mode_t mode, uint8_t attempt, plat_boot_event_t event)
{
    plat_boot_handoff_t h;

    (void)memset(&h, 0, sizeof(h));
    h.slot = 1U;
    h.mode = mode;
    h.attempt = attempt;
    h.image_version = 0x01020003U;
    h.event = event;
    h.event_slot = (event == PLAT_BOOT_EVENT_NONE) ? PLAT_HANDOFF_NO_SLOT : 0U;
    return h;
}

static update_health_t healthy(void)
{
    const update_health_t h = {.supervision = WDGM_GLOBAL_STATUS_OK,
                               .can_state = CAN_FC_ERROR_ACTIVE,
                               .bus_off_count = 0U};

    return h;
}

/* `count` periods of `health`; returns how many of them asked for the confirm. */
static uint32_t run(const update_health_t *health, uint32_t count)
{
    uint32_t requests = 0U;

    for (uint32_t i = 0U; i < count; i++) {
        if (update_health_period(&s_monitor, health)) {
            requests++;
        }
    }

    return requests;
}

/* No hand-off, or the CONFIRMED image: nothing to confirm, ever, however healthy the node. */
static void test_without_a_trial_nothing_is_confirmed(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t confirmed = handoff(PLAT_BOOT_CONFIRMED, 0U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, NULL, PER_SECOND));
    TEST_ASSERT_FALSE(s_monitor.have_handoff);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 10U * WINDOW));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(0U, update_healthy_seconds(&s_monitor, PER_SECOND));

    TEST_ASSERT_TRUE(update_init(&s_monitor, &confirmed, PER_SECOND));
    TEST_ASSERT_TRUE(s_monitor.have_handoff);
    TEST_ASSERT_EQUAL_HEX32(0x01020003U, s_monitor.handoff.image_version);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 10U * WINDOW));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_NOT_NEEDED, s_monitor.state);

    TEST_ASSERT_FALSE(update_init(NULL, &confirmed, PER_SECOND));
}

/* A trial that stays healthy asks for its confirm exactly once, in the period that completes
 * the window, and not one period earlier. */
static void test_a_healthy_trial_asks_once_when_the_window_is_full(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.periods_needed);

    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, WINDOW - 1U));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S - 1U,
                             update_healthy_seconds(&s_monitor, PER_SECOND));

    TEST_ASSERT_TRUE(update_health_period(&s_monitor, &good));
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.requested_period);
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S,
                             update_healthy_seconds(&s_monitor, PER_SECOND));

    /* Never again, whatever follows. */
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 10U * WINDOW));
    TEST_ASSERT_EQUAL_UINT32(WINDOW, s_monitor.periods);
}

/* Each kind of bad period starts the window again: the supervision FAILED, EXPIRED, STOPPED
 * or DEACTIVATED, the controller bus off now, or a bus-off since the last period (the count
 * went up although the controller has recovered). */
static void test_a_period_in_bad_health_starts_the_window_again(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 2U, PLAT_BOOT_EVENT_NONE);
    update_health_t bad[6];

    for (size_t i = 0U; i < 6U; i++) {
        bad[i] = good;
    }

    bad[0].supervision = WDGM_GLOBAL_STATUS_FAILED;
    bad[1].supervision = WDGM_GLOBAL_STATUS_EXPIRED;
    bad[2].supervision = WDGM_GLOBAL_STATUS_STOPPED;
    bad[3].supervision = WDGM_GLOBAL_STATUS_DEACTIVATED;
    bad[4].can_state = CAN_FC_BUS_OFF;
    bad[5].bus_off_count = 1U;

    for (size_t i = 0U; i < 6U; i++) {
        TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND));
        TEST_ASSERT_EQUAL_UINT32(0U, run(&good, WINDOW - 1U));
        TEST_ASSERT_FALSE_MESSAGE(update_health_period(&s_monitor, &bad[i]), "a bad period");
        TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.healthy_periods);
        TEST_ASSERT_EQUAL_UINT32(1U, s_monitor.window_restarts);

        /* The count of bus-offs stays where it went: from here on the periods are good. */
        update_health_t after = good;

        after.bus_off_count = bad[i].bus_off_count;
        TEST_ASSERT_EQUAL_UINT32(0U, run(&after, WINDOW - 1U));
        TEST_ASSERT_TRUE(update_health_period(&s_monitor, &after));
        TEST_ASSERT_EQUAL_UINT32(2U * WINDOW, s_monitor.requested_period);
    }
}

/* Error passive is not bus off: the controller still sends, and the window goes on. */
static void test_error_passive_does_not_stop_the_window(void)
{
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);
    update_health_t passive = healthy();

    passive.can_state = CAN_FC_ERROR_PASSIVE;
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&passive, WINDOW));
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.window_restarts);
}

/* Bad periods before the first good one restart nothing (there was no window yet); each run of
 * good periods that a bad one ends counts once. */
static void test_restarts_count_the_windows_that_were_cut_short(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 3U, PLAT_BOOT_EVENT_NONE);
    update_health_t failed = good;

    failed.supervision = WDGM_GLOBAL_STATUS_FAILED;
    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&failed, 3U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_monitor.window_restarts);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 7U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&failed, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, run(&failed, 1U));
    TEST_ASSERT_EQUAL_UINT32(2U, s_monitor.window_restarts);
    TEST_ASSERT_EQUAL_UINT32(14U, s_monitor.periods);
}

/* The flash task's answer: CONFIRMED for BOOT_OK, FAILED for anything else; an answer while no
 * confirm is asked for changes nothing. */
static void test_the_result_of_the_confirm_decides_its_state(void)
{
    static const boot_status_t failures[] = {BOOT_E_FLASH, BOOT_E_STATE, BOOT_E_ARG,
                                             BOOT_E_POWER_LOST};
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND));
    update_confirm_done(&s_monitor, BOOT_OK); /* nothing asked yet */
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_WAITING, s_monitor.state);
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
    update_confirm_done(&s_monitor, BOOT_OK);
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);
    TEST_ASSERT_EQUAL(BOOT_OK, s_monitor.confirm_status);
    update_confirm_done(&s_monitor, BOOT_E_FLASH); /* a late second answer */
    TEST_ASSERT_EQUAL(UPDATE_CONFIRM_CONFIRMED, s_monitor.state);

    for (size_t i = 0U; i < (sizeof(failures) / sizeof(failures[0])); i++) {
        TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, PER_SECOND));
        TEST_ASSERT_EQUAL_UINT32(1U, run(&good, WINDOW));
        update_confirm_done(&s_monitor, failures[i]);
        TEST_ASSERT_EQUAL(UPDATE_CONFIRM_FAILED, s_monitor.state);
        TEST_ASSERT_EQUAL(failures[i], s_monitor.confirm_status);
        /* A failed confirm is not asked for again. */
        TEST_ASSERT_EQUAL_UINT32(0U, run(&good, 2U * WINDOW));
    }
}

/* ROLLBACK, REJECT and FALLBACK are rollbacks; NONE and ADOPT are not; no hand-off is none. */
static void test_which_events_are_a_rollback(void)
{
    const plat_boot_event_t yes[] = {PLAT_BOOT_EVENT_ROLLBACK, PLAT_BOOT_EVENT_REJECT,
                                     PLAT_BOOT_EVENT_FALLBACK};
    const plat_boot_event_t no[] = {PLAT_BOOT_EVENT_NONE, PLAT_BOOT_EVENT_ADOPT};

    for (size_t i = 0U; i < 3U; i++) {
        const plat_boot_handoff_t h = handoff(PLAT_BOOT_CONFIRMED, 0U, yes[i]);

        TEST_ASSERT_TRUE(update_rolled_back(&h));
    }

    for (size_t i = 0U; i < 2U; i++) {
        const plat_boot_handoff_t h = handoff(PLAT_BOOT_CONFIRMED, 0U, no[i]);

        TEST_ASSERT_FALSE(update_rolled_back(&h));
    }

    TEST_ASSERT_FALSE(update_rolled_back(NULL));
}

static void test_the_names_are_those_of_the_dbc(void)
{
    TEST_ASSERT_EQUAL_STRING("NOT_NEEDED", update_confirm_state_name(UPDATE_CONFIRM_NOT_NEEDED));
    TEST_ASSERT_EQUAL_STRING("WAITING", update_confirm_state_name(UPDATE_CONFIRM_WAITING));
    TEST_ASSERT_EQUAL_STRING("CONFIRMING", update_confirm_state_name(UPDATE_CONFIRM_CONFIRMING));
    TEST_ASSERT_EQUAL_STRING("CONFIRMED", update_confirm_state_name(UPDATE_CONFIRM_CONFIRMED));
    TEST_ASSERT_EQUAL_STRING("FAILED", update_confirm_state_name(UPDATE_CONFIRM_FAILED));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no state, on purpose */
    TEST_ASSERT_EQUAL_STRING("unknown", update_confirm_state_name((update_confirm_state_t)5));
    TEST_ASSERT_EQUAL_STRING("NONE", update_event_name(PLAT_BOOT_EVENT_NONE));
    TEST_ASSERT_EQUAL_STRING("ADOPT", update_event_name(PLAT_BOOT_EVENT_ADOPT));
    TEST_ASSERT_EQUAL_STRING("ROLLBACK", update_event_name(PLAT_BOOT_EVENT_ROLLBACK));
    TEST_ASSERT_EQUAL_STRING("REJECT", update_event_name(PLAT_BOOT_EVENT_REJECT));
    TEST_ASSERT_EQUAL_STRING("FALLBACK", update_event_name(PLAT_BOOT_EVENT_FALLBACK));
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): no event, on purpose */
    TEST_ASSERT_EQUAL_STRING("unknown", update_event_name((plat_boot_event_t)2));
}

/* The window counts periods: with 1 period a second, or 0 taken as 1, it is UPDATE_CONFIRM_AFTER_S
 * periods. */
static void test_the_window_follows_the_periods_per_second(void)
{
    const update_health_t good = healthy();
    const plat_boot_handoff_t trial = handoff(PLAT_BOOT_TRIAL, 1U, PLAT_BOOT_EVENT_NONE);

    TEST_ASSERT_TRUE(update_init(&s_monitor, &trial, 0U));
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S, s_monitor.periods_needed);
    TEST_ASSERT_EQUAL_UINT32(0U, run(&good, UPDATE_CONFIRM_AFTER_S - 1U));
    TEST_ASSERT_EQUAL_UINT32(UPDATE_CONFIRM_AFTER_S - 1U, update_healthy_seconds(&s_monitor, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, run(&good, 1U));
}

/* lib/boot's configuration is the layout's areas, the device given and no validator. */
static void test_the_boot_configuration_is_the_platform_layout(void)
{
    const plat_flash_layout_t layout = {
        .geometry = {.size = 0x202000U, .sector_size = 4096U, .page_size = 256U, .unit_size = 8U},
        .meta = {.offset = 0U, .size = 0x2000U},
        .slot = {{.offset = 0x2000U, .size = 0x100000U}, {.offset = 0x102000U, .size = 0x100000U}}};
    const flash_dev_t dev = {.geometry = layout.geometry};
    boot_config_t config;

    (void)memset(&config, 0x5A, sizeof(config));
    TEST_ASSERT_TRUE(update_boot_config(&layout, &dev, &config));
    TEST_ASSERT_EQUAL_PTR(&dev, config.dev);
    TEST_ASSERT_EQUAL_UINT32(0U, config.meta_addr);
    TEST_ASSERT_EQUAL_UINT32(2U, config.meta_sectors);
    TEST_ASSERT_EQUAL_HEX32(0x2000U, config.slot[0].addr);
    TEST_ASSERT_EQUAL_HEX32(0x100000U, config.slot[0].size);
    TEST_ASSERT_EQUAL_HEX32(0x102000U, config.slot[1].addr);
    TEST_ASSERT_EQUAL_HEX32(0x100000U, config.slot[1].size);
    TEST_ASSERT_NULL(config.validator.start);
    TEST_ASSERT_NULL(config.validator.step);
    TEST_ASSERT_NULL(config.validator.ctx);

    TEST_ASSERT_FALSE(update_boot_config(&layout, NULL, &config));
    TEST_ASSERT_FALSE(update_boot_config(NULL, &dev, &config));
    TEST_ASSERT_FALSE(update_boot_config(&layout, &dev, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_without_a_trial_nothing_is_confirmed);
    RUN_TEST(test_a_healthy_trial_asks_once_when_the_window_is_full);
    RUN_TEST(test_a_period_in_bad_health_starts_the_window_again);
    RUN_TEST(test_error_passive_does_not_stop_the_window);
    RUN_TEST(test_restarts_count_the_windows_that_were_cut_short);
    RUN_TEST(test_the_result_of_the_confirm_decides_its_state);
    RUN_TEST(test_which_events_are_a_rollback);
    RUN_TEST(test_the_names_are_those_of_the_dbc);
    RUN_TEST(test_the_window_follows_the_periods_per_second);
    RUN_TEST(test_the_boot_configuration_is_the_platform_layout);
    return UNITY_END();
}
