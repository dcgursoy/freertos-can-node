# freertos-can-node

[![ci](https://github.com/dcgursoy/freertos-can-node/actions/workflows/ci.yml/badge.svg)](https://github.com/dcgursoy/freertos-can-node/actions/workflows/ci.yml)

A FreeRTOS telemetry node for one module of a battery pack. When finished it publishes its
measurements on CAN and takes fail-safe A/B firmware updates over the same bus. The node runs
as a Linux process on the FreeRTOS POSIX port, and its CAN controller, flash and sensors are
simulated around it, so that all of it builds, runs and is tested on a laptop and in CI.

> **Work in progress: phases 0 to 4 of 8 are done.** This page is a placeholder until the
> last phase. The sections below describe only what exists today and quote no results; the
> final README will take every number from `results/*.json`.

## Where the project stands

| Phase | Content | State |
|---|---|---|
| 0 | Repository, FreeRTOS kernel, build presets, host platform layer, unit tests, sanitizers, coverage, static analysis, CI | done |
| 1 | CAN stack: CRCs, frame bit timing, controller model with fault confinement, transmit queue, SocketCAN and UDP transports, CAN tasks | done |
| 2 | Application: tasks and IPC, DBC, end-to-end protection, telemetry | done |
| 3 | Fault handling: trouble codes, the fault frame, watchdogs, reset cause | done |
| 4 | Simulated NOR flash, bootloader, A/B update logic | done |
| 5 | ISO-TP, UDS download, end-to-end update | next |
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
    [posix_stack.c](platform/posix/posix_stack.c) measures the real stack instead, counted from
    the top of each task's pthread stack. The node's statistics give each task's peak
    (`stack_peak_bytes`), the part the C library and the port had in use before the task's
    code ran (`stack_start_bytes`), and the stack the task was created with, the size meant
    for the target (`stack_size_bytes`).

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
  ([tools/fcn_tools/udpbus.py](tools/fcn_tools/udpbus.py)). A transport that fails while
  the node runs is counted and logged, and one that is gone, such as a deleted SocketCAN
  interface, stops the node with exit status 74.
- **Controller model** ([lib/can](lib/can)). A socket delivers instantly and intact; a bus
  does neither. The model puts every frame on one timeline with its exact stuffed length at
  the configured bit rate. Whenever the bus becomes free, the node's highest-priority
  mailbox and the oldest frame another node is waiting to send arbitrate, as on a real bus
  (frames of other nodes keep the order the transport delivers them in). The model applies
  the fault confinement rules of the Bosch CAN specification (error counters, error passive, bus off,
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
- **Tests.** Unit tests of every module of `lib/can` and `lib/crc`, each also challenged with
  planted defects; the CRCs and frame lengths are checked against published vectors, and
  `can_bits` against an independent Python implementation on more than 100,000 frames; system
  tests that run every bus test on both transports (CI refuses to skip SocketCAN).

## What phase 2 adds

The node is now a battery-module telemetry node. It measures its module through a sensor front
end, judges and filters the readings, and publishes them on CAN in seven cyclic messages, four
of them protected end to end; a tester can switch the protected telemetry off and on with a
protected command. [docs/architecture.md](docs/architecture.md) describes the tasks and how
they work together, [docs/can-protocol.md](docs/can-protocol.md) the messages and the protection.
No timing of the application has been measured yet; that is phase 6.

- **Messages in a DBC** ([dbc/fcn.dbc](dbc/fcn.dbc)). Every frame the node sends or accepts,
  with cycle times, value tables with "signal not available" codes, and the Data IDs of the
  messages protected end to end. [tools/gen_can_db.py](tools/gen_can_db.py) generates the C
  codec in [app/gen](app/gen) from it: a struct of raw values and a pack/unpack pair per message,
  and factor and offset as exact fractions that the hand-written conversions `_Static_assert`
  on. Before generating, it refuses a DBC with overlapping signals, signals outside the DLC, a
  misplaced E2E header, or Data IDs whose CRCs collide. The generated code is checked in,
  compiled with the project's warnings and checked by the MISRA addon. CI fails when it no
  longer matches the DBC (`scripts/codegen-check.sh`; regenerate with `--update`). The codec is
  tested against cantools on random values for every message, and the generator on signals of
  every length from 1 to 32 bits.
- **End-to-end protection** ([lib/e2e](lib/e2e/e2e_p11.h)). AUTOSAR E2E Profile 11, variant
  11A, and the E2E state machine, written from the AUTOSAR specifications: BMS_PackStatus, the
  two cell-voltage messages and BMS_Temperatures carry a CRC over a Data ID that is never sent
  and a rolling counter. The published test vectors are reproduced, and the C code is compared
  with an independent Python implementation ([tools/fcn_tools/e2e.py](tools/fcn_tools/e2e.py))
  on more than 20,000 requests. Where the specification leaves a case open, the project's
  decision and its reason are in [docs/can-protocol.md](docs/can-protocol.md#end-to-end-protection).
- **Sensor front end** ([docs/sensor-model.md](docs/sensor-model.md)). The node reads its
  module through [plat_sensor.h](platform/include/plat_sensor.h). On the host, a deterministic
  model stands behind it: a 20 s load profile, cell voltages that follow the current through each
  cell's internal resistance, and seeded noise (`--sensor-seed`; the same seed gives the same
  readings, whatever the host's timing). Faults are applied through simulation control as on a
  bench (`sensor.set`, `sensor.open`, `sensor.offline`, `sensor.clear`).
- **Battery-module logic** ([lib/bms](lib/bms/bms.h)). Plausibility of every reading, a
  first-order fixed-point filter per channel with exact rounding and a proof that it cannot
  overflow, and the pack summary and state (STARTUP, NORMAL, FAULT), all in integer arithmetic.
- **The application** ([app/app.c](app/app.c), tasks in [app/app_tasks.h](app/app_tasks.h)). Five
  tasks: `can_rx` (priority 6) for received frames and the command's supervision, `sensor` (5)
  every 10 ms, `process` (4) released by the sensor's samples, `can_tx` (3) below the tasks whose
  frames it sends, and `health` (2) every 100 ms. The sensor task posts samples to a queue; the
  processing task runs lib/bms under a mutex with priority inheritance and publishes the
  protected telemetry; interrupts reach the CAN tasks by task notification. Every kernel object
  of the application is allocated statically. The conversions to the DBC's raw values are in
  one place, [app/signals.c](app/signals.c), with a compile-time check of every factor, offset
  and SNA code they rely on.
- **Commands** ([app/command.c](app/command.c)). The node checks TESTER_Command with Profile 11
  and the E2E state machine, for every frame and for every 100 ms without one, and acts on it
  only while the stream is VALID. CmdTelemetryOn switches the protected telemetry off and on.
- **A schedulability check of the real task set** ([tools/fcn_tools/sched.py](tools/fcn_tools/sched.py)).
  Response-time analysis for fixed priorities, in exact arithmetic, with priority-inheritance
  blocking, release jitter and non-preemptive sections, and the utilisation bounds only where
  their assumptions hold. The node writes its task table into its statistics and a system test checks it; the
  execution-time budgets in it are design figures until phase 6 measures them
  ([docs/architecture.md](docs/architecture.md#schedulability)).
- **Telemetry on the bus, decoded** ([tools/fcn_tools/decode.py](tools/fcn_tools/decode.py)).
  Frames decoded with cantools from the DBC and every protected one checked end to end;
  `python -m fcn_tools.monitor udp:29500` prints them live.
- **Tests.** Unit tests of every new module, each challenged with planted defects. System tests
  on both transports ([tests/system/test_telemetry.py](tests/system/test_telemetry.py)) account
  for every cyclic frame against the node's own statistics, check every protected frame with the
  independent Python implementation, compare the decoded values with the sensor model, apply
  every sensor fault through simulation control and look for it in the next frame, and send
  TESTER_Command frames with a wrong CRC, a repeated or jumping counter, another message's Data
  ID or the wrong length, which the node must count and never act on, and a stream that stops,
  which the node must not trust again until it has proved itself.

## What phase 3 adds

The node now notices when something goes wrong, keeps it as a diagnostic trouble code, tells
the bus, and resets itself when one of its tasks hangs. [docs/faults.md](docs/faults.md)
describes all of it; as in phase 2, no timing of the application has been measured for a
target yet.

- **Diagnostic trouble codes** ([lib/dtc](lib/dtc/dtc.h)). DTCs shaped for UDS, so that phase 5
  can serve them over ReadDTCInformation and ClearDiagnosticInformation: 24-bit DTC numbers,
  all eight bits of the ISO 14229-1 status byte with a written rule for each and every choice
  the norm leaves marked, counter-based debouncing in the style of the AUTOSAR Diagnostic Event
  Manager, the first and most recent failed and confirmed DTC, and reads by status mask. Pure
  C, no globals, the state in the caller's memory. Tested against a reference model written in
  the test from the bit definitions, on random tables and random sequences of reports.
- **The faults** ([app/fault.c](app/fault.c)). Sixteen faults in one table, each with its DTC
  number, its limits, its debouncing and a written reason for every value: cell voltage,
  temperature and current limits with hysteresis, an invalid or silent sensor front end,
  samples that stop coming, bus off and error passive (the fault confinement of phase 1 now
  feeds them), a supervised task that stopped, and a start after a reset caused by a fault.
- **One owner for the DTCs.** The processing task judges every sample where the data is and
  hands the verdict to the health task through a queue; the health task alone changes the DTCs,
  so that the schedulability check of phase 2 still holds for the transmit task
  ([docs/architecture.md](docs/architecture.md#schedulability)).
- **The fault frame** BMS_Fault at identifier 0x010, protected end to end like the telemetry:
  the active and confirmed faults, the most recent one, the warning indicator and the count of
  clears, every 100 ms and in the period of every change. BMS_PackStatus says FAULT while a
  fault is active, and a tester clears the DTCs by changing CmdClearFaultsSeq.
- **Two watchdogs.** A watchdog manager in pure C ([lib/wdgm](lib/wdgm/wdgm.h)), the alive
  supervision of the AUTOSAR Watchdog Manager with its requirement numbers in the code, to
  which four tasks report; and a hardware watchdog modelled on the STM32 IWDG
  ([plat_wdg.h](platform/include/plat_wdg.h)), which the health task kicks only while the
  manager allows it. A task that stops is reported in BMS_Fault before the node is reset; a
  task that keeps the CPU is caught by the hardware watchdog alone.
- **The reset cause** ([plat_retained.h](platform/include/plat_retained.h)). What a
  microcontroller keeps in RAM across a reset is modelled by a cell with a magic, a version and
  a CRC, on the host a file the node maps (`--retained-file`); removing it is a loss of power.
  The next start reports why the last run ended, in its statistics, in BMS_NodeInfo and, after
  a reset caused by a fault, as a DTC. Its reserved words carry phase 4's hand-off from the
  bootloader to the image.
- **A hang from outside.** `sensor.hang on|spin` makes the simulated sensor front end hang, as
  a transfer that never completes would, so that the watchdog tests need no hook in the
  application.
- **Tests.** Unit tests of the new modules, each challenged with planted defects; system tests
  that apply every fault from outside and check its DTC in BMS_Fault, in BMS_PackStatus and in
  the statistics, status byte included, while the fault lasts and after it, with two
  exceptions: SAMPLE_TIMEOUT, during which no sample and so no BMS_PackStatus comes, is checked
  in BMS_PackStatus and in the statistics once the stall has ended and the node has survived
  it, and SUPERVISION_EXPIRED ends in a reset, which writes no statistics and leaves no after;
  a fault that comes and goes within one health period; the clear, ordered against the
  evidence; the watchdog reset and the next start, with and without the retained cell; and
  checks in the test harness that the node of no other system test was ever reset, and that
  in every run that ended in order every kick the health task decided on reached the hardware
  watchdog.

## What phase 4 adds

The node can now be updated without ever being left with nothing to start: two image slots in a
simulated NOR flash, a bootloader that starts a new image as a trial and rolls it back when it
does not prove itself, and metadata that survive a loss of supply in the middle of any write.
The transport is phase 5's: here a tool writes the update, with exactly the calls the UDS
download will make. [docs/update.md](docs/update.md) describes all of it; no timing has been
measured for a target.

- **A NOR flash with tearable writes** ([lib/flash](lib/flash/flash.h)). One device interface
  for the update code, with the rules of a NOR part and one made stricter: a program unit is
  written at most once between two erases, as a flash with an error-correcting code per unit
  requires. Behind it, a model that counts operations and wear, says how long each would take,
  and cuts the supply at a chosen operation with a seeded, reproducible tear: a torn program
  leaves a subset of one unit's transitions done, a torn erase a subset of the sector's bits
  set. Its limits (no weak bits, no retention errors, no read disturb, no suspend) are written
  down. On the host it runs over a file of 2 MiB and 8 KiB (`--flash-file`), each operation
  waits the typical time of a 3 V serial NOR from its datasheet, and a cut is armed on the
  command line (`--flash-cut N[:SEED]`) or at run time (`flash.cut`), as a fault of the
  simulated hardware: the run ends as a loss of supply and the next start finds what the cut
  left.
- **An image format** ([lib/image](lib/image/image.h), [lib/sha256](lib/sha256/sha256.h)). A
  256-byte header with the payload's SHA-256 (FIPS 180-4) and a CRC over the header, validated
  in steps of one 64-byte read, so that a check of a whole slot can be spread over many calls.
  A second implementation in Python ([tools/fcn_tools/image.py](tools/fcn_tools/image.py))
  is held against the C code on thousands of images. Integrity only: the documentation says
  what signatures and anti-rollback would add.
- **Boot control** ([lib/boot](lib/boot/boot.h)). Every state change of an update (adopt,
  begin, commit, attempt, confirm, rollback, reject, fallback) as a record in a log in two
  metadata sectors, with a separate commit field that holds the record's count of zero bits, so
  that every torn write is rejected with certainty, not with a probability; every operation is
  justified against the tear model step by step. A trial gets three attempts, each counted in
  the flash before it starts, so a torn attempt counts against the image and a trial that cuts
  its own supply still runs out of them. The log spreads the erases, so that the slots and not
  the metadata limit the part's endurance. A slot writer in the steps a UDS download makes,
  which erases one sector per call. Tested with a reference model of the state machine and a
  campaign that cuts every operation of every sequence at every unit position, on two flash
  models.
- **A bootloader** (`fcn_boot`, [platform/posix/boot_main.c](platform/posix/boot_main.c)). It
  reads the reset cause, selects the slot, copies the payload into a sealed memory file, checks
  its digest again, writes a hand-off into the retained cell (slot, trial or confirmed, attempt,
  version, the rollback it made) and replaces itself with the copy (`fexecve`), so the test
  harness still sees one process. Phase 7's bootloader on the Cortex-M3 sets VTOR and the
  stack pointer and branches instead.
- **The node's side** ([app/update.h](app/update.h)). A trial confirms itself once it has run
  5 s with the watchdog manager OK, its CAN controller error active and never bus off, and frames
  of its own acknowledged in every 100 ms period (a node alone on the bus stays error passive and
  never goes bus off, so bus off alone would not do); a new lowest-priority task makes every
  write of the node to the flash, so that its waits delay no deadline (task table, schedulability
  check and [docs/architecture.md](docs/architecture.md) updated). BMS_NodeInfo reports the
  active slot, a new message BMS_BootInfo the trial, its attempt and the confirm, and a rollback
  is the DTC UPDATE_ROLLBACK in the first BMS_Fault.
- **A flash tool** (`fcn_flash factory|stage|info`) and a reader of the flash file written from
  the documentation alone ([tools/fcn_tools/flash.py](tools/fcn_tools/flash.py)), which every
  test holds against the tool's own reading.
- **Tests.** Unit tests of every new module, each challenged with planted defects; system tests
  that start the node through the bootloader and watch it from the bus: a factory image, a trial
  that confirms itself and stays, a trial whose frames nobody acknowledges that does not confirm
  itself until they are acknowledged again, trials that crash or hang and are rolled back after
  exactly three starts, a slot with one flipped bit that is never started, an update refused while a
  trial is pending, and power cuts while an update is staged, while the bootloader counts an
  attempt or writes a rollback, and while the node confirms, each followed by a start that must
  be what the state machine says.

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

`scripts/check.sh` runs the checks CI runs: that the code generated from the DBC is current,
static analysis, the debug and release builds, the unit and platform tests under
AddressSanitizer and UndefinedBehaviorSanitizer, the tests of code shared between threads
under ThreadSanitizer (the lock-free ring, the watchdog manager's alive counters, the CAN
driver against the controller's hardware thread, the sensor model read by one thread while
another serves its simulation control commands, and the host's flash, operated by one thread
while another serves `flash.status` and `flash.cut`), coverage of `lib/`, the system tests and the
verification of the bug log. The
individual steps are `scripts/codegen-check.sh`, `scripts/static-analysis.sh`,
`scripts/build.sh`, `scripts/test-unit.sh`, `scripts/test-tsan.sh`, `scripts/coverage.sh`,
`scripts/test-system.sh` and `scripts/buglog-verify.sh`. A test whose verdict depends on the
host keeping time reports a missed timing tolerance on its own, apart from every other failure:
a C self-test exits with status 75, a Python test raises `TimingToleranceMissed`
([tools/fcn_tools/timing.py](tools/fcn_tools/timing.py)). Such a test, and nothing else, is
repeated once. If it misses its tolerance again, `scripts/test-system.sh` exits with status 75
instead of 1, the status the C self-tests use for a missed timing tolerance: either the host
did not keep time or the timing has regressed, and a repetition on an otherwise idle machine
tells them apart. `scripts/buglog-verify.sh` follows the same rule: an entry whose test
missed only timing tolerances twice, with or without the bug, is reported as inconclusive,
and the script exits with 75 if nothing else failed. `scripts/test-system.sh` tests the
`fcn_tools` of the checkout it runs in:
it puts that `tools/` directory on `PYTHONPATH` and stops if Python imports the package from
anywhere else, such as another checkout that the virtual environment's editable install
points to. The ThreadSanitizer runtime of gcc 13 needs address-space
randomisation off on kernels that randomise mmap with more than 28 bits (WSL2 among them);
`scripts/test-tsan.sh` turns it off for the test run with `setarch -R`. CI adds the steps that
belong to the runner: it checks the executable bits in the git index, creates and verifies the
SocketCAN interface `vcan0`, and runs the timing benchmark for the record.

## Repository layout

```
lib/             portable C11 modules: no OS calls, no malloc; covered by the unit tests
app/             the FreeRTOS application: tasks and their IPC (app.c), the conversions to the
                 DBC (signals.c), the frames (telemetry.c), the command receiver (command.c),
                 the faults and their DTCs (fault.c), the node's side of an update (update.c);
                 gen/ is generated from the DBC by tools/gen_can_db.py
dbc/             the CAN messages (fcn.dbc), the source of app/gen/ and of the Python decoders
platform/        include/ is what the application expects from a platform;
                 posix/ implements it on Linux under the FreeRTOS POSIX port, and holds the
                 host's bootloader (fcn_boot) and flash tool (fcn_flash)
config/          FreeRTOSConfig.h, MISRA deviation list
cmake/           the compiler settings every project target gets
tests/           unit/ (Unity), platform/ (tests of the host layer), python/ (pytest, tools),
                 system/ (pytest, the node as a process), tools/ (C helpers the tests call)
tools/           Python package: node and bus control, UDP bus for python-can, decoding and
                 E2E checks, the schedulability check, the image format and the flash file,
                 result files; the codec generator
bench/           measurement scripts; they write results/
results/         measured data (JSON with provenance) and plots; host timing only
scripts/         one script per build, test and analysis step
docs/            the application, the CAN protocol, the simulated CAN bus, the sensor model,
                 fault handling, the update, bug log, MISRA deviations, notes on WSL
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
