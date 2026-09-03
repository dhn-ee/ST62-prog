
add_executable(st62_prog)

pico_generate_pio_header(st62_prog ${CMAKE_CURRENT_LIST_DIR}/st62_prog.pio)

target_sources(st62_prog PRIVATE st62_prog.c)

target_link_libraries(st62_prog PRIVATE pico_stdlib hardware_pio)

target_compile_definitions(st62_prog PRIVATE
    PICO_STDIO_ENABLE_CRLF_SUPPORT=0
)

# disable usb output, enable default uart0 output
pico_enable_stdio_usb(st62_prog 0)
pico_enable_stdio_uart(st62_prog 1)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(st62_prog)

