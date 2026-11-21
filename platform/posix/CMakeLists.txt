# Host simulation layer under the FreeRTOS POSIX port.
find_package(Threads REQUIRED)

# The parts of the host layer that know nothing about FreeRTOS: the CAN transports (SocketCAN,
# UDP), the simulation control socket, the model of the sensor front end (plat_sensor.h), the
# retained-RAM cell (plat_retained.h) and the flash (plat_flash.h). A library of their own, so
# that their tests, the bootloader and the tools link without a kernel.
add_library(fcn_host_io STATIC
    can_backend.c
    can_backend_socketcan.c
    can_backend_udp.c
    posix_flash.c
    posix_retained.c
    posix_sensor.c
    posix_sim.c)
target_include_directories(fcn_host_io PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/platform/include")
# struct ifreq and the SO_RXQ_OVFL socket option.
target_compile_definitions(fcn_host_io PRIVATE _GNU_SOURCE)
target_link_libraries(fcn_host_io PUBLIC fcn_can fcn_bms fcn_crc fcn_flash)
fcn_target_defaults(fcn_host_io)

# An OBJECT library on purpose: the kernel calls back into this code (hooks, the assert
# handler, the wrapped usleep), and with a static archive whether those definitions get linked
# would depend on the order of the archives on the link line. Objects are always linked whole.
add_library(fcn_platform_posix OBJECT
    posix_can.c
    posix_clock.c
    posix_hooks.c
    posix_irq.c
    posix_lifecycle.c
    posix_log.c
    posix_stack.c
    posix_tick.c
    posix_wdg.c)

target_include_directories(fcn_platform_posix PUBLIC
    "${CMAKE_SOURCE_DIR}/platform/include"
    "${CMAKE_CURRENT_SOURCE_DIR}")

# pthread_getattr_np, pthread_setattr_default_np, pthread_setname_np, clock_nanosleep, ppoll.
target_compile_definitions(fcn_platform_posix PRIVATE _GNU_SOURCE)

target_link_libraries(fcn_platform_posix PUBLIC
    freertos_kernel fcn_host_io fcn_can fcn_ring fcn_log Threads::Threads)

# Two calls of the port are intercepted at link time: the tick thread's sleep (posix_tick.c)
# and the signal mask changes (posix_irq.c). Each file says why.
target_link_options(fcn_platform_posix INTERFACE -Wl,--wrap=usleep -Wl,--wrap=pthread_sigmask)

fcn_target_defaults(fcn_platform_posix)

# What the bootloader and the flash tool share (boot_host.h): no kernel, the host's flash and
# retained cell, lib/boot with lib/image's validation behind it, lib/log into a file.
add_library(fcn_boot_host STATIC boot_host.c)
target_include_directories(fcn_boot_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fcn_boot_host PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(fcn_boot_host PUBLIC fcn_host_io fcn_bootimg fcn_boot fcn_image fcn_log)
fcn_target_defaults(fcn_boot_host)

# The bootloader of the host build (boot_main.c): select, hand-off, and the jump by memfd and
# fexecve. memfd_create and fexecve need _GNU_SOURCE. The targets are named after their
# programs with "_program": fcn_boot and fcn_flash are the libraries of lib/boot and lib/flash.
add_executable(fcn_boot_program boot_main.c)
target_compile_definitions(fcn_boot_program PRIVATE _GNU_SOURCE FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_boot_program PRIVATE fcn_boot_host)
set_target_properties(fcn_boot_program PROPERTIES OUTPUT_NAME fcn_boot
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_boot_program)

# The flash tool (flash_tool.c): factory, stage, info.
add_executable(fcn_flash_program flash_tool.c)
target_compile_definitions(fcn_flash_program PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(fcn_flash_program PRIVATE fcn_boot_host)
set_target_properties(fcn_flash_program PROPERTIES OUTPUT_NAME fcn_flash
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_flash_program)

# The node as a Linux process: the application on top of this platform layer.
add_executable(fcn_node node_main.c)
target_compile_definitions(fcn_node PRIVATE _POSIX_C_SOURCE=200809L
                                            FCN_VERSION="${PROJECT_VERSION}")
target_link_libraries(fcn_node PRIVATE fcn_app fcn_platform_posix)
set_target_properties(fcn_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
fcn_target_defaults(fcn_node)
