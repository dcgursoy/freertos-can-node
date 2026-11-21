# The application

What the node's firmware does: its tasks and their priorities and periods, how they talk to each
other and why each mechanism was chosen, the path of a measurement from the front end to a frame
on the bus, and the analysis that says whether the tasks meet their deadlines. Phase 3 added the
fault monitor and the supervision of the tasks to the processing and the health task
([app/fault.h](../app/fault.h), SUPERVISION in [app/app.c](../app/app.c)). Phase 4 added the
node's side of an update: the bootloader's hand-off read at start, the confirm of a trial image,
and the flash task that makes every write of the node to the flash ([app/update.h](../app/update.h),
THE UPDATE in [app/app.c](../app/app.c)).
The messages themselves are described in [can-protocol.md](can-protocol.md), the simulated bus in
[can-simulation.md](can-simulation.md) and the sensor model in [sensor-model.md](sensor-model.md).

```
  front end ──> sensor ──(sample queue)──> process ──> protected telemetry ─────────────┐
  (plat_sensor.h)  │                         │  │                                        │
                   └──> BMS_Heartbeat ───────┼──┼────────────────────────────────────────┤
                                             │  └──(verdict queue)──> health ──> BMS_Fault, task stats,
                                             v                          │  │     node info ──┤
                                 pack state (mutex) <───────────────────┘  │                 v
                                 DTC summary ──> process (PackFaultActive) │       can_io (can_tx) ──> bus
  bus ──> can_io (can_rx) ──> TESTER_Command check (command.c) ──> telemetry switch ──> process
                                                              └──> clear requests ──> health

  sensor, process, can_rx, can_tx ──(alive indications)──> watchdog manager (lib/wdgm), run by health
  health ──(kick, while the manager allows it)──> hardware watchdog (plat_wdg.h) ──> reset if not kicked

  bootloader's hand-off (plat_retained.h) ──> app_start: ActiveSlot, BMS_BootInfo, UPDATE_ROLLBACK
  health ──(confirm request, once a trial ran 5 s in good health)──> flash ──> lib/boot ──> flash (plat_flash.h)
         <──(the confirm's result)──────────────────────────────────┘
```

The DTCs ([lib/dtc](../lib/dtc/dtc.h), the faults in [app/fault.c](../app/fault.c)) have one
owner, the health task; the processing task judges the samples and hands it the verdicts, and
every other task reads only the summary the health task publishes. The supervision cycle of the
watchdog manager and the kick of the hardware watchdog are the first thing the health task does
in every period. [faults.md](faults.md) describes the fault handling in full: the faults and
their DTCs, the fault frame, the watchdogs and the reset cause.

## Tasks

Six tasks of the application, all created statically before the scheduler starts
([app/app.c](../app/app.c), [app/can_io.c](../app/can_io.c)); priorities and periods are defined in
one place, [app/app_tasks.h](../app/app_tasks.h) and [app/can_io.h](../app/can_io.h). A higher number
is more urgent.

| Task | Priority | Released by | Does |
|---|---|---|---|
| `can_rx` | 6 | the CAN interrupt (task notification); at the latest after 50 ms, and at the end of every 100 ms receive cycle | takes received frames out of the controller's FIFO (at most 32 per activation) and calls their handlers: a pong for every ping, the check of every TESTER_Command; once per receive cycle the TESTER_Command check without data |
| `sensor` | 5 | every 10 ms (`xTaskDelayUntil`) | publishes BMS_Heartbeat, reads the current in every period, the cells in every second and the temperatures in every tenth, posts the sample |
| `process` | 4 | a sample in the queue, or 30 ms without one | runs lib/bms on the sample under the pack mutex, builds, protects and publishes the protected messages; judges the sample (or the timeout) for the fault monitor and posts the verdict to `health` |
| `can_tx` | 3 | a producer or the controller (task notification), or the queue's next deadline, at the latest after 50 ms | loads the controller's three mailboxes from the transmit queue, handles transmit-done events, the bus-off policy |
| `health` | 2 | every 100 ms (`xTaskDelayUntil`) | the supervision cycle of the watchdog manager and the kick of the hardware watchdog; the DTCs (the verdicts, the CAN controller, the supervision, clears) and BMS_Fault in every period; whether a trial image runs in good health, and once it has for 5 s the request for its confirm; BMS_TaskStats every second period (one task per frame), BMS_NodeInfo and a log line every tenth, BMS_BootInfo in the periods 2, 12, 22, ...; ends a run of `--duration-s` |
| `flash` | 1 | the health task's request (task notification) | every write of the node to the flash: the confirm of a trial (`boot_confirm()` of lib/boot), and at start, when the bootloader started the node, a read of the metadata for the log and the statistics; phase 5's slot writer will run here too. Without a flash it waits and does nothing. Each flash operation blocks it for the time the part takes (a page program 0.4 ms, a sector erase 45 ms, [plat_flash.h](../platform/include/plat_flash.h)) |

