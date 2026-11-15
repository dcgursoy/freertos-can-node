# Bug log

Real defects that a test caught while this project was being built. Three rules keep the log
honest:

1. An entry exists only if a test actually failed because of the bug. Design changes, review
   findings and tooling problems are not listed here.
2. An entry is written together with the fix, with the output of the failing test saved under
   `docs/buglog/<id>/failing-output.txt`.
3. Every entry carries `reintroduce.patch`, which puts the bug back into the current sources,
   and `test.sh`, which runs the test that caught it. `scripts/buglog-verify.sh` applies each
   patch to a scratch copy, builds, runs the test and requires it to fail. CI runs that script,
   so an entry whose bug cannot be re-created, or whose test stopped noticing it, breaks the
   build.

```bash
scripts/buglog-verify.sh
```

| # | Phase | What was wrong | Caught by |
|---|---|---|---|
| [001](#001-the-stack-report-read-the-stack-of-a-task-that-no-longer-existed) | 0 | The stack report read the idle task's stack after the kernel had deleted the task | system test of the node's statistics |

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
The bug was found and fixed before the repository's first commit, so no commit contains it;
the patch is the way to see it fail.
