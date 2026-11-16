# Self-tests of the POSIX platform layer: small FreeRTOS programs run by CTest.
# Labels: "platform" for all of them, "timing" for those that can miss a timing tolerance on a
# host that does not run them well. Such a test exits with status 75 and only that status is
# repeated, once, by run_selftest.sh; any other failure counts at once.

# The build id (the commit and, for a tree with uncommitted changes, a hash of those changes)
# is written into a C file on every build, not when CMake configures, so a binary built after
# a commit or an edit carries it (cmake/FcnBuildId.cmake).
set(_fcn_build_id_c "${CMAKE_CURRENT_BINARY_DIR}/fcn_build_id.c")
add_custom_target(fcn_build_id
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DOUTPUT=${_fcn_build_id_c}"
            -P "${CMAKE_SOURCE_DIR}/cmake/FcnBuildId.cmake"
    BYPRODUCTS "${_fcn_build_id_c}"
    COMMENT "Build id of the source tree"
    VERBATIM)

add_executable(plat_selftest plat_selftest.c "${_fcn_build_id_c}")
add_dependencies(plat_selftest fcn_build_id)
target_include_directories(plat_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(plat_selftest PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(plat_selftest PRIVATE fcn_platform_posix)
fcn_target_defaults(plat_selftest NO_COVERAGE)

set(_fcn_sanitizer_env
    "ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:detect_leaks=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
set(_fcn_run_selftest "${CMAKE_CURRENT_SOURCE_DIR}/run_selftest.sh")

# fcn_add_platform_test(<name> STATUS <exit status> LABELS <labels...>
#                       [STDERR <regex>] [FILE <path> <regex>] [TIMEOUT <seconds>]
#                       ARGS <selftest arguments...>)
# STDERR and FILE: a line of stderr, or of the file, has to match the extended regex.
# A test labelled "timing" may be repeated once if it exits with status 75.
function(fcn_add_platform_test name)
    cmake_parse_arguments(ARG "" "STATUS;STDERR;TIMEOUT" "LABELS;FILE;ARGS" ${ARGN})
    set(_options "")
    if("timing" IN_LIST ARG_LABELS)
        list(APPEND _options --retry-timing)
    endif()
    if(DEFINED ARG_STDERR)
        list(APPEND _options --stderr "${ARG_STDERR}")
    endif()
    if(DEFINED ARG_FILE)
        list(APPEND _options --file ${ARG_FILE})
    endif()
    if(NOT DEFINED ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    add_test(NAME platform_${name}
             COMMAND sh "${_fcn_run_selftest}" ${_options} ${ARG_STATUS} --
                     "$<TARGET_FILE:plat_selftest>" ${ARG_ARGS})
    set_tests_properties(platform_${name} PROPERTIES
        LABELS "${ARG_LABELS}"
        TIMEOUT ${ARG_TIMEOUT}
        ENVIRONMENT "${_fcn_sanitizer_env}")
endfunction()

set(_dir "${CMAKE_CURRENT_BINARY_DIR}")

# The tick: locked mode keeps time and drops few ticks; stock mode leaves the tick alone.
fcn_add_platform_test(tick_locked STATUS 0 LABELS platform timing ARGS tick locked 3)
fcn_add_platform_test(tick_stock STATUS 0 LABELS platform ARGS tick stock 1)
fcn_add_platform_test(tick_suspended_scheduler STATUS 0 LABELS platform timing ARGS suspend 20)

# Simulated interrupts from a free-running source, with the load task. "signal-only" switches
# the tick hook's safety net off: the signal path has to serve every interrupt alone. Period 0
# is an interrupt storm: the line is raised in a tight loop. In poll mode the median latency
# has to stay below 0.88 tick, a bound a slow host can miss (status 75): hence "timing".
fcn_add_platform_test(irq_signal STATUS 0 LABELS platform ARGS irq signal 250 3 1)
fcn_add_platform_test(irq_signal_only STATUS 0 LABELS platform ARGS irq signal-only 250 3 1)
fcn_add_platform_test(irq_poll STATUS 0 LABELS platform timing ARGS irq poll 250 3 1)
fcn_add_platform_test(irq_storm_signal STATUS 0 LABELS platform ARGS irq signal 0 2 1)
fcn_add_platform_test(irq_storm_signal_only STATUS 0 LABELS platform ARGS irq signal-only 0 2 1)
fcn_add_platform_test(irq_storm_poll STATUS 0 LABELS platform ARGS irq poll 0 2 1)

# Ping-pong: no interrupt lost, and with signals the task runs within the tick of its ISR.
fcn_add_platform_test(irq_rounds_signal STATUS 0 LABELS platform timing
                      ARGS irq-rounds signal 3000)
fcn_add_platform_test(irq_rounds_signal_only STATUS 0 LABELS platform timing
                      ARGS irq-rounds signal-only 3000)
fcn_add_platform_test(irq_rounds_poll STATUS 0 LABELS platform timing ARGS irq-rounds poll 3000)

# Several lines with their own contexts, singly and together; empty and invalid lines.
fcn_add_platform_test(irq_lines_signal STATUS 0 LABELS platform ARGS irq-lines signal)
fcn_add_platform_test(irq_lines_poll STATUS 0 LABELS platform ARGS irq-lines poll)

fcn_add_platform_test(stack STATUS 0 LABELS platform ARGS stack)
# Both log tests arm the shutdown guard: producers stuck in the log sink end the run with
# status 70 about 5 s later. A ring damaged by two producers at once can also hang the logger
# thread, which runs the guard; then the TIMEOUT ends the test (30 s, 60 s by default).
fcn_add_platform_test(log STATUS 0 LABELS platform ARGS log "${_dir}/selftest_log.txt")
fcn_add_platform_test(log_preempt STATUS 0 LABELS platform TIMEOUT 30
                      ARGS log-preempt "${_dir}/selftest_log_preempt.txt")

# Fatal paths: the documented exit status and the message on stderr.
fcn_add_platform_test(assert STATUS 20 LABELS platform
                      STDERR "^FATAL assertion failed: .*plat_selftest\\.c [0-9]+$" ARGS assert)
fcn_add_platform_test(malloc_failed STATUS 22 LABELS platform
                      STDERR "^FATAL pvPortMalloc failed: free heap bytes [0-9]+$"
                      ARGS malloc-fail)
fcn_add_platform_test(stack_overflow STATUS 21 LABELS platform
                      STDERR "^FATAL stack overflow in task: ovf 0$" ARGS stack-overflow)
fcn_add_platform_test(reset STATUS 10 LABELS platform ARGS reset)
# The line logged right before plat_reset() reaches the file: the reset flushes the log.
fcn_add_platform_test(reset_log STATUS 10 LABELS platform
                      FILE "${_dir}/selftest_reset_log.txt" " I reset: the last line before the reset$"
                      ARGS reset-log "${_dir}/selftest_reset_log.txt")
# A scheduler that never stops is ended by the shutdown guard, armed for 300 ms (the run takes
# about 0.3 s). The TIMEOUT fails a guard that never fires and one that falls back to the
# default POSIX_SHUTDOWN_TIMEOUT_MS of 5 s; a guard that is late by less than about 2.5 s is
# not caught.
fcn_add_platform_test(shutdown_guard STATUS 70 LABELS platform TIMEOUT 3
                      STDERR "^FATAL the scheduler did not stop in time$" ARGS guard)

# An orderly stop on SIGTERM must leave through main() with status 0.
add_test(NAME platform_sigterm
         COMMAND timeout --preserve-status -s TERM 1 $<TARGET_FILE:plat_selftest> run)
set_tests_properties(platform_sigterm PROPERTIES
    LABELS "platform" TIMEOUT 30 ENVIRONMENT "${_fcn_sanitizer_env}")
