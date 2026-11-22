# Bug log

Real defects that a test caught while this project was being built. Three rules keep the log
honest:

1. An entry exists only if a test actually failed because of the bug. Design changes, tooling
   problems and review remarks that no test backs up are not listed here.
2. An entry is written together with the fix, with the output of the failing test saved under
   `docs/buglog/<id>/failing-output.txt`.
3. Every entry carries `reintroduce.patch`, which puts the bug back into the current sources,
   and one or more `test*.sh`: the test that caught the bug and, where one was added with the
   fix, the regression test. `scripts/buglog-verify.sh` works on a scratch copy: it requires
   every test to pass without the patch and to fail with it, and the patched sources to
   build. CI runs that script, so an entry whose bug cannot be re-created, or whose test
   stopped noticing it, breaks the build.

```bash
scripts/buglog-verify.sh
```

| # | Phase | What was wrong | Caught by |
|---|---|---|---|
| [001](#001-the-stack-report-read-the-stack-of-a-task-that-no-longer-existed) | 0 | The stack report read the idle task's stack after the kernel had deleted the task | system test of the node's statistics |
| [002](#002-interrupt-handlers-could-stack-up-until-the-task-stack-overflowed) | 0 | Simulated interrupt handlers could run on top of each other without bound; an interrupt storm overflowed the task's stack | interrupt-storm self-test written during the review of phase 0 |
| [003](#003-a-suspended-scheduler-made-the-tick-run-fast-and-then-stand-still) | 0 | While a task held the scheduler suspended, the locked tick took the kernel's deferred ticks for lost ones: time ran several times too fast, then stood still | suspended-scheduler self-test written during the review of phase 0 |
| [004](#004-a-short-set-at-the-instant-a-received-frame-was-due-to-start-let-the-frame-through) | 1 | In the CAN controller model, a bus short set exactly at the start of a queued received frame took effect a frame late, and the frame was delivered | unit test of the controller model |
| [005](#005-frames-from-other-nodes-went-onto-the-bus-ahead-of-the-nodes-own-whatever-their-priority) | 1 | In the CAN controller model, every frame read from the transport went onto the bus ahead of the node's own mailboxes, whatever its priority: a flood of 0x700 just above the bus capacity kept the status frame 0x030 off the bus for as long as it lasted | flood test written during the review of phase 1 |
| [006](#006-an-abort-took-back-frames-the-driver-loaded-after-it) | 1 | On the host, an abort of the transmit mailboxes also took back the frames the driver loaded right after it, whenever they were still on their way to a mailbox when the hardware thread served the abort | platform test written during the review of phase 1 |
| [007](#007-the-receive-task-took-fewer-frames-per-tick-than-a-full-1-mbits-bus-delivers) | 1 | The receive task took at most 16 frames per tick, fewer than a full 1 Mbit/s bus delivers: the receive FIFO overran for as long as such a load lasted | flood at 1 Mbit/s during the review of phase 1; system test added with the fix |
| [008](#008-a-can-transport-that-went-away-went-unnoticed) | 1 | A SocketCAN interface deleted under the running node went unnoticed: frames were still reported sent, nothing was logged, and the node exited 0 | run that deleted the node's interface during the review of phase 1; system test added with the fix |
| [009](#009-the-stack-report-left-out-the-frame-of-the-task-function) | 0 | The stack depth was counted from the frame of `plat_task_enter()`, so a buffer in a task function's own body never appeared in the report | review of phase 1 (a buffer planted in a task's body); stack self-test added with the fix |
| [010](#010-the-image-found-the-files-still-locked-by-the-bootloader-it-had-replaced) | 4 | The bootloader kept the flash and the retained cell locked up to its exec and left their release to the exec; when another process held a reference to the bootloader's memory map at that moment, the locks outlived the exec and the image it had started found both files "in use" | asan run of the system tests on CI (GitHub's ubuntu-24.04 runner); platform and system tests added with the fix |

---

## 001 The stack report read the stack of a task that no longer existed

**Found** 2026-11-15, Phase 0, on the first run of the system tests, by
`tests/system/test_smoke.py::test_runs_for_the_requested_time_and_reports_statistics`:

```
E           assert 511584 < (256 * 1024)
```

The node's statistics claimed the idle task had used 511,584 bytes of its 524,288-byte stack.

**Cause.** On the host, a task's real stack depth is measured by painting the unused part of
its pthread stack when the task starts and scanning for the first overwritten byte later
(`platform/posix/posix_stack.c`). The statistics were written by `main()` after the scheduler
had stopped. Stopping the scheduler makes the kernel delete the idle task; the POSIX port then
cancels and joins that task's thread, and the C library releases the thread's stack pages. The
scan therefore read zero-filled pages where the paint pattern used to be, found "used" memory
at the very bottom of the stack and reported the whole stack as consumed. Every other task
looked fine, because their threads are only parked, not deleted, when the scheduler ends; that
is also why the platform self-test, which did not register the idle task yet, had passed.
Reading the stack of an exited thread could just as well have faulted.

**Fix.** The platform records every task's peak just before it tells the scheduler to end
(`posix_stack_freeze()`, called from `plat_end_scheduler()`), while all threads still exist,
and reports the recorded values from then on.

**Regression test.** `plat_selftest stack` now registers the idle task too and fails if the
report after shutdown is missing it or shows any task using more than half its stack. The
system test that found the bug stays as it was.

**Evidence.** [failing output](buglog/001-stack-report-after-shutdown/failing-output.txt),
[patch that re-creates the bug](buglog/001-stack-report-after-shutdown/reintroduce.patch).
The verification runs both the system test that found the bug (`test.sh`) and the regression
self-test (`test-regression.sh`) against the patch. The bug was found and fixed before the
repository's first commit, so no commit contains it; the patch is the way to see it fail.

---

## 002 Interrupt handlers could stack up until the task stack overflowed

**Found** 2026-11-15, in the review of phase 0. Reading `platform/posix/posix_irq.c`, the
reviewer doubted the comment that said nothing could unblock signals before the handler was
done, and proved the doubt with a test: the self-test's interrupt source changed to raise the
line in a tight loop.

```
$ plat_selftest irq signal 0 2 1
Segmentation fault (core dumped)
```

Phase 0's own tests had passed because they never raised faster than about 3 kHz and did not
look at how deep the handler nested. With the nesting counter that the fix added, the old
code fails the ordinary 4 kHz test as well: handlers 8 deep and 13.7 KiB of stack where one
handler needs 2 KiB, and 25 deep at 10 kHz (depths as measured then, counted from the frame of
`plat_task_enter()`; see entry 009).

**Cause.** A simulated interrupt is a signal handled on the thread of the running task. The
handler must run with signals blocked from entry to return, as an ISR runs with its own
interrupt masked. The handler called `vPortEnterCritical()` and `vPortExitCritical()` around
its work, and a context switch requested by an ISR does the same inside `vPortYield()`. In the
FreeRTOS POSIX port, leaving the outermost critical section calls
`pthread_sigmask(SIG_UNBLOCK, all signals)`. So every signal was unblocked while the handler's
frame was still on the stack, and a signal that was already pending was delivered right
there, on top of it. That handler ended the same way, and so on: the depth grew by one for
every interrupt that arrived before the previous handler had returned, and nothing bounded
it. Each level costs about 1.7 KiB of the task's stack. The port's own tick handler does not
have the problem because it adjusts the nesting counter directly, which only code inside
`port.c` can do.

This was not a cosmetic limit: the CAN controller model of phase 1 raises this interrupt
once per frame and has a mode without pacing.

**Fix.** `pthread_sigmask()` is intercepted at link time (`-Wl,--wrap=pthread_sigmask`, the
mechanism already used for the tick). While a thread is inside the interrupt handler, a
request to unblock signals is dropped; the mask is restored when the handler returns, which
is what happens for the tick handler too. The kernel submodule stays unmodified.

**Regression test.** `plat_selftest irq` now reports the deepest handler nesting and the
deepest stack use of any task and fails if the nesting exceeds 1 or a stack exceeds 48 KiB,
and a period of 0 turns the source into an interrupt storm. CTest runs the storm in both
delivery modes (`platform_irq_storm_signal`, `platform_irq_storm_poll`), under ASan and UBSan
like every other test. With the fix a storm ends with nesting 1, 2 KiB of stack as measured
then (6.4 KiB counted from the top of the stack, as since entry 009) and no raise left
unserved; `results/platform_timing.json` (`interrupt_storm`) has the measured rates.

**Evidence.** [failing output](buglog/002-nested-interrupt-handlers/failing-output.txt),
[patch that re-creates the bug](buglog/002-nested-interrupt-handlers/reintroduce.patch).
The bug is in commits `7a347ab` to `e3c2f4b`.

---

## 003 A suspended scheduler made the tick run fast and then stand still

**Found** 2026-11-15, in the review of phase 0. The reviewer asked what the tick-locking code
sees while a task holds the scheduler suspended, and measured it: a task that calls
`vTaskSuspendAll()`, works for 20 ms and calls `xTaskResumeAll()`.

```
$ plat_selftest suspend 20
the tick count advanced by 106 ticks while the scheduler was suspended for 20001 us
the tick count advanced by 1 ticks in the 100000 us after the scheduler was resumed
```

Twenty milliseconds of work moved FreeRTOS time forward by 106 ms, every timeout in the
system that lay within those 106 ms expired at the same instant, and then the tick stood
still while the wall clock caught up: one tick in the following 100 ms. A 100 ms suspension
gave 518 ticks. Nothing in phase 0 keeps the scheduler suspended that long, so no test had
seen it.

**Cause.** The platform keeps the tick locked to the wall clock by comparing the ticks that
should have happened with the ticks the kernel has counted, and raising a replacement for
each one that is missing ([posix_tick.c](../platform/posix/posix_tick.c)). It took "the ticks
the kernel has counted" from `xTaskGetTickCount()`. While the scheduler is suspended that
count does not move: the kernel takes every tick interrupt, notes it in `xPendedTicks`, and
adds them all to the tick count when the scheduler resumes. So each tick taken during the
suspension looked lost. The loop raised replacements, several per millisecond, the kernel
faithfully noted every one of them, and on resume all of them were added to the tick count.
From then on the count was ahead of the clock, and the loop waited for the clock to catch
up.

**Fix.** The loop now counts tick interrupts where the kernel takes them: the tick hook
increments a counter (`posix_tick_taken()`), and that counter is what is compared with the
wall clock. The kernel calls the tick hook once for every tick interrupt, whether the
scheduler is suspended or not, and does not call it again when the noted ticks are replayed.
A side effect: the tick thread no longer reads a kernel variable it has no business reading.

**Regression test.** `plat_selftest suspend <ms>` suspends the scheduler for the given time
and fails unless the tick count advanced by the time that passed, both during the suspension
and in the second after it (the task blocks for 1000 ticks, and their wall time is compared
with them). CTest runs it with 20 ms (`platform_tick_suspended_scheduler`). With the fix:
20 ticks during, 1000 after. (Until the review of the tests the second interval was 100 ms
of busy-waiting with a tolerance of 3 ticks; see `tests/platform/plat_selftest.c` for why
both changed.)

**Evidence.** [failing output](buglog/003-tick-count-while-suspended/failing-output.txt),
[patch that re-creates the bug](buglog/003-tick-count-while-suspended/reintroduce.patch).
The bug is in commits `7a347ab` to `c754d4c`.

---

## 004 A short set at the instant a received frame was due to start let the frame through

**Found** 2026-11-15, by the unit tests of the CAN controller model while it was written
(phase 1). The test `test_a_received_frame_due_to_start_at_the_instant_of_a_short_never_passes`
queues two received frames back to back and shorts the bus exactly at the start of frame of
the second.

```
test_can_ctrl.c:1185:test_a_received_frame_due_to_start_at_the_instant_of_a_short_never_passes:FAIL: Expected 1006000 Was 1106000:next wake-up
```

The short took effect 50 bit times late, after the second frame, and that frame was delivered
to the driver although the bus was shorted at its start of frame.

**Cause.** A fault set while a frame is on the wire takes effect after that frame
(`lib/can/can_ctrl.h`, "A fault that begins or ends while a frame is on the wire takes effect
from the next frame"). To find "the frames on the wire", `free_after_started()` walks the queue
of received frames and stops at the first one that has not started yet. Its test was
`start_ns > at_ns`, so a frame starting at exactly the instant of the fault counted as already
started. Everywhere else the model applies an input before the events of its own instant: a
transmission due to start at the instant a fault is set starts under the fault. The two rules
disagreed at a single nanosecond.

**Fix.** `start_ns >= at_ns`: a frame due to start at the instant of the fault has not started.
The rule is now written next to the function.

**Regression test.** The test that found it, in `tests/unit/test_can_ctrl.c`. Since the fix
of bug 005, the second frame of that test waits for the bus instead of being placed in
advance, so the comparison this fix changed no longer decides its outcome.
`test_a_received_frame_placed_at_the_instant_of_a_short_never_passes`, next to it, first polls
at the frame's start of frame, so that the frame is on the timeline at the instant of the
short. It is the test that fails with `reintroduce.patch` now:

```
tests/unit/test_can_ctrl.c:1481:test_a_received_frame_placed_at_the_instant_of_a_short_never_passes:FAIL: Expected 1006000 Was 1106000:next wake-up
```

**Evidence.** [failing output](buglog/004-fault-at-the-start-of-a-queued-frame/failing-output.txt)
(the original test, before the fix of 005),
[patch that re-creates the bug](buglog/004-fault-at-the-start-of-a-queued-frame/reintroduce.patch).
The bug never reached a commit.

---

## 005 Frames from other nodes went onto the bus ahead of the node's own, whatever their priority

**Found** 2026-11-16, in the review of phase 1. The reviewer flooded the node's bus with
8-byte frames 0x700, a little faster than the bus can carry them, and counted the node's
status frames 0x030. For as long as the flood lasted, almost none got out, although on a real
bus 0x030 wins arbitration against 0x700 at every frame boundary. With the bug put back, the
system test added with the fix (a flood at 1.2 times the capacity of a 125 kbit/s bus, 1.5 s)
fails on both transports:

```
E           AssertionError: status counter 1 followed by 133
```

After the first period's value, the next one on the bus was that of period 133. The node had
published 160 values and replaced 132 of them (`superseded`), and only 28 reached the bus.

**Cause.** `can_ctrl_wire_rx()` put each frame read from the transport on the bus timeline at
once: ending at its arrival, or, if the bus was busy, right after everything already placed.
It then moved the bus-free time past the frame. The node's own frame could start only at that
bus-free time. So the wire queue, up to 64 frames (about 15 ms at 500 kbit/s), was a
reservation of the bus ahead of every mailbox, decided before any arbitration. Under a steady
flood the host glue refilled the queue on every pass, and the node never got the bus. Two
things kept this hidden. The documentation said the model sends "the highest-priority mailbox
first", and that was true among the node's own frames only. The unit test
`test_a_transmission_requested_while_frames_are_being_received_starts_after_them` asserted the
inverted order (a 0x009 of the node after a queued 0x2AA). No system test offered more traffic
than the bus carries.

**Fix.** A received frame that could not have passed by the time it is handed in now waits in
the wire queue, as a frame waits in the transmit buffer of its sender. Whenever the bus
becomes free, the node's best mailbox and the oldest waiting received frame arbitrate, as on a
real bus: the smaller arbitration key starts. Equal keys, which two nodes may not send, go to
the received frame. A frame handed in that finds the bus free, with no other received frame
waiting, still ends at its arrival. Received frames keep their arrival order, and the queue
and `wire_overruns` keep their meaning. `lib/can/can_ctrl.h` states each modelling decision
(ARBITRATION, Receive).

**Regression test.** The unit tests of "2b. arbitration against received frames" in
`tests/unit/test_can_ctrl.c`. The test that asserted the inversion was rewritten as
`test_a_frame_of_this_node_goes_before_a_waiting_received_frame_with_a_larger_key`. The
section also has the mirror case, ties, the arrival order, the best mailbox, a frame handed in
at the instant the bus becomes free, the late-caller rule, and a flood in model time: each
frame of the node waits at most for the one frame on the bus. Then
`tests/system/test_can_flood.py`, on both transports: every status value is on the bus or
counted as replaced, and every flood frame is delivered or counted (`wire_overruns`,
`rx_overflow`). Its timing twin asserts that no value is replaced and that the period holds on
the wall clock.

**Evidence.** [failing output](buglog/005-received-frames-ahead-of-the-mailboxes/failing-output.txt),
[patch that re-creates the bug](buglog/005-received-frames-ahead-of-the-mailboxes/reintroduce.patch).
The verification runs the unit test (`test.sh`) and the system test (`test-flood.sh`). The bug
is in commit `a4039fa`.

---

## 006 An abort took back frames the driver loaded after it

**Found** 2026-11-16, in the review of phase 1. Reading the abort path of the host CAN
controller, the reviewer saw that serving an abort emptied the whole transmit ring. That ring
can hold requests the driver made after the abort. A platform test confirmed it: whether a
frame loaded right after `plat_can_abort_tx()` went out or came back aborted depended on how
soon the host ran the hardware thread. With the bug put back, the regression test added with
the fix loses every one of its 50 fresh frames:

```
tests/platform/test_posix_can.c:778:test_a_frame_loaded_right_after_an_abort_is_not_aborted:FAIL: Expected 0 Was 50:frames loaded after the abort were aborted
```

**Cause.** `plat_can_abort_tx()` sets a command bit and wakes the hardware thread
([posix_can.c](../platform/posix/posix_can.c)). The driver's transmit requests reach the
mailboxes through a ring, which the hardware thread empties into the mailboxes on each pass.
When it served an abort, it aborted the frames in the mailboxes and then reported every request
still in the ring as aborted too. A comment next to that loop assumed that a request still on
its way to a mailbox was made before the abort. A request the driver made after
`plat_can_abort_tx()` had returned, but before the hardware thread's next pass, was in the ring
as well, and it was aborted with the rest. The CAN transmit task does exactly this: it requests
the abort and, in the same activation, loads the frames the queue hands out (`carry_out()` and
`load_mailboxes()` in [can_io.c](../app/can_io.c)). A mailbox abort on a real controller
affects only the frames that are loaded when it is requested.

**Fix.** Every transmit request carries a submission serial, which `plat_can_tx()` takes from a
counter. `plat_can_abort_tx()` stores the counter's value as the abort mark before it sets the
command. The hardware thread aborts the frames in the mailboxes, and from the ring only the
requests with a serial before the mark; the comparison holds across the wrap of the counter.
The first later request ends the drain, and it and the ones behind it are loaded as usual.
Nothing is loaded while an abort is pending, so when an abort is served, the mailboxes hold
only frames made before it. The loading loop takes a request from the ring before it looks for
a pending abort: the driver sets the abort's command before it pushes a later request, so a
request taken that was made after an abort always finds the command set and waits for the abort
to be served. (Looking first and taking afterwards, as the first version of the fix did, left a
window of two operations in which a fresh frame could still be loaded and then aborted; the
verification of the fix found it by widening the window.)
[plat_can.h](../platform/include/plat_can.h) now states the contract: a frame loaded after the
call returned is not affected, however soon after it comes.

**Regression test.** `tests/platform/test_posix_can.c`.
`test_a_frame_loaded_right_after_an_abort_is_not_aborted` aborts and at once loads a fresh
frame, 50 times, and expects every fresh frame to be sent.
`test_an_abort_takes_back_the_frames_loaded_before_it_and_no_other` loads three frames on a bus
that nobody acknowledges, aborts, and loads a fourth. It expects exactly the three back as
aborted, and the fourth on the bus once there is an acknowledgement again. With the patch the
second test fails as well, because the fourth frame comes back aborted (`Expected 3 Was 2:an
outcome too early`). Both tests also run under ThreadSanitizer (`scripts/test-tsan.sh`). The
two-operation window described under Fix is not covered by a test: hitting it on purpose needs
a way to hold the hardware thread between the two operations, which the platform test does not
have.

**Evidence.** [failing
output](buglog/006-abort-took-back-frames-loaded-after-it/failing-output.txt), [patch that
re-creates the bug](buglog/006-abort-took-back-frames-loaded-after-it/reintroduce.patch). The
patch removes the comparison with the abort mark, so the abort empties the whole ring again.
The bug is in commit `a4039fa`.

---

## 007 The receive task took fewer frames per tick than a full 1 Mbit/s bus delivers

**Found** 2026-11-16, in the review of phase 1. The reviewer flooded the node's bus at 1 Mbit/s
with the shortest frames there are, base-format data frames without data. Comparing what the
controller model delivered with what the driver took, a large part was lost to receive FIFO
overruns, whether or not a handler was registered for the identifier. With the bug put back,
the system test added with the fix loses 22.3 % and 18.4 % of the frames delivered in its
measurement window, one run for each of its two identifiers:

```
E           AssertionError: 9149 of 41113 frames delivered in the window lost to receive FIFO overruns (32.0 frames/ms offered, 20.6 received, bus busy 94%, largest wake-up lag of the hardware thread 10.1 ms; ticks dropped from the window's start to the end of the run 0, in the whole run 9; rx_batches_cut in the whole run 2287)
```

**Cause.** The receive task drains the controller's receive FIFO in batches of at most
`CAN_IO_RX_BATCH` frames per activation, so that its execution time per activation has a bound.
When a batch is full, it lets the other tasks run until the next tick and counts the cut
(`rx_batches_cut`); otherwise it waits for the next frame. The batch was 16 and the tick is
1 ms. A full 1 Mbit/s bus delivers up to 21.3 frames per millisecond: the shortest frame is 44
bits, plus the 3-bit intermission. While the task kept pace, taking frames as they came, the
batch did not matter. But once anything delayed it long enough for 16 frames to be waiting (at
full load, 0.75 ms: a tick handler, a lower-priority task in a critical section, the host), it
took 16 per tick from then on, the backlog grew by up to 5.3 frames every millisecond, and the
FIFO of 512 frames filled and overran for as long as the load lasted. The batch size had been
chosen without reference to the bit rate. At 500 kbit/s a full bus carries at most 10.6 frames
per millisecond, and at 1 Mbit/s frames with 8 data bytes come at most 9.0 per millisecond. Both
are below 16, which is why no earlier test had seen the bug.

**Fix.** `CAN_IO_RX_BATCH` is now 32 ([can_io.h](../app/can_io.h)). A compile-time check in
[can_io.c](../app/can_io.c) ties it to the tick rate and to `PLAT_CAN_MAX_BITRATE`, the highest
bit rate the controller can be configured for (now in
[plat_can.h](../platform/include/plat_can.h), and the limit of the node's `--bitrate`). One
batch per tick must cover the frames a full bus can deliver in a tick. The margin above 21.3
drains a backlog: up to 10.7 frames more than arrive, every tick.

**Regression test.**
`tests/system/test_node_errors.py::test_the_receive_path_keeps_up_with_a_full_bus_of_the_shortest_frames_at_1_mbit`
runs once for an identifier nobody handles and once for the ping. A tester offers 32 frames per
millisecond to a node at 1 Mbit/s until the test ends the node's run (SIGTERM, right after the
measurement window below). Every frame the model delivered must be accounted for exactly: taken
by the driver, lost to an overrun, or still in the FIFO at the end. The test makes the delay
that starts the overrun itself: 0.3 s into the load it stops the node for 10 ms (SIGSTOP), as a
busy host would. When the node runs again, the controller model delivers at once the 64 frames
that waited for the bus (its wire queue; the tester's frames beyond those are wire overruns). A
receive path that keeps up drains them in about 6 ms; one that takes 16 per tick falls behind.
Load and losses are then measured over the same 2 s window, 0.2 s after the stop. The model
must have delivered at least 16.8 frames per millisecond in the window, more than 16 per tick;
that is a timing tolerance, checked first, since a lighter load says nothing about the receive
path. At that rate a 16-per-tick path that stays behind for the whole window loses at least
4.8 % of the window's frames; the test allows 1 %. (It does not always stay behind: one of the
ten runs with the bug put back quoted below lost 4.7 %, with batches cut in 735 ticks of the run
instead of about 2300.)

A fixed node loses more than 1 % of the window, 337 frames or more, only when the host holds its
receive task up for at least 12.8 ms inside the window: the bus delivers at most 21.3 frames per
millisecond, and a hardware thread that the host did not run delivers at most the 64 of the wire
queue at once. While the task is held up, the kernel counts at most one tick (ticks are taken
on the thread of the running task, and the receive task outranks every other), so the tick
code's lag grows. The tick code makes up a lag of up to 6 ticks and gives up all but one tick
of a longer one, at least 6 at a time: all at once when the tick thread runs again, if the host
held it up too, or 6 every 6 ms while the hold-up lasts, if it did not. The part of a hold-up
in one piece inside the window is enough for at least one such give-up after the window began,
and the run cannot end before it is counted: the health task ends the run on a tick, and the
tick thread checks the lag, and gives up one longer than 6, right after each tick it raises.
The node reports the ticks given up so far in `can.status` (`ticks_dropped`), and the test
counts those from the window's start (its first `can.status`) to the end of the run (the
statistics), which it ends right after the window: a hold-up that overlaps the window's start
or end is in that span, the start-up and the test's own stop are not. The 1 % limit is a timing
tolerance in a run that dropped 6 ticks or more in that span, and a plain assertion in one that
dropped fewer.

A host that holds the task up in many short pieces is not covered. One that runs it late at
every wake-up, after the next tick but before the one after it, costs it every other tick: the
kernel takes the tick that came meanwhile as soon as the task runs, the task was not waiting for
that tick, and after its batch it waits for the next one. No tick is missed and none is given
up, and the test calls the losses a failure. On the development laptop on 2026-11-21, a copy of
the test made a host like that for the whole window (the receive task's thread made
`SCHED_IDLE` and put on one processor, the node's other threads kept off it, with a busy loop
there that pauses for 0.2 ms every 2 ms): the fixed node lost 7.7 to 10.3 % of the window in
six runs, with no tick dropped. Busy loops of the thread's own priority on its processor, one
or three, cost it nothing in six runs. Hold-ups in one piece made the same way, inside the
window or across its start, were all counted: 1.2 to 2.6 % lost with 24 to 74 ticks dropped in
the span, a timing miss in all six runs.

`test.sh` repeats a run that missed only timing tolerances once, as `scripts/test-system.sh`
does, and exits with 75 if the repetition misses only timing tolerances too;
`scripts/buglog-verify.sh` then reports the entry as inconclusive rather than caught or broken.
The failing output is from a run on an idle host: neither run dropped a tick in that span, and
both failed.

Three earlier versions of this test went wrong on one host or another. The first two relied on
the host to cause the first delay. The first counted the losses over the whole run against 2 %
of everything received; on GitHub's runner it passed with the bug put back, once for the ping
and once for both identifiers. The second measured the losses in the window and printed its
figures: on the runner the patched node lost only 5.1 % and 1.2 % of the window, with batches
cut in about half of the ticks (2060 and 1728 of about 4000), where the development laptop
showed 20 to 22 % and a cut in every tick. On the faster runner the task fell behind less often,
and the second figure was a hair above the 1 % limit. Stopping the node before the window took
the host out of that question.

The third version judged the window's losses by the ticks dropped in the whole run, against 23:
what a 24 ms hold-up gives up when it holds up the tick thread too. On the development laptop on
2026-11-21 that left the entry inconclusive in two `scripts/buglog-verify.sh` runs in a row. In
the second, the patched node lost 23 to 31 % of the window in all four runs, but those runs had
dropped 222 to 628 ticks, and the fixed node, which lost nothing, 99 and 182: every failure was
a timing miss. The whole-run count takes in the start-up and the test's own stop, neither of
which can cost the window a frame. And 23 was no bound for a hold-up that the tick thread runs
through, of which the tick code gives up 6 ticks at a time and makes up the rest (18 or 24 of a
24 ms hold-up), nor for the part inside the window of a hold-up that began before it. The node
now reports the count in `can.status`, and the test counts from the window's start against the
bound that holds for every hold-up in one piece, 6. It also ends the run right after the
window instead of letting the node run on to the end of its 4 s. A copy of the test that also
read `can.status` at the window's end showed why: in 20 runs with the bug put back, two of the
four runs that dropped ticks in the span had dropped all of them after the window (23 and 8);
with the run ended after the window, none of 40 runs, with and without the bug, dropped a tick
after it. On the laptop afterwards
(`pytest tests/system/test_node_errors.py -m timing -k keeps_up`, release preset, five times
with the bug put back and five without), the patched node lost 4.7 to 22.2 % of the window in
all ten runs, with no tick dropped in the span in nine of them (a plain failure) and 12 in one
(a timing miss); the fixed node lost nothing in all ten, and dropped no tick in the span.

**Evidence.** [failing output](buglog/007-receive-fifo-overran-at-1-mbit/failing-output.txt),
[patch that re-creates the bug](buglog/007-receive-fifo-overran-at-1-mbit/reintroduce.patch).
The patch sets the batch back to 16 and removes the compile-time check, which would otherwise
refuse that value. The verification runs the system test (`test.sh`). The bug is in commit
`a4039fa`.

---

## 008 A CAN transport that went away went unnoticed

**Found** 2026-11-16, in the review of phase 1. The reviewer deleted the node's SocketCAN
interface while the node ran, in a user and network namespace of its own. The node went on
reporting every frame as sent, logged nothing about the transport, and exited with status 0 at
the end of its run. Only the end-of-run statistics showed the failed sends. With the bug put
back, the system test added with the fix finds the node still running 10 s after its interface
was deleted:

```
E       AssertionError: assert 'still running 10 s after its interface was deleted' == 74
```

**Cause.** The hardware thread ignored what the transport's `send()` returned, and a `recv()`
that failed only ended the reading for that pass
([posix_can.c](../platform/posix/posix_can.c)). The backends counted refused sends, but nothing
looked at the count while the node ran: `can.status` did not report it, nothing was logged, and
the node had no way to stop when its bus was gone.

**Fix.** The backend counts a failed send or receive with its errno. The count is reported in
`can.status` (`transport_tx_failed`, `transport_rx_failed`). The failure is logged: the first
one at once, then at most one line a second with the number of failures since the last line. A
failure that means the interface is gone also stops the node in an orderly way: ENODEV, when
the interface is unregistered under the socket and the kernel unbinds it, or ENXIO, a send on
such a socket. The hardware thread sends SIGTERM to the process, as an operator would, and the
scheduler ends. The statistics are written with the reason (`transport_error`). The node prints
`fcn_node: the CAN transport <spec> went away: <reason>` and exits with status 74
(`POSIX_EXIT_CAN_TRANSPORT_LOST`, [posix_platform.h](../platform/posix/posix_platform.h)). An
interface that is only down (ENETDOWN) is logged and counted like any other failure and does
not stop the node: the socket stays bound, and it works again once the interface is up. The bus
model is left alone: a frame the transport refused has still been transmitted as far as the
model and the driver are concerned, and no bus fault is derived from a transport failure.

**Regression test.**
`tests/system/test_node_errors.py::test_a_transport_that_goes_away_stops_the_node_with_its_own_exit_status`.
In a user and network namespace of its own (`unshare -rn`), so that the machine's `vcan0` is
not touched, it creates a vcan interface, starts the node on it, and deletes it. It expects
exit status 74 within 10 s, the line on stderr, the two log lines, the counters and
`transport_error` in the statistics, and an error-active controller with no errors. Where the
namespace or the `vcan` module is not available, the test is skipped; `test.sh` counts that as
a failure, since a skipped test cannot show the bug. Next to it,
`test_an_interface_that_goes_down_and_up_again_does_not_stop_the_node` takes the interface down
for a second and expects the node to log the failure, send again once the interface is up, and
stop on SIGTERM with status 0 and no transport error.
`tests/platform/test_can_backend.c::test_a_send_and_a_receive_the_socket_refuses_are_counted_with_their_error`
checks the counting in the UDP backend.

**Evidence.** [failing output](buglog/008-lost-transport-went-unnoticed/failing-output.txt),
[patch that re-creates the bug](buglog/008-lost-transport-went-unnoticed/reintroduce.patch).
The patch makes the hardware thread's handler of transport failures return at once. The bug is
in commit `a4039fa`.

---

## 009 The stack report left out the frame of the task function

**Found** 2026-11-16, in the review of phase 1. The reviewer added a 64 KiB buffer to the body
of `health_task` and ran the node:

```
$ fcn_node --can udp:39800 --duration-s 2 --stats-json ...
{"name": "health", ..., "stack_peak_bytes": 2264}
```

The unmodified node reports 2264 bytes as well. The task used more than 64 KiB, and the figure
did not move. The same buffer in a callee of the task function was reported (65552 bytes).
That is why the platform self-test, which burns its stack only in a callee, had passed.

**Cause.** `plat_task_enter()` took the origin of the measurement from its own frame
(`__builtin_frame_address(0)`) and counted the depth down from there. A task function
allocates its whole frame, its locals included, in its prologue, before its first statement
calls `plat_task_enter()`. That frame lies above the origin and was never counted. A buffer
declared in a task's body, the usual firmware pattern, was therefore invisible to the
measurement that later phases size stacks from.

**Fix.** The depth is counted from the top of the thread's stack, the high end of what
`pthread_getattr_np()` reports (`platform/posix/posix_stack.c`). Every frame of the thread is
now in the figure. That includes what the C library and the port keep above the task
function, which the report now gives separately as `start_bytes`: 4432 bytes for every task
whose task function calls `plat_task_enter()` (release build); the idle task, which registers
from its hook, reports 16 bytes more. The floor of the measurement rises accordingly, from 512
bytes to about 5 KiB (`platform/include/platform.h`, `plat_stack_info_t`). The report also
gives the stack each task was created with (`stack_size_bytes`), and the system test now
checks that what a task's own code used fits it.

**Regression test.** `plat_selftest stack` has a task with a 16 KiB buffer in its own task
function. It fails unless that task reports at least the buffer, and at least half the buffer
more than the task that uses nothing. It also checks that the tasks started the same way
report the same, non-zero `start_bytes`, and that what remains after subtracting it holds the
buffer. `tests/system/test_smoke.py` checks, for each application task, that its peak less its
start fits the stack it was created with, plus one signal frame.

**Evidence.** [failing output](buglog/009-task-frame-left-out-of-stack-depth/failing-output.txt),
[patch that re-creates the bug](buglog/009-task-frame-left-out-of-stack-depth/reintroduce.patch).
With the patch the body task is reported at 512 bytes, the floor. The bug is in commits
`7a347ab` to `a4039fa`.

---

## 010 The image found the files still locked by the bootloader it had replaced

**Found** 2026-11-21, Phase 4, by CI run 44956788218 on GitHub's ubuntu-24.04 runner (commit
`1bd6024`). In `scripts/test-system.sh` the functional run against the release build passed
(1038 tests) and so did the timing run (39). The run against the asan build failed 5 tests.
Each failed because the image that fcn_boot had just started found a file still locked:

```
FAILED tests/system/test_boot.py::test_without_a_retained_cell_the_image_gets_no_hand_off - AssertionError: node ended with 1
fcn_node: --flash-file .../flash.bin is in use by another process
FAILED tests/system/test_update.py::test_a_trial_that_fails_is_started_three_times_and_rolled_back_with_its_dtc[watchdog-11] - AssertionError: rolled-back ended with 1
fcn_node: --retained-file .../cell.bin is in use by another node
FAILED tests/system/test_update.py::test_a_trial_with_one_bit_flipped_is_rejected_and_never_started[payload] - AssertionError: node ended with 1
fcn_node: --retained-file .../cell.bin is in use by another node
FAILED tests/system/test_update_cuts.py::test_a_cut_anywhere_in_staging_leaves_the_confirmed_image_and_the_next_update_works - AssertionError: cut-2-0 ended with 1
fcn_node: --retained-file .../cut-2-0.cell is in use by another node
FAILED tests/system/test_update_cuts.py::test_a_cut_in_an_attempt_counts_against_the_trial_and_never_gives_it_a_fourth_start - AssertionError: run-0 ended with status 1, not a reset
payload crash: --retained-file .../cell.bin is in use by another node
========== 5 failed, 1033 passed, 39 deselected in 269.11s (0:04:29) ===========
```

fcn_boot itself had opened and locked the same files in each of these starts, so the lock the
image ran into was the one fcn_boot held before its exec. On the development laptop (WSL2) the
same tree had passed the asan functional run in each gate before the commit.

**Cause.** `1bd6024` (the fix of review finding r4-loader-1, a failed exec that left the
hand-off in the cell) made fcn_boot keep the flash and the cell open, mapped and locked up to
`fexecve()`. It left their release to the exec, because both descriptors are close-on-exec.
The same commit made the node and the test payloads open the cell first. A `flock` lock
belongs to the open file description, not to the descriptor. It goes with `flock(LOCK_UN)` or
with the last reference to the description, and a `MAP_SHARED` mapping is such a reference. The
exec closes the descriptor. The mapping goes only with the old memory map, when the last
reference to that map is dropped. Another process can hold such a reference at the moment of
the exec: the kernel takes one for each read of `/proc/<pid>/stat` or `/proc/<pid>/maps`.
Then the old map, its mappings and the locks outlive the exec until that process lets go, and
the image, already running, finds the files locked. When nothing else holds the map, the exec's
own release is the last one: the mappings and the descriptions go inside the `execve()` call,
before the image's first instruction, and the image always finds the files free. That is why
the laptop never showed it.

What held the reference on the runner is not known. The tests that failed read no `/proc` file
of the node or of fcn_boot. The reproduction below shows that a reader of those files is
enough, and that a reader of `/proc/<pid>/comm` (which takes no reference to the memory map)
is not.

**Reproduction** on the laptop (8 CPUs, kernel 6.6.87.2-microsoft-standard-WSL2). The asan
fcn_boot was started 2000 times in a row, with `ASAN_OPTIONS` as `scripts/test-system.sh` sets
them. Each time it started the crash payload, the confirmed image in slot A of its flash, which
opens the cell first and ends with 13. The reader was a small C program. It found every
fcn_boot process in `/proc` and read the given files of it in a tight loop until a little after
its exec. Starts in which the image found a file still locked:

| fcn_boot | Other load | Before the fix | With the fix |
|---|---|---|---|
| asan | none | 0 of 2000 | |
| asan | a busy loop on each of the 8 CPUs | 0 of 2000 | |
| asan | reader of `stat` and `maps` | 120 of 2000 | 0 of 2000 |
| asan | reader of `stat` only | 40 of 2000 | 0 of 2000 |
| asan | reader of `comm` only | 0 of 2000 | |
| release | reader of `stat` and `maps` | 23 of 2000 | 0 of 2000 |

Three earlier runs of the asan fcn_boot before the fix, whose failures were not yet sorted by
which program found the lock, had 87 of 2000 (reader of `stat` and `maps`), 34 of 2000 (`stat`
only) and 97 of 2000 (`maps` only).

The same holds without chance. `fcn_boot_held` (the regression test below) holds fcn_boot's
old memory map past the exec every time, through a process that shares the map and has no
descriptor of either file, only the map's two shared mappings. Started 500 times on the crash
payload (release build), it failed all 500 starts with the jump of `1bd6024` (`boot_main.c`
without the two releases) and none with the fix.

**Fix.** fcn_boot releases both locks with `flock(LOCK_UN)` directly before `fexecve()`
(`posix_flash_unlock()`, `posix_retained_unlock()`). `LOCK_UN` removes the description's lock
at once, whatever else still refers to the description. It keeps the descriptors (still
close-on-exec) and the mappings. If the exec fails, it takes the cell's lock back without
waiting (`posix_retained_relock()`) before it takes the hand-off back through the mapping.
While the lock is released, `posix_retained` writes nothing into the cell. If another process
has locked the cell in the meantime, fcn_boot writes nothing into it, says that the hand-off
could not be taken back, and ends with 42 as before. Every other program opens and closes the
files as it did.

The window this leaves runs from the release until the image has opened and locked the files
itself. Another program started on the same files at that moment could take one. The image
then fails to open it, as before `1bd6024`, and a hand-off left in a cell that another program
opens is the case described in [plat_retained.h](../platform/include/plat_retained.h) and
[update.md](update.md#the-hand-off) (THE REMAINING WINDOW). This is host only: two programs
started on one cell at the same moment, where a target has one reset vector.

The alternative was to hand the locked descriptors to the image: fcn_boot would clear
close-on-exec, and the image would take the files from descriptors named on its command line
instead of opening them again. That leaves no window at all, but every image (the node and
both payloads) would need a second way into each file, with its own checks, and the image's
command line would change. It would remove a window that only two programs started on one cell
at the same moment can reach. The explicit release costs two calls and keeps the interface.

The fix does not cover a lock held by an image that ends without closing the files: a reset
or a power cut, which end with `_exit()` (`plat_reset()`, the test payloads), or SIGKILL. That
lock also goes only with the image's memory map. (The node's orderly end unmaps and closes
both files itself before it exits.) In the release loop with the reader, 6 of the 2000 starts
found the cell still locked by the payload that had just ended: `fcn_boot: --retained-file ...
is in use by another node`. That was before the fix and again with it. No CI run has shown
this so far.

In a second run of the same loop on the fixed code the image never found a file still locked
by fcn_boot: 3000 starts (asan, reader of `stat` and `maps`), 2000 (asan, `stat` only) and 2000
(release, `stat` and `maps`); in the last, 4 starts found the cell still locked by the payload
that had just ended. The same loop on `1bd6024`, run right after it as a control (asan, `stat`
and `maps`), had the image find the cell locked in 53 of 1000 starts, and fcn_boot after the
payload's end in 1.

**Regression test.** Four platform tests and a system test, none of which depends on the host's
timing:
- `tests/platform/test_posix_retained.c`:
  - `test_a_mapping_keeps_the_lock_after_its_descriptor_is_closed` shows the kernel's side. A
    forked process cannot lock the file while a mapping of a closed descriptor exists, and can
    after the `munmap`.
  - `test_a_lock_released_while_the_cell_stays_open_lets_another_process_take_it` checks the
    release. With the cell still open and mapped, so that its description exists as after the
    exec on CI, a forked process cannot lock the file before `posix_retained_unlock()` and can
    after it. Nothing is written into the cell while the lock is released.
  - `test_the_lock_comes_back_only_while_nobody_else_holds_the_cell` checks the retake. It
    succeeds when nobody else took the cell. It fails while a forked process holds the cell,
    and then the clear, a reset and a new hand-off leave the file unchanged. It succeeds again
    once that process has ended.
- `tests/platform/test_posix_flash.c`:
  `test_a_lock_released_while_the_file_stays_open_lets_another_process_take_it` checks the
  release for the flash.
- `tests/system/test_boot.py`:
  `test_the_image_locks_both_files_while_the_bootloaders_old_memory_map_lives_on` checks that
  fcn_boot itself releases both locks before its exec. The platform tests call the release
  functions directly, so they pass whether or not `boot_main.c` calls them. Here the node is
  started through `fcn_boot_held` ([tests/tools/boot_held.c](../tests/tools/boot_held.c)):
  `boot_main.c` linked with `-Wl,--wrap=fexecve`, whose exec first starts a process that
  shares fcn_boot's memory map (`clone()` with `CLONE_VM`) and holds it until the image has
  ended. That is what a reader of `/proc/<pid>/stat` does by chance for a moment, here every
  time: the old map's mappings of the flash and the cell outlive the exec. The node then has
  to lock the cell and the flash and run to the end of its `--duration-s 1`.

`tests/system/test_boot.py::test_an_image_that_cannot_be_started_leaves_no_hand_off_behind`
still covers a failed exec end to end: the lock is taken back and the hand-off is gone.

**Evidence.** [failing output](buglog/010-image-found-files-still-locked/failing-output.txt),
[patch that re-creates the bug](buglog/010-image-found-files-still-locked/reintroduce.patch).
The patch takes the `flock(LOCK_UN)` out of `posix_retained_unlock()` and
`posix_flash_unlock()`, so that the locks go only with the last reference to their
descriptions, as in `1bd6024`. The bug is in commit `1bd6024`.
