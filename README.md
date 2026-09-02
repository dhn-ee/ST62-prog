# ST62prog

This project is a programmer for ST62/ST63 cpu internal EPROM/EEPROM controlled by a Raspberry Pi Pico2.  It has sucessfully programmed several ST62T45B in-circuit.

All known previous programmers for these chips were implemented using the PC parallel printer port and proprietary software running on DOS or early Windows and are no longer readily available.

This programmer does not have a GUI or 2-way communication with a PC.  It uses UART serial communication to display output and all actions (dump or program), target cpu config, and the eprom/eeprom images are hard coded at compile time.


## GETTING STARTED

### SOFTWARE
0.  Setup a pico-sdk environment (online tutorials) for a Pico2 board
1.  Configure options in st62_prog.pio:  actions (dump or program) and target cpu TROMIN and SDOP pins active high or low
2.  convert image data for EPROM and EEPROM to C header format and add to prom-image.h
3.  compile firmware 3 times with different options (dump, program EPROM, program EEPROM) and rename output file for each: st62_prog.uf2 -> st62_prog-DUMP.uf2

### HARDWARE
1.  Build the programmer hardware interface from schematic (component choices described in DESIGN.txt).  It's simple enough to build on perfboard so no pcb design provided.
2.  Find your target cpu pin assignments in Table 2 or 3 of the Programming Specification document
3.  After initial tests (see below) without a target cpu, connect 8 wires from the interface to the target or adapter board pins identified in step 2 (header, pogo pins, or solder doesn't matter).  The target cpu must be powered by the programmer or uncontrolled EPROM programming may occur.

After building the interface and configuring/compiling the firmware (with define DISABLE_CHECKS), power the Pico2 without a target cpu connected.  You'll be able to see led behavior and serial output as well as check output signals with an oscilloscope or logic analyzer.  Be sure to comment out DISABLE_CHECKS when done.

The first test with a target cpu connected should be dump EPROM.  A blank cpu will still have some data in the reserved locations.  First programming test should try to program 1-2 bytes in locations that are unused by your application.

Programming 3884 bytes takes 8 seconds and the led will blink but there is no serial output until programming is finished.  Serial output for successful programming will show "dbg_maxpcnt=1 and dbg_progfail=0"

This programmer does not perform a blank check because the reserved areas are target dependent.  The recommended 1st EPROM dump can be blank checked externally.

Blank bytes are 0 therefore this programmer will skip over 0 in the rom image data.  Because of hard real-time requirements (see DESIGN.txt) do not call st62_program_region() with a region that contains many consecutive 0.  Break it up into multiple calls for smaller regions with no consecutive 0.

More details are in DESIGN.txt and SPEC.txt
