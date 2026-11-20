# Portable modules: pure C11, no operating system calls, no dynamic allocation.
# Each module is a static library fcn_<name>; headers are included as "<name>/<header>.h".
add_subdirectory(ring)
add_subdirectory(log)
add_subdirectory(crc)
add_subdirectory(can)
add_subdirectory(e2e)
add_subdirectory(bms)
add_subdirectory(dtc)
add_subdirectory(wdgm)
