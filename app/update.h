/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The node's side of an update in phase 4: what the bootloader's hand-off says about the image
 * that runs (plat_retained.h, THE HAND-OFF), and the confirm of a trial image, which decides
 * when the trial has proved itself and asks for the CONFIRM record (lib/boot, boot_confirm()).
 *
 * THE HAND-OFF. Read once at the start (plat_boot_handoff()). No hand-off means that the node
 * was started without the bootloader: ActiveSlot SNA in BMS_NodeInfo, BootMode NONE in
 * BMS_BootInfo, nothing to confirm. A hand-off names the slot, whether the image runs as the
 * CONFIRMED image or as a TRIAL (and which of its three attempts), its version, and the state
 * change the bootloader made before the start; a rollback among those is the DTC
 * UPDATE_ROLLBACK (fault.h, update_rolled_back()).
 *
 * THE CONFIRM OF A TRIAL. A trial confirms itself once it has run UPDATE_CONFIRM_AFTER_S
 * seconds in good health without a break: the watchdog manager's global status OK and the CAN
 * controller not bus off in every health period of the window, and no bus-off between two of
 * them. The health task judges each period (update_health_period()); a period that is not
 * healthy starts the window again, counted in `window_restarts`. When the window is full the
 * monitor asks for the confirm once (it returns true), and the flash task makes it
 * (boot_confirm() of the slot the hand-off names) and reports how it went
 * (update_confirm_done()). A confirm that fails is not tried again in the run: the trial is then
 * rolled back after its attempts, which is safe, while writing again and again into a flash
 * that refused a write is not.
 *
 * WHY 5 SECONDS (UPDATE_CONFIRM_AFTER_S). Long enough for every check of the node to have
 * judged the new image at least once with a margin: the watchdog manager finds a task that
 * stops within 300 to 400 ms and the hardware watchdog resets the node 500 ms after its last
 * kick (app.c, SUPERVISION), so an image that hangs in its first seconds is reset well inside
 * the window, as is one that crashes; 50 supervision cycles and 5 BMS_NodeInfo frames have gone
 * by; the node has sent about 1600 frames (the cycle times of the DBC), and a bus fault that
 * makes its transmissions fail takes the controller bus off after 32 failed ones in a row
 * (transmit error counter +8 each, bus off above 255). Short enough for the system tests, which
 * run several updates. A product would wait longer (minutes, a drive cycle) or
 * leave the confirm to a tester's routine after its own checks of the new image; phase 5's UDS
 * server can add the routine.
 *
 * CONTEXT. No operating system and no locks: an update_monitor_t belongs to the health task
 * (the node: app.c), which alone calls update_health_period() and update_confirm_done(); the
 * result it passes to the latter comes from the flash task through the health task's own
 * copy. update_init() runs before the scheduler starts.
 */
#ifndef FCN_UPDATE_H
#define FCN_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/boot.h"
#include "can/can_fc.h"
#include "plat_flash.h"
#include "plat_retained.h"
#include "wdgm/wdgm.h"

/** How long a trial must run in good health before it confirms itself, in seconds. */
#define UPDATE_CONFIRM_AFTER_S 5U

/** Where the confirm stands: the values of BMS_BootInfo's ConfirmState. */
typedef enum {
    UPDATE_CONFIRM_NOT_NEEDED = 0, /**< no trial: no hand-off, or the CONFIRMED image runs */
    UPDATE_CONFIRM_WAITING = 1,    /**< a trial counting its healthy time */
    UPDATE_CONFIRM_CONFIRMING = 2, /**< the confirm was asked for; the flash task writes it */
    UPDATE_CONFIRM_CONFIRMED = 3,  /**< the trial confirmed itself: it is the active image */
    UPDATE_CONFIRM_FAILED = 4      /**< the confirm could not be written */
} update_confirm_state_t;

/** What the health task reads in each period for the confirm. */
typedef struct {
    wdgm_global_status_t supervision; /**< the watchdog manager's global status now */
    can_fc_state_t can_state;         /**< the CAN controller's error state now */
    uint32_t bus_off_count;           /**< bus-off events since the start */
} update_health_t;

/** The monitor. Set up with update_init(); read the members for diagnosis, write none. */
typedef struct {
    bool have_handoff;            /**< the bootloader started the node */
    plat_boot_handoff_t handoff;  /**< valid if have_handoff */
    update_confirm_state_t state; /**< where the confirm stands */
    uint32_t periods_needed;      /**< healthy periods that make the window */
    uint32_t healthy_periods;     /**< healthy periods in a row so far (stops at periods_needed) */
    uint32_t window_restarts;     /**< periods that were not healthy after a healthy one */
    uint32_t periods;             /**< health periods judged */
    uint32_t requested_period;    /**< the period the confirm was asked for in; 0 before */
    uint32_t bus_off_count;       /**< the count at the last period */
    boot_status_t confirm_status; /**< what boot_confirm() returned; BOOT_OK before */
} update_monitor_t;

/**
 * Set up `m` for this start: `handoff` is the bootloader's hand-off, or NULL for a start without
 * one. `periods_per_second` health periods make a second (at least 1). A TRIAL hand-off starts
 * the window (UPDATE_CONFIRM_WAITING); anything else needs no confirm. Returns false, setting up
 * nothing, for a NULL `m`. Before the scheduler starts.
 */
bool update_init(update_monitor_t *m, const plat_boot_handoff_t *handoff,
                 uint32_t periods_per_second);

/**
 * One health period of the confirm: judge `health` (see THE CONFIRM OF A TRIAL). Returns true
 * exactly once, in the period that completes the window: then the confirm is to be asked for,
 * and the state is UPDATE_CONFIRM_CONFIRMING. False otherwise, and in every state but WAITING,
 * whatever `health` says. The owner, once per health period.
 */
bool update_health_period(update_monitor_t *m, const update_health_t *health);

/** The flash task's result of the confirm asked for: CONFIRMED for BOOT_OK, FAILED for any
 *  other status. Ignored unless the state is CONFIRMING. The owner. */
void update_confirm_done(update_monitor_t *m, boot_status_t status);

/** The whole seconds of good health counted towards the confirm (0 without a trial; all of
 *  them, UPDATE_CONFIRM_AFTER_S, once the confirm was asked for). Any context, a copy of `m`. */
uint32_t update_healthy_seconds(const update_monitor_t *m, uint32_t periods_per_second);

/** Whether the hand-off reports a rollback (the bootloader's ROLLBACK, REJECT or FALLBACK):
 *  the evidence of UPDATE_ROLLBACK (fault_init()). False for a NULL `handoff`. Any context. */
bool update_rolled_back(const plat_boot_handoff_t *handoff);

/** The name of a confirm state as in the DBC ("WAITING"), or "unknown". Any context. */
const char *update_confirm_state_name(update_confirm_state_t state);

/** The name of a hand-off's event as in the DBC ("ROLLBACK"), or "unknown". Any context. */
const char *update_event_name(plat_boot_event_t event);

/**
 * lib/boot's configuration for the node: `dev` (plat_flash_device()), the metadata area and the
 * slots of `layout` (plat_flash_layout()), and no validator: the node of phase 4 only confirms
 * (boot.h). Fills `out` completely. Returns false, filling nothing, for a NULL pointer. Any
 * context.
 */
bool update_boot_config(const plat_flash_layout_t *layout, const flash_dev_t *dev,
                        boot_config_t *out);

#endif /* FCN_UPDATE_H */
