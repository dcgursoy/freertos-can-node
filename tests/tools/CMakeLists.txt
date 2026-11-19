# Small command-line programs that expose library functions to the Python tests, so that an
# independent Python implementation can be compared with the C code on many inputs.
add_executable(can_bits_cli can_bits_cli.c)
target_link_libraries(can_bits_cli PRIVATE fcn_can)
set_target_properties(can_bits_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(can_bits_cli NO_COVERAGE)

# The generated CAN codec as a table (can_db_table.h; can_db_table.c is generated with the codec
# by tools/gen_can_db.py). Used by can_db_cli and by tests/unit/test_can_db.c.
add_library(fcn_can_db_table STATIC can_db_table.c)
target_include_directories(fcn_can_db_table PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fcn_can_db_table PUBLIC fcn_can_db)
fcn_target_defaults(fcn_can_db_table NO_COVERAGE)

add_executable(can_db_cli can_db_cli.c)
target_link_libraries(can_db_cli PRIVATE fcn_can_db_table)
set_target_properties(can_db_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(can_db_cli NO_COVERAGE)

add_executable(e2e_cli e2e_cli.c)
target_link_libraries(e2e_cli PRIVATE fcn_e2e)
set_target_properties(e2e_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(e2e_cli NO_COVERAGE)
