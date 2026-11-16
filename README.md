# freertos-can-node

[![ci](https://github.com/dcgursoy/freertos-can-node/actions/workflows/ci.yml/badge.svg)](https://github.com/dcgursoy/freertos-can-node/actions/workflows/ci.yml)

A FreeRTOS telemetry node for one module of a battery pack. When finished it publishes its
measurements on CAN and takes fail-safe A/B firmware updates over the same bus. The node runs
as a Linux process on the FreeRTOS POSIX port, and its CAN controller, flash and sensors are
simulated around it, so that all of it builds, runs and is tested on a laptop and in CI.

> **Work in progress: phase 0 of 8 is done.** This page is a placeholder until the last
> phase. The sections below describe only what exists today and quote no results; the final
> README will take every number from `results/*.json`.

## Where the project stands

| Phase | Content | State |
|---|---|---|
| 0 | Repository, FreeRTOS kernel, build presets, host platform layer, unit tests, sanitizers, coverage, static analysis, CI | done |
| 1 | CAN stack: CRCs, frame bit timing, controller model, SocketCAN and UDP backends | next |
| 2 | Application: tasks and IPC, DBC, end-to-end protection, telemetry | |
| 3 | Fault handling: trouble codes, watchdogs, bus-off recovery | |
| 4 | Simulated NOR flash, bootloader, A/B update logic | |
| 5 | ISO-TP, UDS download, end-to-end update | |
| 6 | Measurements and plots | |
| 7 | Cortex-M3 build in QEMU with a real bootloader jump | |
| 8 | Final README and documentation | |

## What phase 0 contains

- **Kernel.** [FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel) V11.3.3 as a
  pinned, unmodified submodule, built for its POSIX port with CMake. Project code is C11
  with `-Wall -Wextra -Werror` and a stricter set on top ([cmake/FcnHelpers.cmake](cmake/FcnHelpers.cmake)).
  `configASSERT`, the stack-overflow and malloc-failed hooks, run-time statistics and the
  trace facility are on ([config/posix/FreeRTOSConfig.h](config/posix/FreeRTOSConfig.h)).
- **Host platform layer** ([platform/posix](platform/posix)). On the POSIX port every task is
  a thread of a Linux process. The layer removes three systematic errors of that port:
  - its tick comes from a thread that sleeps a relative period, so FreeRTOS time runs slow
    against the wall clock. [posix_tick.c](platform/posix/posix_tick.c) locks the tick count
    to `CLOCK_MONOTONIC` at link time, without touching the kernel;
  - a thread outside the scheduler may not call the kernel, so models of peripherals reach
    tasks through lock-free rings and a simulated interrupt ([posix_irq.c](platform/posix/posix_irq.c));
  - tasks run on ordinary pthread stacks, so the kernel's stack high-water mark sees nothing.
    [posix_stack.c](platform/posix/posix_stack.c) measures the real stack instead.

  **It does not turn Linux into a real-time system.** When a thread runs is still decided by
  the host's scheduler. Tick jitter, dropped ticks, wake-up times and interrupt latency in
  any result of this project are those of the host (WSL2 on the development laptop, a shared
  runner in CI) and say nothing about the same code on a microcontroller. What does carry
  over is everything counted rather than timed: orderings, state machines, protocol content.
- **Tests.** Unity unit tests for the portable code in [lib/](lib), platform self-tests that
  run small FreeRTOS programs ([tests/platform](tests/platform)), and pytest system tests that
  start the node and watch it from outside ([tests/system](tests/system)).
- **Static analysis.** clang-format, cppcheck, cppcheck's MISRA C:2012 addon on the portable
  code with [documented deviations](docs/misra-deviations.md), and clang-tidy. Every run also
  sends a file of planted defects through the same commands, so "no findings" is a checked
  statement.
- **Bug log.** [docs/bug-log.md](docs/bug-log.md) lists real bugs that a test caught. Each
  entry has a patch that re-creates the bug; CI checks that the entry's tests pass without
  the patch and fail with it.
- **First measurement.** [results/platform_timing.json](results/platform_timing.json) and
  [the plot next to it](results/platform_timing.png), written by
  [bench/platform_timing.py](bench/platform_timing.py): tick accuracy and interrupt latency
  of the host platform layer on the development laptop. Host timing, as said above.

## Build and test

Ubuntu 24.04 (natively, or under WSL2):

```bash
sudo apt-get install build-essential cmake ninja-build python3-venv can-utils cppcheck clang-tidy clang-format
```

```bash
git clone --recurse-submodules https://github.com/dcgursoy/freertos-can-node.git
```

```bash
cd freertos-can-node && scripts/setup-python.sh
```

```bash
scripts/check.sh
```

`scripts/check.sh` runs the checks CI runs: static analysis, the debug and release builds,
the unit and platform tests under AddressSanitizer and UndefinedBehaviorSanitizer, the unit
tests that start threads under ThreadSanitizer, coverage of `lib/`, the system tests and the
verification of the bug log. The individual steps are `scripts/static-analysis.sh`,
`scripts/build.sh`, `scripts/test-unit.sh`, `scripts/test-tsan.sh`, `scripts/coverage.sh`,
`scripts/test-system.sh` and `scripts/buglog-verify.sh`. A test whose verdict depends on the
host keeping time exits with status 75 when only a timing tolerance was missed; that status,
and nothing else, is repeated once. The ThreadSanitizer runtime of gcc 13 needs address-space
randomisation off on kernels that randomise mmap with more than 28 bits (WSL2 among them);
`scripts/test-tsan.sh` turns it off for the test run with `setarch -R`. CI adds the steps that
belong to the runner: it checks the executable bits in the git index, creates and verifies the
SocketCAN interface `vcan0`, and runs the timing benchmark for the record.

## Repository layout

```
lib/             portable C11 modules: no OS calls, no malloc; covered by the unit tests
app/             the FreeRTOS application: tasks and their IPC
platform/        include/ is what the application expects from a platform;
                 posix/ implements it on Linux under the FreeRTOS POSIX port
config/          FreeRTOSConfig.h, MISRA deviation list
cmake/           the compiler settings every project target gets
tests/           unit/ (Unity), platform/ (self-tests of the host layer), system/ (pytest)
tools/           Python package: process control, result files, plot style
bench/           measurement scripts; they write results/
results/         measured data (JSON with provenance) and plots; host timing only
scripts/         one script per build, test and analysis step
docs/            bug log, MISRA deviations, notes on WSL
third_party/     FreeRTOS-Kernel and Unity, pinned submodules
```

## Windows and WSL2

The project is developed on Windows with WSL2. Two things are worth knowing there:

- Keep build trees and the Python environment on the Linux filesystem, not on the Windows
  drive: set `FCN_BUILD_ROOT` and `FCN_VENV` (for example to `~/fcn-build` and `~/.venvs/fcn`)
  before running the scripts.
- The stock WSL2 kernel has no `vcan` module. Nothing needs it yet;
  [docs/wsl-vcan.md](docs/wsl-vcan.md) shows how to add it for the SocketCAN transport that
  phase 1 brings.

## Licence

MIT, see [LICENSE](LICENSE). FreeRTOS-Kernel and Unity are MIT-licensed as well.
