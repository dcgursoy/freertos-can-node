# The FreeRTOS application: portable code that uses only the FreeRTOS API, lib/ and platform.h.
# The executable that puts it on a platform is defined by that platform (platform/posix).
add_library(fcn_app STATIC
    app.c
    node_stats.c)
target_include_directories(fcn_app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/platform/include")
target_link_libraries(fcn_app PUBLIC freertos_kernel fcn_log)
fcn_target_defaults(fcn_app)
