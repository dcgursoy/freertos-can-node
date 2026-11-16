# Unity unit tests for the portable modules under lib/. One executable per module.
#
# Each test file carries its own main() with an explicit RUN_TEST list. The test functions are
# static, so a test that is written but not listed fails the build (-Wunused-function).
find_package(Threads REQUIRED)

fcn_add_unit_test(test_spsc_ring SOURCES test_spsc_ring.c LIBS fcn_ring Threads::Threads)
fcn_add_unit_test(test_log SOURCES test_log.c LIBS fcn_log)
fcn_add_unit_test(test_crc SOURCES test_crc.c LIBS fcn_crc)
fcn_add_unit_test(test_can_frame SOURCES test_can_frame.c LIBS fcn_can)
fcn_add_unit_test(test_can_bits SOURCES test_can_bits.c LIBS fcn_can)
fcn_add_unit_test(test_can_fc SOURCES test_can_fc.c LIBS fcn_can)
fcn_add_unit_test(test_can_ctrl SOURCES test_can_ctrl.c LIBS fcn_can)
fcn_add_unit_test(test_can_txq SOURCES test_can_txq.c LIBS fcn_can)
