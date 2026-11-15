# Host simulation layer under the FreeRTOS POSIX port.
#
# An OBJECT library on purpose: the kernel calls back into this code (hooks, the assert
# handler, the wrapped usleep), and with a static archive whether those definitions get linked
# would depend on the order of the archives on the link line. Objects are always linked whole.
find_package(Threads REQUIRED)

add_library(fcn_platform_posix OBJECT
    posix_clock.c
    posix_hooks.c
    posix_irq.c
    posix_lifecycle.c
    posix_log.c
    posix_stack.c
    posix_tick.c)

target_include_directories(fcn_platform_posix PUBLIC
    "${CMAKE_SOURCE_DIR}/platform/include"
    "${CMAKE_CURRENT_SOURCE_DIR}")

# pthread_getattr_np, pthread_setattr_default_np, pthread_setname_np and clock_nanosleep.
target_compile_definitions(fcn_platform_posix PRIVATE _GNU_SOURCE)

target_link_libraries(fcn_platform_posix PUBLIC freertos_kernel fcn_ring fcn_log Threads::Threads)

# Route the port's tick-thread sleep through platform/posix/posix_tick.c (see that file).
target_link_options(fcn_platform_posix INTERFACE -Wl,--wrap=usleep)

fcn_target_defaults(fcn_platform_posix)

# The node as a Linux process: the application on top of this platform layer.
add_executable(fcn_node node_main.c)
target_compile_definitions(fcn_node PRIVATE _POSIX_C_SOURCE=200809L
                                            FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_node PRIVATE fcn_app fcn_platform_posix)
set_target_properties(fcn_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_node)
