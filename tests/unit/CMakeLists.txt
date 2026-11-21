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
fcn_add_unit_test(test_dtc SOURCES test_dtc.c LIBS fcn_dtc)
# The watchdog manager; one test reports checkpoints from several threads (also run under
# ThreadSanitizer by scripts/test-tsan.sh, which selects unit tests that create threads).
fcn_add_unit_test(test_wdgm SOURCES test_wdgm.c LIBS fcn_wdgm Threads::Threads)
# The flash interface on the NOR model, power cuts included.
fcn_add_unit_test(test_flash SOURCES test_flash.c LIBS fcn_flash)
fcn_add_unit_test(test_sha256 SOURCES test_sha256.c LIBS fcn_sha256)
# The image format and the validation of a slot, over a flash device in the test's RAM.
fcn_add_unit_test(test_image SOURCES test_image.c LIBS fcn_image)
# The parts of the application that need no kernel (app/signals.c, app/telemetry.c,
# app/command.c, app/fault.c, app/update.c).
fcn_add_unit_test(test_signals SOURCES test_signals.c LIBS fcn_app_logic)
fcn_add_unit_test(test_telemetry SOURCES test_telemetry.c LIBS fcn_app_logic)
fcn_add_unit_test(test_command SOURCES test_command.c LIBS fcn_app_logic)
fcn_add_unit_test(test_fault SOURCES test_fault.c LIBS fcn_app_logic)
fcn_add_unit_test(test_update SOURCES test_update.c LIBS fcn_app_logic)
# lib/image's validation as lib/boot's validator, over a RAM flash and lib/flash's model.
fcn_add_unit_test(test_bootimg SOURCES test_bootimg.c LIBS fcn_bootimg fcn_flash)
# Boot control and the slot writer (lib/boot), on the tests' own NOR model (boot_test_nor.c,
# behind boot_test_flash.h). test_boot_cuts.c is the power-cut campaign, in three parts that are
# three tests (BOOT_CUTS_PART): the host geometry with an empty log, the host geometry with the
# log filled first, and two other geometries.
set(FCN_BOOT_TEST_SUPPORT boot_test_support.c boot_test_nor.c)
fcn_add_unit_test(test_boot SOURCES test_boot.c ${FCN_BOOT_TEST_SUPPORT} LIBS fcn_boot)
fcn_add_unit_test(test_boot_cuts_host SOURCES test_boot_cuts.c ${FCN_BOOT_TEST_SUPPORT}
                  LIBS fcn_boot)
target_compile_definitions(test_boot_cuts_host PRIVATE BOOT_CUTS_PART=0)
fcn_add_unit_test(test_boot_cuts_host_log SOURCES test_boot_cuts.c ${FCN_BOOT_TEST_SUPPORT}
                  LIBS fcn_boot)
target_compile_definitions(test_boot_cuts_host_log PRIVATE BOOT_CUTS_PART=1)
fcn_add_unit_test(test_boot_cuts_small SOURCES test_boot_cuts.c ${FCN_BOOT_TEST_SUPPORT}
                  LIBS fcn_boot)
target_compile_definitions(test_boot_cuts_small PRIVATE BOOT_CUTS_PART=2)
# The same tests on lib/flash's NOR model (flash/nor.h, through boot_test_flash_lib.c), which
# tears with its own seeded model: lib/boot is shown safe on both models, not only on the one
# written for its tests.
set(FCN_BOOT_TEST_SUPPORT_LIBFLASH boot_test_support.c boot_test_flash_lib.c)
fcn_add_unit_test(test_boot_libflash SOURCES test_boot.c ${FCN_BOOT_TEST_SUPPORT_LIBFLASH}
                  LIBS fcn_boot fcn_flash)
fcn_add_unit_test(test_boot_cuts_host_libflash SOURCES test_boot_cuts.c
                  ${FCN_BOOT_TEST_SUPPORT_LIBFLASH} LIBS fcn_boot fcn_flash)
target_compile_definitions(test_boot_cuts_host_libflash PRIVATE BOOT_CUTS_PART=0)
fcn_add_unit_test(test_boot_cuts_host_log_libflash SOURCES test_boot_cuts.c
                  ${FCN_BOOT_TEST_SUPPORT_LIBFLASH} LIBS fcn_boot fcn_flash)
target_compile_definitions(test_boot_cuts_host_log_libflash PRIVATE BOOT_CUTS_PART=1)
fcn_add_unit_test(test_boot_cuts_small_libflash SOURCES test_boot_cuts.c
                  ${FCN_BOOT_TEST_SUPPORT_LIBFLASH} LIBS fcn_boot fcn_flash)
target_compile_definitions(test_boot_cuts_small_libflash PRIVATE BOOT_CUTS_PART=2)
