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
handler needs 2 KiB, and 25 deep at 10 kHz.

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
like every other test. With the fix a storm ends with nesting 1, 2 KiB of stack and no raise
left unserved; `results/platform_timing.json` (`interrupt_storm`) has the measured rates.

**Evidence.** [failing output](buglog/002-nested-interrupt-handlers/failing-output.txt),
[patch that re-creates the bug](buglog/002-nested-interrupt-handlers/reintroduce.patch).
The bug is in commits `323055e` to `68ba365`.
