# MISRA C:2012: what is checked, and the deviations

**What this is.** The portable code in `lib/` and `app/`, the part that would ship on a
microcontroller, is checked on every run of `scripts/static-analysis.sh` (and so in CI) with
the MISRA C:2012 addon of cppcheck 2.13. A finding outside the deviations below fails the
check.

**What this is not.** It is not a claim of MISRA compliance. The addon implements a subset of
the rules (it has no checks for the directives, nor for rules such as 1.3, 2.1, 8.13, 9.1,
17.5, 18.1 to 18.3 or 22.1 to 22.4), and compliance in the MISRA sense needs a compliance
matrix, a qualified tool and a review process that a portfolio project does not have. The
accurate statement is: *checked with cppcheck's MISRA addon, with the deviations listed
here.*

The host simulation code in `platform/posix` and the tests are not in the MISRA scope: they
are Linux programs built on signals, pthreads and stdio. They are still subject to cppcheck's
regular checks and to clang-tidy.

The deviations live in [config/misra-suppressions.txt](../config/misra-suppressions.txt),
which the analysis reads. This table has to match that file.

| Rule | Category | Scope | Why |
|---|---|---|---|
| 1.4 | required | project | The project is written in C11 on purpose: `<stdatomic.h>` for the lock-free ring between an interrupt and a task, and `_Noreturn` on the reset functions. Rule 1.4 was introduced by Amendment 2, before MISRA had guidance for C11; MISRA C:2023 permits both features under rules of their own. |
| 8.7 | advisory | project | The modules under `lib/` are libraries. Their public functions are called by other modules and by the unit tests, which the analysis of one program configuration does not see, so it reports them as candidates for internal linkage. |
| 8.9 | advisory | project | The statically allocated kernel objects and buffers of a module are declared together at file scope, so that the module's memory budget can be read in one place, rather than as `static` locals of the one function that passes them to the kernel. |
| 15.5 | advisory | project | Early returns are used for argument checks where they make a function shorter and clearer than a single exit with nested conditions. |
| 11.5 | advisory | `lib/ring/spsc_ring.c` | The ring stores elements of a caller-chosen type and, like `memcpy`, takes storage and elements as `void` pointers. They are converted only to `uint8_t *` for copying bytes. |
| 17.1, 21.6 | required | `lib/log/log.c` | A printf-style log call needs `<stdarg.h>` and `vsnprintf`. The logging front end is the only file allowed to use them; it formats into a fixed buffer on the caller's stack and never allocates. |

A local exception that applies to a single line is written next to that line as a
`cppcheck-suppress` comment with its reason. There are none in `lib/` or `app/` today.
