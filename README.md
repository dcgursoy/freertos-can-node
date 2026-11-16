# freertos-can-node

[![ci](https://github.com/dcgursoy/freertos-can-node/actions/workflows/ci.yml/badge.svg)](https://github.com/dcgursoy/freertos-can-node/actions/workflows/ci.yml)

A FreeRTOS telemetry node for one module of a battery pack. When finished it publishes its
measurements on CAN and takes fail-safe A/B firmware updates over the same bus. The node runs
as a Linux process on the FreeRTOS POSIX port, and its CAN controller, flash and sensors are
simulated around it, so that all of it builds, runs and is tested on a laptop and in CI.

> **Work in progress: phases 0 and 1 of 8 are done.** This page is a placeholder until the
> last phase. The sections below describe only what exists today and quote no results; the
> final README will take every number from `results/*.json`.

## Where the project stands

| Phase | Content | State |
|---|---|---|
| 0 | Repository, FreeRTOS kernel, build presets, host platform layer, unit tests, sanitizers, coverage, static analysis, CI | done |
| 1 | CAN stack: CRCs, frame bit timing, controller model with fault confinement, transmit queue, SocketCAN and UDP transports, CAN tasks | done |
| 2 | Application: tasks and IPC, DBC, end-to-end protection, telemetry | next |
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

## What phase 1 adds

The node now has a CAN path from a task to the bus and back. As a stand-in for the telemetry
of phase 2 it sends a status frame every 10 ms and answers a ping frame.
[docs/can-simulation.md](docs/can-simulation.md) describes the simulated bus in full.

- **Transports** ([platform/posix/can_backend.h](platform/posix/can_backend.h)). Linux
  SocketCAN, and a virtual bus over UDP on the loopback interface for machines without the
  `vcan` kernel module. The node and every Python tool select one with the same string
  (`socketcan:vcan0`, `udp:29500`). The UDP bus is also a python-can interface
  ([tools/fcn_tools/udpbus.py](tools/fcn_tools/udpbus.py)).
- **Controller model** ([lib/can](lib/can)). A socket delivers instantly and never fails; a
  bus does neither. The model puts every frame on one timeline with its exact stuffed length
  at the configured bit rate, sends the highest-priority mailbox first, and applies the fault
  confinement rules of the Bosch CAN specification (error counters, error passive, bus off,
  recovery) to faults that the test bench injects as physical conditions: a shorted bus, a
  bus held dominant, a missing acknowledgement.
- **Transmit queue** ([lib/can/can_txq.h](lib/can/can_txq.h)). One latest-value slot per
  cyclic message and a bounded FIFO with a timeout for frames that must not be replaced, with
  exact accounting of every frame, and the bus-off policy: empty the mailboxes, keep
  accepting frames, ask for recovery after a hold-off that backs off. Frames that go stale in
  a mailbox during a fault that never reaches bus off are called back, so what the bus sees
  after any fault is current data.
- **CAN tasks** ([app/can_io.c](app/can_io.c)). A receive task and a transmit task behind one
  interrupt handler, connected to it by task notifications.
- **Simulation control** ([platform/posix/posix_sim.h](platform/posix/posix_sim.h)). Faults are
  applied to the simulated hardware through a channel of its own, the way a test bench works
  on a real device; the application has no test hooks.
- **Tests.** Unit tests of every module of `lib/can` and `lib/crc` against published vectors
  and independent Python implementations; system tests that run every bus test on both
  transports (CI refuses to skip SocketCAN).

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
tests/           unit/ (Unity), platform/ (tests of the host layer), python/ (pytest, tools),
                 system/ (pytest, the node as a process), tools/ (C helpers the tests call)
tools/           Python package: node and bus control, UDP bus for python-can, result files
bench/           measurement scripts; they write results/
results/         measured data (JSON with provenance) and plots; host timing only
scripts/         one script per build, test and analysis step
docs/            the simulated CAN bus, bug log, MISRA deviations, notes on WSL
third_party/     FreeRTOS-Kernel and Unity, pinned submodules
```

## Windows and WSL2

The project is developed on Windows with WSL2. Two things are worth knowing there:

- Keep build trees and the Python environment on the Linux filesystem, not on the Windows
  drive: set `FCN_BUILD_ROOT` and `FCN_VENV` (for example to `~/fcn-build` and `~/.venvs/fcn`)
  before running the scripts.
- The stock WSL2 kernel has no `vcan` module. Everything runs on the UDP bus without it, and
  the SocketCAN half of the tests is skipped; [docs/wsl-vcan.md](docs/wsl-vcan.md) shows how
  to add the module and run that half too.

## Licence

MIT, see [LICENSE](LICENSE). FreeRTOS-Kernel and Unity are MIT-licensed as well.
