# Small command-line programs that expose library functions to the Python tests, so that an
# independent Python implementation can be compared with the C code on many inputs.
add_executable(can_bits_cli can_bits_cli.c)
target_link_libraries(can_bits_cli PRIVATE fcn_can)
set_target_properties(can_bits_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(can_bits_cli NO_COVERAGE)