Every task's stack is 512 words (`APP_TASK_STACK_WORDS`, `TASK_STACK_WORDS` in can_io.c), the
size meant for the target; on the host each task runs on a pthread stack of its own, which the
platform measures ([README](../README.md#what-phase-0-contains)).

**Why the flash task is the lowest.** A flash operation is synchronous and slow: the driver of a
serial NOR polls the part's busy flag, and the host's flash blocks the calling task one tick at a
time until the modelled time has passed. A task with a deadline, or one the watchdog manager
supervises, would spend that time waiting: a sector erase is 45 ms, almost half a health period,
and phase 5's erase of a slot is 256 of them. Below every other task the waits delay nobody; the
only cost to the others is its short critical sections (see the analysis below). It is not a
supervised entity of the watchdog manager: it waits for requests without a period, and in phase 4
it has one job per run at most. A hang of it shows as a confirm that never comes (BMS_BootInfo
ConfirmState CONFIRMING), after which the trial is rolled back by its attempts.

**Rate-monotonic order, with one exception.** A task with a shorter period has the higher
priority: `can_rx` (modelled per tick, T = 1 ms; frames can come every 47 µs) above `sensor` and
`process` (10 ms) above `health` (100 ms). `sensor` and `process` share a period; `sensor` is
above, because `process` waits for its samples. The exception is `can_tx`, which is released by
every frame it loads and so more often than the tasks above it. It runs below the tasks that
produce the frames on purpose: a producer is never preempted by its own output, and the frames a
period produces go out together. The design notes said the exception would be `can_rx`;
modelled by its demand per tick (T = 1000 µs, see below), `can_rx` has a period no longer than
that of any task below it and is in rate-monotonic order; the task that is not is `can_tx`. The
response-time analysis below holds for any fixed priorities and is the verdict.

## Communication between the tasks

| From | To | Mechanism | Why this one |
|---|---|---|---|
| CAN interrupt | `can_rx`, `can_tx` | task notification, one bit per cause (`xTaskNotifyFromISR`, `eSetBits`) | each task waits for "something for you" from one interrupt, with a few causes that may coincide. Bits in the notification value carry them without a kernel object per cause, and a bit set while the task is busy is still there when it next waits |
| any producer | `can_tx` | the transmit queue ([lib/can/can_txq.h](../lib/can/can_txq.h)) in short critical sections, then a notification bit | a cyclic message needs a slot that keeps only the newest value, not a FIFO: a busy bus must delay data, not queue stale values. The queue is plain C shared by several tasks, so it is guarded by `taskENTER_CRITICAL` for the few instructions an operation takes, cheaper than a mutex for that length |
| `sensor` | `process` | FreeRTOS queue of 4 samples (`app_sample_t`: lib/bms's `bms_sample_t` and the groups the front end did not answer, copied), statically allocated | a copy decouples the two in time and in memory: the sensor task can take the next sample while the last one is processed. The sensor task never waits for room (timeout 0): a full queue costs a sample, which is counted (`sample_queue.full`) and which lib/bms sees as a gap in the sequence numbers, but it never holds up the time base. The processing task waits with a timeout of 30 ms, three sensor periods: a timeout means the sensor task has stopped delivering, and is counted (`process.timeouts`) and judged as the fault SAMPLE_TIMEOUT. The processing task makes no log call; the health task logs the start and the end of such a stall from its counts (`process.timeouts`, `process.recoveries`) |
| `process` | `health` | the pack state (`bms_pack_t`) behind a mutex with priority inheritance | shared state that a reader must see whole, written by one task and read by another. The mutex is held only to run lib/bms on one sample or to copy the state, and frames are built from the copy outside the lock. While `health` (2) holds it, `process` (4) waiting for it lifts `health` to 4, so no task between them (`can_tx`) can prolong the wait |
| `can_rx` | `process` | the telemetry switch, one `bool` read and written in critical sections | one word, set from TESTER_Command and read once per sample: a critical section around one load or store is the cheapest correct way |
| `process` | `health` | a FreeRTOS queue of 32 verdicts on the samples (`fault_verdict_t`, 8 bytes: two masks and the count of clear requests the processing task read as it posted the verdict, [app/fault.h](../app/fault.h)), statically allocated; the processing task never waits for room, a lost verdict is counted (`process.verdicts_lost`) | the DTCs have one owner, the health task; the processing task judges each sample where the data is and hands over the verdict in its order, so that the debouncing counts samples, and the count tells the health task which verdicts came after a clear request. The owner sits below `can_tx`: the DTC work in the processing task would have added to `can_tx`'s response time, which the analysis below does not allow (see [the task table](#the-result-for-the-nodes-task-table)) |
| `health` | `process` | the summary of the DTCs (`fault_summary_t`), written after every health period and read once per sample in critical sections | PackFaultActive and PackState FAULT follow the faults active in the summary; a few words, like the telemetry switch |
| `can_rx` | `process`, `health` | the count of clear requests of TESTER_Command, written and read in critical sections | the processing task tags each verdict with it, and the health task carries out a clear once per new count (`command_rx_t.clear_requests`), before the first verdict that carries it |
| `sensor`, `process`, `can_rx`, `can_tx` | `health` | alive indications to the watchdog manager ([lib/wdgm](../lib/wdgm/wdgm.h)), one lock-free atomic addition each | a task that hangs stops counting; the health task examines the counts once per period and kicks the hardware watchdog only while the manager allows it |
| bootloader | every task | the hand-off (`plat_boot_handoff()`), read once by `app_start()` before the scheduler starts and constant after | written before the image runs, read-only while it runs: no lock |
| `health` | `flash` | a task notification (`xTaskNotifyGive`), once per run when the trial has run 5 s in good health | one request and no data: the slot to confirm is the hand-off's, which both tasks read |
| `flash` | `health` | the result of the confirm (`boot_status_t` and a flag), written and read in critical sections | two words; the health task takes it in its next period and updates the confirm's state (BMS_BootInfo) |
| every task | statistics | counters each written by one task only; the health task copies them at the end after raising itself to the highest priority | no task is in the middle of an update when they are read |

Every kernel object of the application is allocated statically (`xTaskCreateStatic`,
`xQueueCreateStatic`, `xSemaphoreCreateMutexStatic`); `lib/` and `app/` never allocate memory.

## From a sample to a frame

1. **Release.** `sensor` returns from `xTaskDelayUntil` at its period's tick (pdFALSE, a period
   that started late, is counted in `sensor.overruns`). After such an overrun it does not make
   up the periods that have passed since: it skips them (`sensor.skipped_periods`) and runs the
   period the tick is in, on the grid of its period.
2. **Heartbeat.** It publishes BMS_Heartbeat first: the number of the period (from 1; a count of
   the periods it ran, so that a skipped period leaves no gap) and the tick count, the status
   frame of phase 1 bit for bit, from transmit-queue slot 0.
3. **Acquisition.** It reads the front end ([plat_sensor.h](../platform/include/plat_sensor.h)):
   the current in every period, the cells in every second (sample numbers 0, 2, 4, ...), the
   temperatures in every tenth (0, 10, 20, ...). The sample is stamped with `plat_time_ns()` and
   numbered. A group the front end did not answer goes in with every channel flagged invalid,
   so that it becomes "not available" downstream instead of keeping its last values.
4. **Queue.** `xQueueSend(..., 0)`: the sample is copied into the queue, or counted as lost.
5. **Processing.** `process` takes it, and under the pack mutex runs `bms_process()`
   ([lib/bms](../lib/bms/bms.h)): every reading is checked against what its sensor can deliver
   (0 to 5000 mV, −40.0 to 125.0 °C, ±300 A) and against the front end's own invalid flag; a
   valid one goes through a first-order fixed-point filter per channel (shifts 2 for the
   current, 2 for the cells and 1 for the temperatures, `s_bms_config` in app.c; a time
   constant of −1 / ln(1 − 2^−shift) samples, 3.5, 3.5 and 1.4, which at 10, 20 and 100 ms per
   sample is about 35, 70 and 144 ms); an invalid one makes its channel invalid
   and resets its filter. Then the pack summary: the pack voltage as the sum of the filtered
   cells (valid only while all eight are), the lowest and highest cell, and the pack state
   (STARTUP until every channel has reported, FAULT while one is invalid, NORMAL otherwise).
6. **Conversion.** Outside the lock, from the copy: [app/signals.c](../app/signals.c) turns mA,
   mV and tenths of a degree into the raw values of the DBC. Divisions round half away from
   zero, as the filters do. A valid value beyond its signal saturates and is never sent as
   "signal not available": a cell below 1.000 V goes out as 1.000 V, as the DBC's comment says.
   SNA is sent only for an invalid channel; PackCurrent and PackVoltage, which have no SNA code,
   go out as 0 with their valid flag 0. One `_Static_assert` per factor, offset, SNA code and
   range ties the arithmetic to the generated header.
7. **Frame.** [app/telemetry.c](../app/telemetry.c) fills the generated codec's struct, packs it
   ([app/gen/can_db.c](../app/gen/can_db.c), generated from the DBC) and protects it with
   `e2e_p11_protect()`, one counter per protected message. BMS_PackStatus goes out with every
   sample, the cell messages with every sample that carries cells, BMS_Temperatures with every
   sample that carries temperatures, all of them only while the telemetry switch is on.
8. **Transmission.** `can_io_publish()` puts the frame into its message's slot of the transmit
   queue, with the sample's time stamp, and notifies `can_tx`, which loads it into a mailbox when
   one is free; the controller model sends it when it wins arbitration
   ([can-simulation.md](can-simulation.md)).

**Who sends what.** Each message has exactly one sending task, which alone writes its counters.

| Message | Sender | When | Switched off by CmdTelemetryOn = 0 |
|---|---|---|---|
| BMS_Fault 0x010 | health | every health period, 100 ms, after the period's last change of the DTCs: they change only in that job, so every change goes out at once, and a fault that came and went within the period is active in its frame. A fault of the pack values reaches its DTC in the first health period after the sample that decided it (at most 100 ms) | no |
| BMS_PackStatus 0x020 | process | every sample, 10 ms | yes |
| BMS_Heartbeat 0x030 | sensor | first thing in every period, 10 ms | no |
| BMS_CellVoltages1/2 0x040/0x041 | process | every sample with cells, 20 ms | yes |
| BMS_Temperatures 0x050 | process | every sample with temperatures, 100 ms | yes |
| BMS_TaskStats 0x600 | health | every 200 ms (the odd health periods), round robin over can_rx, sensor, process, can_tx, health, flash, IDLE | no |
| BMS_NodeInfo 0x610 | health | every second | no |
| BMS_BootInfo 0x611 | health | every second, 200 ms after BMS_NodeInfo | no |
| BMS_Pong 0x6F1 | can_rx | for every ping | no |

The heartbeat comes from the sensor task because its two numbers describe that task: the
number of the period it runs and the tick it was released in. The sensor task is the highest-priority
periodic task and is released by the tick alone, so the numbers keep the timing of phase 1's
status frame. The frame reaches the bus only when `can_tx` loads it, and `can_tx` (3) runs below
`process` (4): the heartbeat goes out after the processing job of its period, and a heartbeat on
the bus says that the sensor task and `can_tx` both run. A processing task that blocks shows on
the bus by itself: its protected messages stop while the heartbeat goes on, and their receivers'
end-to-end checks time out. A processing job that does not end keeps `can_tx` from running, as
any task at priority 3 or above that keeps the CPU does: the heartbeat then stops with every
other frame of the node, which a receiver cannot tell from a time base that stopped. The
watchdogs of phase 3 find both: the watchdog manager a task that stops reporting that it is
alive, the hardware watchdog a health task that no longer kicks it (SUPERVISION in
[app/app.c](../app/app.c)).

**The way back: TESTER_Command.** `can_rx` owns the command receiver
([app/command.c](../app/command.c)): it checks every TESTER_Command frame on arrival (AUTOSAR E2E
Profile 11 and the E2E state machine) and, at the end of every 100 ms receive cycle without a
frame, checks with "no new data". A command is carried out only while the state machine is VALID
and only for a new, intact frame; then CmdTelemetryOn becomes the telemetry switch, and a
CmdClearFaultsSeq that differs from that of the command carried out before it, in a stream that
stayed VALID between the two, is a request to clear the DTCs, which the health task carries out
in its next period, in order with the evidence ([faults.md](faults.md#the-fault-frame-bms_fault-0x010)). The parameters and what
follows from them are in
[can-protocol.md](can-protocol.md#the-nodes-receiver-of-tester_command). After a host stall the
receive cycle makes one check for all the periods it missed and counts the others
(`can.driver.rx_cycles_skipped`), so that the command stream does not go INVALID because the
node, rather than the tester, stood still.

## Statistics

`--stats-json` writes, besides what phase 1 wrote (unchanged):

- `task_table`: the task table of the schedulability check (below), schema `fcn.task_table/1`;
- `sensor`, `sample_queue`, `process` and `pack`: periods, reads and unanswered reads per group,
  samples posted and lost, samples processed, timeouts, backlog, and the final pack state with
  its counters (flagged and implausible readings, lost samples, sequence errors);
- `telemetry.messages`: per cyclic message what the application did with it (published,
  suppressed by the switch, failed) and its transmit-queue slot's counters, produced = sent +
  superseded + pending;
- `command`: the Profile 11 status counts, the checks, the state machine's state and counts, the
  commands carried out and ignored, the switch and the last CmdClearFaultsSeq;
- `overruns` per task, and `can.driver.rx_cycles` and `rx_cycles_skipped`;
- `reset`: the reset cause of this start from the retained-RAM cell (`--retained-file`,
  [plat_retained.h](../platform/include/plat_retained.h)): `cause` and `cause_code` (the value of
  `plat_reset_reason_t`), `retained_cell`, the `verdict` on the cell and `resets_since_power_on`;
- `hw_watchdog`: what the hardware watchdog model counted
  ([plat_wdg.h](../platform/include/plat_wdg.h)): `started`, `timeout_ms`, `kicks`,
  `longest_unkicked_ns`, and the host stalls it did not count (`gaps`, `longest_gap_ns`);
- `faults`: the DTCs as their owner, the health task, left them ([app/fault.h](../app/fault.h)):
  the summary the last BMS_Fault carried (`pack_fault_active`, `active`, `confirmed`,
  `most_recent`, `warning`), `clears`, `clear_requests_seen`, `verdicts_applied`,
  `health_cycles`, `status_changes`, the `reset_cause` the reset faults were judged from, and per
  fault in `dtcs` its `event` number, `name`, `dtc` number and its display `code` (`P1A01-00`), the
  `status` byte (ISO 14229-1), the debounce `counter`, the `fdc` and `failed_cycles`;
- `watchdog_manager`: the supervision ([lib/wdgm](../lib/wdgm/wdgm.h)): `global_status`,
  `cycles`, `first_expired`, `expired_cycles`, `expired_tolerance`, the health task's `kicks` and
  `kicks_withheld`, `hw_started`, `hw_timeout_ms`, and per entity its `status`, `examinations`,
  `incorrect`, `last_count`, the fewest and most alive indications in one cycle (`min_count`,
  `max_count`) and its configuration;
- in `process`, `verdicts_posted`, `verdicts_lost` and `recoveries` (samples that came after a
  timeout); in `sensor`, `skipped_periods` (the periods skipped after an overrun, which `periods`
  does not count); in `command`, `clear_requests`; in `can.driver`, `error_passive_entries`;
- `flash` (phase 4): what the host's flash counted ([posix_flash.h](../platform/posix/posix_flash.h)):
  `file`, `created`, the reads, programs, units programmed, erases and refused calls, `busy_ns`,
  the erases per area (`erases_meta`, `erases_slot_a`, `erases_slot_b`), `max_sector_erases`, and
  the cut (`cut`: none, armed or fired, `cut_in`, `cut_seed`);
- `handoff` (phase 4): the bootloader's hand-off as the node took it
  ([plat_retained.h](../platform/include/plat_retained.h)): `verdict` (taken, invalid, none),
  `present`, `slot`, `mode` (confirmed or trial), `attempt`, `image_version` and its
  `image_version_code`, `event` (NONE, ADOPT, ROLLBACK, REJECT, FALLBACK), `event_slot`,
  `select_write_error`, and `rolled_back` (the evidence of UPDATE_ROLLBACK);
- `confirm` (phase 4): the confirm of a trial ([app/update.h](../app/update.h)): `state`
  (NOT_NEEDED, WAITING, CONFIRMING, CONFIRMED, FAILED), `confirm_after_s`, `periods_needed`,
  `periods` judged, `healthy_periods`, `window_restarts`, `requested_period`, `status` (lib/boot's
  result), and the flash task's own counts in `flash_task`: `flash`, `boot_init`, `described`
  and what the metadata said at start (`have_record`, `seq`, `active_slot`, `trial_slot`,
  `attempts_used`, `records_valid`), `confirms`, `confirm_status` and `confirm_ns`, how long the
  confirm took with its waits for the flash.

## Schedulability

[tools/fcn_tools/sched.py](../tools/fcn_tools/sched.py) checks whether the node's tasks meet their
deadlines under fixed-priority preemptive scheduling, the way FreeRTOS schedules them:

```bash
python -m fcn_tools.sched <table.json> [--json report.json]
```

It prints a report. It exits with 0 when every task meets its deadline, 1 when one does not, and
2 when it refuses the input. `--json` also writes the report as JSON (schema
`fcn.sched_report/1`).

### The task table

The input (schema `fcn.task_table/1`) has one entry per task:

| Field | Meaning |
|---|---|
| `name` | unique |
| `priority` | the FreeRTOS priority: a higher number is more urgent |
| `kind` | `periodic`: a job arrives every `period_us`, the period of the task's `xTaskDelayUntil` loop. `sporadic`: jobs arrive at least `min_interarrival_us` apart |
| `deadline_us` | at most the period; counted from the arrival |
| `wcet_us` | the execution-time budget of one job, including its critical sections, kernel calls and context switches |
| `jitter_us` | release jitter: how long after its arrival a job can still be waiting to become ready |
| `nonpreemptive_us` | the longest stretch of a job that no other task can preempt (`taskENTER_CRITICAL`, `vTaskSuspendAll`); 0 if there is none |
| `critical_sections` | for each mutex the task takes, the longest time it holds it in one job |

All times are integers in microseconds, and every field is required. Unknown fields, numbers
that are not integers and duplicate keys are refused, so a misspelt field cannot fall back to a
default. The optional `wcet_basis` says whether the execution times are `design` budgets or
`measured`.

The node writes the table it runs with into its statistics (`task_table`). It comes from
[app/task_table.c](../app/task_table.c), whose priorities and periods are the constants the tasks
are created with, so it cannot drift from the code that creates them.
`tests/system/test_telemetry.py` runs `python -m fcn_tools.sched` on the table of a running
node and fails on any exit status other than 0; it also checks the table's priorities against
the tasks that ran and its periods against the cycle times of the DBC.

The execution-time budgets and critical-section lengths in the table are design figures for a
Cortex-M3 class target until phase 6 measures execution times; each is derived in a comment in
app/task_table.c. The table says so (`"wcet_basis": "design"`), and the report repeats it on its
second line. Until then, "schedulable" means: schedulable as long as no job runs longer than its
budget.

### How the node's tasks are modelled

The rules (Audsley, Burns, Richardson, Tindell and Wellings 1993, and those of the model below),
as app/task_table.c applies them:

- **Interrupts** are sporadic tasks above every task, with a priority of `configMAX_PRIORITIES`
  (8) or more: `can_irq` (9) at the shortest frame on the bus at the highest bit rate the node
  accepts (44 bits and the 3-bit intermission at 1 Mbit/s: 47 µs), so that every bit rate is
  covered, and the kernel's tick, `tick_irq` (8), periodic with T = 1000 µs
  (`configTICK_RATE_HZ` is 1000).
- **`can_rx` and `can_tx`** are released by events that can come far more often than once per
  tick, so each is modelled by its demand per tick: one job per tick (T = 1000 µs) that does what
  any 1000 µs can ask of the task. Every wake-up of either is one pass of its loop in
  [app/can_io.c](../app/can_io.c), so the budget counts passes as well as frames. At 1 Mbit/s at
  most 22 frames end on the bus in any 1000 µs, one per 47 µs.
  - `can_rx`: at most one pass per received frame and one for the receive cycle, 23 passes of
    3 µs; 22 frames of 4 µs; the receive cycle's 12 µs. 169 µs.
  - `can_tx`: at most one pass per frame that ends on the bus (the transmit-done of one of the
    node's frames, or the pong that a ping makes `can_rx` queue), one for `sensor` and `process`
    together (it runs only once both have blocked), two for `health` (which is below it, so each
    publish wakes it at once, and BMS_TaskStats and BMS_NodeInfo can fall into one period) and
    one timeout of its wait: 26 passes of 13 µs. A load and a transmit-done for each of the
    node's frames that ends, and a load for each of its 3 mailboxes: 47 events of 3 µs. 479 µs.

  A busy period of a lower task begins with no work of theirs left over, so all they do in it
  is caused within it, at most one budget per 1000 µs: the interference the analysis charges is
  an upper bound. The model is that of a bus without faults, as for `can_irq`: error frames, the
  controller's state changes and the transmit queue's aborts of frames in the mailboxes are
  outside it. The derivation is in app/task_table.c. Modelled per event instead (T = 47 µs),
  `can_tx` would be unschedulable, because it runs below `sensor` and `process` by design.
- **A task released by another task** is periodic with the releasing task's period, and its
  release jitter is the releasing task's worst-case response time (Audsley et al. 1993, section
  4). `process` is released by the sensor task's sample: T = 10 ms and J = 2000 µs, the sensor
  task's deadline, which bounds its response time. Its deadline and response time count from the
  sensor task's arrival, so its response time is the latency from the tick.
- **Blocking.** Every task that calls can_io enters critical sections around the transmit
  queue, at most 5 µs; the health task suspends the scheduler while `uxTaskGetSystemState()`
  reads the task list (20 µs); `process` and `health` share the pack mutex (40 and 10 µs).
- **Phase 3** added to two budgets. `process`: the verdict on each sample for the fault monitor
  and its post to the health task, 10 µs (150 → 160 µs). The processing job makes no log call:
  a log line costs 100 µs, more than `can_tx`'s margin below, so the lines about a stall of the
  samples are the health task's, written from the processing task's counts. `health`: the
  supervision cycle and the kick (17 µs), up to a queue full of verdicts worked into the DTCs
  (32 × 21 µs: each taken out and its tag of clear requests compared, then up to nine reports
  with the read of testFailed after each for the summary of the period; the task takes no more
  than that in one period), up to a clear before each verdict and one after them (33 × 15 µs),
  the CAN controller's counters and monitors and the summary (40 µs), BMS_Fault (25 µs), and
  log lines of 100 µs: two for every fault, which can rise and fall within one period, one for
  the clears, one for the expiry of a supervised task and two for a stall of the samples
  (36 × 100 µs): 300 → 5149 µs, most of it the log lines of a period in which every fault
  changes twice, which does not happen; the health task is the lowest-priority task, so its
  budget delays no other. The health task's
  frames are spread over its periods (BMS_TaskStats in the odd ones, BMS_NodeInfo in every tenth),
  so a period publishes at most two and `can_tx`'s count of passes for it stays at two. The DTC
  work belongs to the health task and not to the processing task, the task that sees the
  samples, because the processing task is above `can_tx`: each microsecond in its budget is one
  more in `can_tx`'s response time. With the owner's work in the processing task (its budget
  160 + 54 = 214 µs: the reports, the summary and BMS_Fault) the analysis finds `can_tx`
  unschedulable, R = 1209 µs against its deadline of 1000 µs.
- **Phase 4** added the flash task and five microseconds to the health task's budget.
  `health`: the period judged for the confirm of a trial and, once per run, the request to the
  flash task (5 µs); one more fault, UPDATE_ROLLBACK, adds two possible log lines (200 µs):
  5149 → 5354 µs. BMS_BootInfo goes out in a period without BMS_TaskStats and BMS_NodeInfo, so
  a period still publishes at most two frames and `can_tx`'s count of passes for the health task
  stays at two. `flash`: sporadic, released at most once per health period (T = D = 100 ms), its
  budget the CPU time of one confirm: two scans of the 64 entries of the metadata log (30 µs an
  entry), the record handed to the driver (100 µs) and two log lines (100 µs each): 4140 µs. Its
  waits for the part block it and take no processor time, so they are not in the budget; a job
  that waits in the middle is outside the model (see [Filling in the table for another
  task](#filling-in-the-table-for-another-task)), which for the lowest-priority task lengthens
  only its own response. Its critical sections (a log line's push, the result handed back) are
  counted as 5 µs, those of the transmit queue.
- **The idle task** is not listed. There are no software timers (`configUSE_TIMERS` is 0), so
  there is no timer task either.

### What each test proves

| Test | What it proves | What it assumes beyond the model below |
|---|---|---|
| U = Σ C/T ≤ 1 | Nothing when it holds. When it fails, no scheduler can meet every deadline with these budgets. | none |
| Liu and Layland (1973): U ≤ n(2^(1/n) − 1) | Schedulable when it holds. Nothing when it fails: it is a sufficient test only. | distinct rate-monotonic priorities; deadlines equal to periods; no blocking; no release jitter |
| Hyperbolic bound, Bini, Buttazzo and Buttazzo (2003): Π(C/T + 1) ≤ 2 | The same as Liu and Layland, and it accepts every set they accept, and more. | the same |
| Response-time analysis: Joseph and Pandya (1986); Audsley, Burns, Richardson, Tindell and Wellings (1993) | The worst-case response time of every task, compared with its deadline. This is the verdict of the report and the exit status. | any fixed priorities. Exact without blocking; an upper bound with it. |

When the table breaks an assumption of the two bounds, the report says "not applicable", names
the assumption, and does not evaluate the bound. Applied outside its assumptions, a bound can
accept a set that misses a deadline.

The response-time analysis follows each task's busy period over all of its jobs (Lehoczky 1990;
Tindell, Burns and Wellings 1994). For a task that misses its deadline it therefore reports the
worst response time, not only the first job's. A task that meets its deadline has a single job
in its busy period, and its response time is the classic one for the first job.

Blocking is that of priority inheritance (Sha, Rajkumar and Lehoczky 1990):

- A lower-priority task delays a task while it holds a mutex that this task, or a task above it,
  takes.
- A non-preemptive stretch of a lower-priority task delays every task above it.
- A job is blocked at most once on each such resource and at most once by each lower-priority
  task. The analysis takes the smaller of the two sums this gives.

Tasks of equal priority, which FreeRTOS time-slices, count each other as interference. All
arithmetic is exact, in integers and fractions.

The report also says whether the priorities are in rate-monotonic order and in
deadline-monotonic order. If they are not:

- The two bounds do not apply.
- The response-time analysis is unaffected: it holds for any fixed priorities, and its verdict
  stands.
- If the order is not deadline-monotonic, the report repeats the analysis with
  deadline-monotonic priorities. For deadlines no longer than periods, without blocking and
  jitter, that order is optimal (Leung and Whitehead 1982): if any fixed-priority order makes
  such a set schedulable, deadline-monotonic does. With blocking or jitter another order can
  still do better.

### The result for the node's task table

The table the node wrote in a run of one second, checked as the system test checks it:

```bash
"${FCN_BUILD_ROOT:-build}/release/bin/fcn_node" --duration-s 1 --stats-json stats.json
python -c 'import json; print(json.dumps(json.load(open("stats.json"))["task_table"]))' > task_table.json
python -m fcn_tools.sched task_table.json
```

```
Utilisation U = sum of C/T = 0.8145
  necessary          U <= 1                                       holds
  Liu and Layland    U <= n(2^(1/n) - 1) = 0.7241 for n = 8       not applicable: deadline shorter than period: sensor; release jitter: process; blocking by lower-priority tasks: can_irq, tick_irq, can_rx, sensor, process, can_tx, health; priorities not in rate-monotonic order
  hyperbolic         prod(C/T + 1) = 2.0355 <= 2                  not applicable: deadline shorter than period: sensor; release jitter: process; blocking by lower-priority tasks: can_irq, tick_irq, can_rx, sensor, process, can_tx, health; priorities not in rate-monotonic order

Priority order (a higher number is more urgent)
  rate-monotonic      no: can_tx (T 1000, priority 3) is not above sensor (T 10000, priority 5) (and 1 more pair(s))
  deadline-monotonic  no: can_tx (D 1000, priority 3) is not above sensor (D 2000, priority 5) (and 1 more pair(s))

  task      prio  kind           T       D     C     J   B      R  slack  job  verdict
  can_irq      9  sporadic      47      47     2     0  20     22     25  1/1  meets
  tick_irq     8  periodic    1000    1000     3     0  20     25    975  1/1  meets
  can_rx       6  sporadic    1000    1000   169     0  20    202    798  1/1  meets
  sensor       5  periodic   10000    2000   100     0  20    306   1694  1/1  meets
  process      4  periodic   10000   10000   160  2000  30   2484   7516  1/1  meets
  can_tx       3  sporadic    1000    1000   479     0  25    978     22  1/1  meets
  health       2  periodic  100000  100000  5354     0   5  19907  80093  1/1  meets
  flash        1  sporadic  100000  100000  4140     0   0  34801  65199  1/1  meets

Verdict: schedulable: every task meets its deadline
```

(An excerpt; the report also lists the blocking of every task and the analysis with
deadline-monotonic priorities, under which the set is schedulable too.) Both bounds are not
applicable, as expected for this table: the sensor task's deadline is shorter than its period,
the processing task has release jitter, the critical sections and the pack mutex mean blocking,
and `can_tx` breaks rate-monotonic order. The response-time analysis is the verdict: with the
design budgets, every task meets its deadline. The smallest margin relative to its deadline is
`can_tx`'s, 22 of 1000 µs (37 in phase 2, 27 in phase 3: the 10 µs added to the processing
task's budget in phase 3 are 10 µs more of `can_tx`'s response, and in phase 4 the flash task's
critical sections add 5 µs to its blocking). That margin is thin, and most of `can_tx`'s 479 µs
is what its passes cost whatever they carry: 338 µs, 13 µs for each of 26. The analysis adds up
the non-preemptive stretches of the lower-priority tasks (`can_tx`: the health task's 20 µs and
the flash task's 5 µs, B = 25), the smaller of its two sums; on one processor a job can be
blocked by only one of them, the one running when it arrives, so 20 µs would be the bound here
and the 5 µs are a pessimism of the analysis. The processing task's response, 2484 µs from the
tick, is mostly the 2000 µs of release jitter that the sensor task's deadline allows it. The
health task's response, 19907 µs, is a fifth of its period, and the flash task's, 34801 µs from
the request, a third of its: CPU time only, before its waits for the part. The largest blocking
term, 20 µs, is the health task's scheduler suspension, which the analysis counts against the
interrupts as well, a pessimistic choice for `vTaskSuspendAll()`, which does not mask
interrupts.

### The model, and where the POSIX port leaves it

Every result of the analysis is about this model of the node:

1. One processor. The highest-priority ready job runs, and it preempts the running one at once
   (`configUSE_PREEMPTION`).
2. The jobs of task i arrive at least T_i apart. Each becomes ready at most J_i after it arrives.
3. A job runs for at most C_i. Before it completes, it waits for nothing but a mutex.
4. Mutexes have priority inheritance, as FreeRTOS mutexes do. Critical sections on them are not
   nested.
5. Interrupt handlers exist only as tasks of the table: sporadic tasks above every other
   priority, with the shortest time between two of their interrupts.

On a microcontroller, assumption 1 holds by construction: the core executes the task the kernel
chose until an interrupt arrives. On the FreeRTOS POSIX port it does not hold. Every task is a
thread of a Linux process. The FreeRTOS scheduler decides which thread may run, but the host's
scheduler decides when that thread gets a CPU (and under WSL2, Windows schedules the virtual
machine). A job can stand still in the middle of its execution while FreeRTOS considers it
running, and nothing in the model accounts for that time.

Assumption 3 is not a fact about the host either. An execution time measured on the host is
that of a laptop CPU under a general-purpose operating system, and it says nothing about a
Cortex-M. The tick and the simulated CAN interrupt are signals that host threads outside the
scheduler send to the thread of the running task ([posix_tick.c](../platform/posix/posix_tick.c),
[posix_irq.c](../platform/posix/posix_irq.c)). A periodic task is released as late as the host
delivers its tick.

On the POSIX port the check is therefore a check of the design: the task set, its priorities,
its budgets and its blocking. It does not predict the response times the node shows on the host.
A response time measured on the host that exceeds the analysed one does not contradict the
analysis. What carries over is what is counted rather than timed: which task can preempt or
block which.

### Filling in the table for another task

- **A task released by several kinds of event** (a queue, notification bits, a timeout) has as
  many arrivals as those events can produce: give the shortest time between two of them.
  Notifications that coalesce let one job serve several events; the analysis still counts one
  job per event, so it stays an upper bound.
- **A job that waits mid-execution** for anything but a mutex (a queue, a delay, a notification)
  is outside the model. For the analysis it is two tasks, one per part.
- **An interrupt handler**: its execution time is the handler's, and its minimum inter-arrival
  time is the shortest time between two of its interrupts; for the CAN controller that is the
  shortest frame on the bus at its bit rate, which [fcn_tools.canbits](../tools/fcn_tools/canbits.py)
  gives.

### How the check is tested

[tests/python/test_sched.py](../tests/python/test_sched.py) checks the analysis against answers
that do not come from it:

- task sets whose response times are worked out by hand in its comments;
- the examples of Audsley et al. (1993), against the paper's text. The values the paper gives
  for Tables 1 and 2 come out exactly, and so do 16 of the 18 rows of Table 3. Row 11 matches
  only with a blocking time 1000 µs above the printed 1050 µs. The paper's row 12 is a larger
  solution of the same equation: the paper starts each task's iteration at the response time of
  the task above it, which assumes the lower task's response time is the longer one. Blocking
  can break that assumption, and here it does. Both rows are worked out in the test;
- a discrete-time simulation of the schedule as an independent oracle: preemption, priority
  inheritance and non-preemptive stretches, from the critical instant over the hyperperiod, on
  random task sets with integer parameters. The analysis is never below the simulated worst
  case, with blocking and with any release pattern within the jitter. It equals the simulation
  without blocking, both for synchronous releases and for the critical-instant pattern with
  jitter.
