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
# The codec generated from the DBC (app/gen), reached generically through the generated table
# of tests/tools/can_db_table.h.
fcn_add_unit_test(test_can_db SOURCES test_can_db.c LIBS fcn_can_db_table)
fcn_add_unit_test(test_e2e_p11 SOURCES test_e2e_p11.c LIBS fcn_e2e)
fcn_add_unit_test(test_e2e_sm SOURCES test_e2e_sm.c LIBS fcn_e2e)
fcn_add_unit_test(test_bms SOURCES test_bms.c LIBS fcn_bms)
# The parts of the application that need no kernel (app/signals.c, app/telemetry.c,
# app/command.c).
fcn_add_unit_test(test_signals SOURCES test_signals.c LIBS fcn_app_logic)
fcn_add_unit_test(test_telemetry SOURCES test_telemetry.c LIBS fcn_app_logic)
fcn_add_unit_test(test_command SOURCES test_command.c LIBS fcn_app_logic)
