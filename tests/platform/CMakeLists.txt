# Self-tests of the POSIX platform layer: small FreeRTOS programs run by CTest.
# Labels: "platform" for all of them, "timing" for those whose verdict depends on the host
# keeping time (CI may repeat those once; see scripts/test-unit.sh).
add_executable(plat_selftest plat_selftest.c)
target_compile_definitions(plat_selftest PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(plat_selftest PRIVATE fcn_platform_posix)
fcn_target_defaults(plat_selftest NO_COVERAGE)

set(_fcn_sanitizer_env
    "ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:detect_leaks=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")

# fcn_add_platform_test(<name> <expected exit status> <labels> <selftest arguments...>)
function(fcn_add_platform_test name expected_status labels)
    string(JOIN " " _args ${ARGN})
    add_test(NAME platform_${name}
             COMMAND sh -c "\"$<TARGET_FILE:plat_selftest>\" ${_args}; test $? -eq ${expected_status}")
    set_tests_properties(platform_${name} PROPERTIES
        LABELS "${labels}"
        TIMEOUT 60
        ENVIRONMENT "${_fcn_sanitizer_env}")
endfunction()

fcn_add_platform_test(tick_locked 0 "platform;timing" tick locked 3)
fcn_add_platform_test(tick_suspended_scheduler 0 "platform;timing" suspend 20)
fcn_add_platform_test(irq_signal 0 "platform" irq signal 250 3 1)
fcn_add_platform_test(irq_poll 0 "platform" irq poll 250 3 1)
# Period 0 is an interrupt storm: the line is raised in a tight loop.
fcn_add_platform_test(irq_storm_signal 0 "platform" irq signal 0 2 1)
fcn_add_platform_test(irq_storm_poll 0 "platform" irq poll 0 2 1)
fcn_add_platform_test(stack 0 "platform" stack)
fcn_add_platform_test(log 0 "platform" log "\"${CMAKE_CURRENT_BINARY_DIR}/selftest_log.txt\"")
fcn_add_platform_test(assert 20 "platform" assert)
fcn_add_platform_test(malloc_failed 22 "platform" malloc-fail)
fcn_add_platform_test(reset 10 "platform" reset)

# An orderly stop on SIGTERM must leave through main() with status 0.
add_test(NAME platform_sigterm
         COMMAND timeout --preserve-status -s TERM 1 $<TARGET_FILE:plat_selftest> run)
set_tests_properties(platform_sigterm PROPERTIES
    LABELS "platform" TIMEOUT 30 ENVIRONMENT "${_fcn_sanitizer_env}")
