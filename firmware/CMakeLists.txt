cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_maxv C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_maxv
    main.c
    jtag.c
    svf.c
    avr.c
)

target_link_libraries(pico_maxv
    pico_stdlib
    hardware_gpio
)

# USB serial on, UART off: the whole point is a single USB cable.
pico_enable_stdio_usb(pico_maxv 1)
pico_enable_stdio_uart(pico_maxv 0)

# Name the device properly in the OS and in the browser's port picker.
# The SDK defaults to "Raspberry Pi" / "Pico", which tells a student
# nothing about which port to choose. Both macros are #ifndef-guarded in
# the SDK, so defining them here overrides cleanly without patching it.
#
# The VID/PID are deliberately left alone: they belong to Raspberry Pi,
# the bootloader and drivers key off them, and the page filters the port
# picker on the VID.
target_compile_definitions(pico_maxv PRIVATE
    USBD_MANUFACTURER="studentboards.dev"
    USBD_PRODUCT="Board Programmer"
)

pico_add_extra_outputs(pico_maxv)
