# Unity unit tests for the portable modules under lib/. One executable per module.
#
# Each test file carries its own main() with an explicit RUN_TEST list. The test functions are
# static, so a test that is written but not listed fails the build (-Wunused-function).
find_package(Threads REQUIRED)

fcn_add_unit_test(test_spsc_ring SOURCES test_spsc_ring.c LIBS fcn_ring Threads::Threads)
fcn_add_unit_test(test_log SOURCES test_log.c LIBS fcn_log)
