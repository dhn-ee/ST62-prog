/* ST62/ST63 cpu programmer */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/pio.h"

#include "st62_prog.pio.h"
#include "prom-image.h"

// Pico2 pin assignments
	// stdio UART0 TX=GP0, RX=GP1 (default)
	// GP17=OSCIN
#define PIO_SIDESET_PIN_BASE 17
	// GP13=VPP, 14=TROMIN, 15=RESET_
#define PIO_OUTPUT_PIN_BASE 13
	// GP11=SDOP
#define PIO_INPUT_PIN_BASE 11

  // PROGRAM_PROM, PROGRAM_EEPROM  defined in st62_prog.pio

  // ST62T45B control register addresses (target dependent)
#define DRWR  0xC9
#define DRBR  0xCB
#define EECTL 0xDF


  // DISABLE_CHECKS is only used for initial testing of a new build without a
  // target chip connected.  It bypasses checks for synchronization and EPROM
  // write data match.  Firmware compiled with DISABLE_CHECKS defined and
  // PROGRAM_PROM = 1 will go through a full EPROM programming action so that
  // the interface outputs can be captured with an oscilloscope or logic
  // analyzer to confirm correctness of polarities, VPP level, etc.
// #define DISABLE_CHECKS


// Global variables
static PIO pio = pio0;		// hard code pio 0, sm 0 & sm 1
static uint sm0 = 0;
static uint sm1 = 1;

static uint8_t cur_pc = 0xff;	// current simulated program counter 8 lsbs
static uint8_t exp_pc;		// expected program counter for sync check
static bool checksync_nextcyc = false;	// check sync in next Mcycle call
static int st62_syncfail = 0;		// sync status:  0 in sync, !0 failed
static int dbg_maxpcnt = 0;	// maximum programming attempts for any cell
static int dbg_progfail = 0;	// number of cells that failed 5 prog attempts
static int dbg_idlestat = 0;	// EEPROM write wait success, timeout, syncfail
static int chars_rxed = 0;
static char rxbuf[8192];


// 32-bit data/instruction sent to PIO TXFIFO has 4 parts:
//    [31] T13 clock low stretch enable
// [30:24] zeros (shifted out for cycles T9-T13)
// [23:16] pio 16-bit instruction to set or clear VPP output
//   [7:0] data, opcode, etc. for cycles T1-T8
// #define CLK_STRETCH (1 << 31)

// PIO 16-bit instructions shifted left 8 bits used to set Vpp enable
#define PIO_SET_PINS_0 0xe00000	/* set pins, 0 (RESET_=0, TROMIN=0, VPP=0) */
#define PIO_SET_PINS_1 0xe00100	/* set pins, 1 (RESET_=0, TROMIN=0, VPP=1) */
  // in T8 right after data msb is output to TROMIN, the pio set instruction
  // executes which would set TROMIN to 0 unless it includes the msb
#define VPP0(D) (PIO_SET_PINS_0 | ((D & 0x80)<<2) | D)
#define VPP1(D) (PIO_SET_PINS_1 | ((D & 0x80)<<2) | D)

	// 1 ST62 machine cycle is 13 cycles (T1-T13) of 1.25MHz test clock
	// MUST provide 32-bit data/instruction txdata before next M-cycle to
	// stay synchronized (1560 system clock cycles)
static void __no_inline_not_in_flash_func(st62_Mcycle)(uint32_t txdata,
	uint32_t *rxdata) {
  pio->txf[sm1] = txdata;			// write to sm1 TXFIFO

	// inline pio_sm_is_rx_fifo_empty(pio, sm1)
  while ((pio->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + sm1))) != 0)
    tight_loop_contents();

  *rxdata = pio->rxf[sm1]; // read from sm1 RXFIFO (data from previous Mcycle)

		// when enabled verify program counter 8 lsbs synchronization
  uint8_t real_pc = (uint8_t) *rxdata;		// pc output in previous cycle
  if (checksync_nextcyc && (real_pc != exp_pc)) {
    st62_syncfail = (exp_pc << 16) | real_pc;		// sync mismatch
  }
  checksync_nextcyc = false;
}


static void __not_in_flash_func(st62_setupchecksync)() {
#ifndef DISABLE_CHECKS
  checksync_nextcyc = true;		// setup sync check in next Mcycle
  exp_pc = cur_pc;
#endif
}


	// program 1 EPROM byte within the 64 byte address window 0x40-7f
	// attempt up to 5 times until previous content of EPROM cell before
	// programming matches wdata (each cell programmed minimum 2 times)
static int __no_inline_not_in_flash_func(st62_program_byte)(uint8_t addr,
	uint8_t wdata) {

  uint32_t rxdata;
  int i;
  for (i=0; i<5; i++) {
	// LDI to (0x040-0x07F) behaves differently when programming (VPP on)
    st62_Mcycle(VPP1(0x0d), &rxdata);		// (opcode) LDI addr, wdata
    st62_Mcycle(VPP1(addr), &rxdata);		// address
    st62_setupchecksync();
    st62_Mcycle((1<<31)|VPP1(wdata), &rxdata);	// data; stretch T13 low clk
    if (st62_syncfail)
      return -1;

    sleep_ms(1);
    pio->irq = (1 << 5);			// clear irq5 to resume clock
    // CRITICAL TIMING here: only about 50 cpu cycles between clear irq5 and
    // pio sm1 tries to read TXFIFO data sent by st62_Mcycle()
    st62_Mcycle(VPP1(0x00), &rxdata);
    cur_pc += 3;
    uint8_t prevdat = (uint8_t) rxdata;		// EPROM cell previous value
#ifndef DISABLE_CHECKS
    if (prevdat == wdata)		// previous data matches write data
#else
    if (i == 1)				// special debugging force match
#endif
      return i;
  }
  ++dbg_progfail;				// 5 attempts failed
  return i;
}

	// LDI addr, wdata (VPP off)
static void __no_inline_not_in_flash_func(st62_LDI_vpp0)(uint8_t addr,
	uint8_t wdata){
  uint32_t rxdata;

  st62_Mcycle(VPP0(0x0d), &rxdata);		// (opcode) LDI addr, wdata
  if (st62_syncfail)
    return;
  st62_Mcycle(VPP0(addr), &rxdata);		// address
  st62_setupchecksync();
  st62_Mcycle(VPP0(0x00), &rxdata);
  if (st62_syncfail)
    return;
  st62_Mcycle(VPP0(wdata), &rxdata);		// data
  cur_pc += 3;
}

	// LDI to set Data Rom Window Register C9 (VPP on)
static void __no_inline_not_in_flash_func(st62_LDI_drwr)(uint8_t page){
  uint32_t rxdata;

  st62_Mcycle(VPP1(0x0d), &rxdata);		// (opcode) LDI C9h, page
  if (st62_syncfail)
    return;
  st62_Mcycle(VPP1(DRWR), &rxdata);		// DRWR address
  st62_setupchecksync();
  st62_Mcycle(VPP1(0x00), &rxdata);
  if (st62_syncfail)
    return;
  st62_Mcycle(VPP1(page), &rxdata);		// page address
  cur_pc += 3;
}

	// ST62T45B EPROM memory map (3884 bytes)
	// 080-F7F:  pages 02-3D, window addresses 40-7F
	// F80-F9F:  page  3E, window addresses 40-5F
	// FF0-FF7:  page  3F, window addresses 70-77
	// FFC-FFF:  page  3F, window addresses 7C-7F
static int __no_inline_not_in_flash_func(st62_program_region)(uint16_t first,
	uint16_t last) {

  uint8_t page = first >> 6;
  uint8_t wadr = (first & 0x3f) + 0x40;		// window address (40-7F)
  st62_LDI_drwr(page);
  for (uint16_t romadr=first; romadr<=last; ++romadr) {
    uint8_t wdata = romimg[romadr];
    if (wdata != 0) {
      int pcnt = st62_program_byte(wadr, wdata);
      if (pcnt < 0)
	return -1;				// sync failure
      else if (pcnt > dbg_maxpcnt)
	dbg_maxpcnt = pcnt;
    }
    if (++wadr > 0x7f) {
      wadr = 0x40;
      if (++page < 0x40) {
	st62_LDI_drwr(page);
	gpio_xor_mask(1 << PICO_DEFAULT_LED_PIN);	// toggle led
      }
    }
  }
  return 0;
}

	// optional program and always dump EPROM
static void __no_inline_not_in_flash_func(st62_rdwrPROM)() {
  uint32_t rxdata;

#ifdef PROGRAM_PROM
  st62_Mcycle(VPP1(0x00), &rxdata);		// first NOP (sync), VPP on
  st62_Mcycle(VPP1(0x00), &rxdata);
#else
  st62_Mcycle(VPP0(0x00), &rxdata);		// first NOP (sync)
  st62_Mcycle(VPP0(0x00), &rxdata);
#endif
  st62_setupchecksync();
  cur_pc += 1;
  // cpu pc should now be 0; expect next Mcycle return rxdata=0xfff

#ifdef PROGRAM_PROM
				// VPP rise delay (20us)
  st62_Mcycle(VPP1(0x00), &rxdata);		// NOP	(VPP on)
  st62_Mcycle(VPP1(0x00), &rxdata);
  cur_pc += 1;

  int dbg_pcnt;
  do {
  // change region addresses for your target then comment out #error
#error "Setup EPROM regions for your target"
    dbg_pcnt = st62_program_region(0x080, 0xF9F);
    if (dbg_pcnt < 0) break;
    dbg_pcnt = st62_program_region(0xFF0, 0xFF7);
    if (dbg_pcnt < 0) break;
    dbg_pcnt = st62_program_region(0xFFC, 0xFFF);
  } while (0);

  for (int i=0; i<5; i++) {		// VPP fall delay (104us for 0.1uF cap)
    st62_Mcycle(VPP0(0x00), &rxdata);		// NOP	(turn off VPP)
    if (st62_syncfail)
      return;
    st62_Mcycle(VPP0(0x00), &rxdata);
    st62_setupchecksync();
    cur_pc += 1;
  }

  if (dbg_pcnt < 0)
    return;

#endif

  for (uint32_t page=0x02; page<0x40; page++) {	// dump pages 02-3f
    st62_Mcycle(VPP0(0x0d), &rxdata);		// LDI C9h, page ; DWR=page
    if (st62_syncfail)
      return;
    if (page > 0x02) {
      rxbuf[chars_rxed++] = (uint8_t) rxdata;	// data from last ld prev page
    }
    st62_Mcycle(VPP0(DRWR), &rxdata);
    st62_setupchecksync();
    st62_Mcycle(VPP0(0x00), &rxdata);
    if (st62_syncfail)
      return;

    st62_Mcycle(VPP0(page), &rxdata);
    cur_pc += 3;

    for (uint32_t addr=0x40; addr<0x80; addr++) {
      st62_Mcycle(VPP0(0x1f), &rxdata);		// LD A, addr ; read ROM
      if (addr > 0x40) {
	rxbuf[chars_rxed++] = (uint8_t) rxdata;	// data from previous ld
      }
      st62_Mcycle(VPP0(addr), &rxdata);
      st62_setupchecksync();
      st62_Mcycle(VPP0(0x00), &rxdata);
      if (st62_syncfail)
	return;

      st62_Mcycle(VPP0(0x00), &rxdata);
      cur_pc += 2;
    }
  }

  st62_Mcycle(VPP0(0x00), &rxdata);		// NOP
  rxbuf[chars_rxed++] = (uint8_t) rxdata;	// data from very last ld
  st62_Mcycle(VPP0(0x00), &rxdata);

}


static int __no_inline_not_in_flash_func(st62_EEPidle)() {
  uint32_t rxdata;
			// read EECTL until E2BUSY=0 or timeout after 25ms
  for (int i=0; i<600; ++i) {			// 25ms*1000/(4*13*0.8us)
    st62_Mcycle(VPP0(0x1f), &rxdata);		// (opcode) LD A, EECTL
    // detect idle here in data from previous LD; must finish this LD to
    // stay in sync
    bool idle = (i > 0) && !(rxdata & 0x02);	// true: EEprom not busy
    st62_Mcycle(VPP0(EECTL), &rxdata);
    st62_setupchecksync();
    st62_Mcycle(VPP0(0x00), &rxdata);
    if (st62_syncfail)
      return -1;
    st62_Mcycle(VPP0(0x00), &rxdata);
    cur_pc += 2;
    if (idle)
      return 0;					// finished LD, can return now
  }
  return 1;					// timeout
}

	// write EEprom addresses 00-80
static int __no_inline_not_in_flash_func(st62_writeEE_region)(uint8_t first,
	uint8_t last) {

  st62_LDI_vpp0(EECTL, 0x01);			// enable EEprom writes
  if (st62_syncfail)
    return -1;

  uint8_t page;
  if (first < 0x40)
    page = 0x01;				// EEprom page 0
  else
    page = 0x02;				// EEprom page 1
  st62_LDI_vpp0(DRBR, page);			// set Data Bank to EEprom page
  if (st62_syncfail)
    return -1;

  uint8_t wadr = (first & 0x3f);		// window address (00-3F)
  for (uint8_t imgadr=first; imgadr<=last; ++imgadr) {
    uint8_t wdata = EEpromimg[imgadr];
    if (wdata != 0xff) {
      st62_LDI_vpp0(wadr, wdata);		// write EEprom byte
      if (st62_syncfail)
	return -1;

      dbg_idlestat = st62_EEPidle();		// wait for write to finish
      if (dbg_idlestat != 0)
	return -1;
    }

    if (++wadr > 0x3f) {
      wadr = 0x00;
      if ((page < 0x02) && (imgadr<last)) {
	page = 0x02;
	st62_LDI_vpp0(DRBR, page);		// set Data Bank to EEprom page
	if (st62_syncfail)
	  return -1;
      }
    }
  }

  st62_LDI_vpp0(EECTL, 0x00);			// disable EEprom writes
  if (st62_syncfail)
    return -1;
  else
    return 0;
}


	// program and dump EEPROM
static void __no_inline_not_in_flash_func(st62_rdwrEEPROM)() {
  uint32_t rxdata;

  st62_Mcycle(VPP0(0x00), &rxdata);		// first NOP (sync)
  st62_Mcycle(VPP0(0x00), &rxdata);
  st62_setupchecksync();
  cur_pc += 1;
  // cpu pc should now be 0; expect next Mcycle return rxdata=0xfff

#ifdef PROGRAM_EEPROM
  // change region addresses for your target then comment out #error
#error "Setup EEPROM regions for your target"
  st62_writeEE_region(0x00, 0x3f);
#endif

  uint8_t bank = 0x01;	// Data RAM Bank Register:  select EEPROM page 0
  for (uint32_t page=0; page<2; page++) {	// dump 2 EEPROM pages
    st62_Mcycle(VPP0(0x0d), &rxdata);		// LDI CBh, bank ; DRBR
    if (st62_syncfail)
      return;
    if (page > 0) {
      rxbuf[chars_rxed++] = (uint8_t) rxdata;	// data from last ld prev page
    }
    st62_Mcycle(VPP0(DRBR), &rxdata);
    st62_setupchecksync();
    st62_Mcycle(VPP0(0x00), &rxdata);
    if (st62_syncfail)
      return;

    st62_Mcycle(VPP0(bank), &rxdata);
    cur_pc += 3;

    for (uint32_t addr=0x00; addr<0x40; addr++) {
      st62_Mcycle(VPP0(0x1f), &rxdata); // LD A, addr ; read ROM
      if (addr > 0x00) {
	rxbuf[chars_rxed++] = (uint8_t) rxdata;	// data from previous ld
      }
      st62_Mcycle(VPP0(addr), &rxdata);
      st62_setupchecksync();
      st62_Mcycle(VPP0(0x00), &rxdata);
      if (st62_syncfail)
	return;

      st62_Mcycle(VPP0(0x00), &rxdata);
      cur_pc += 2;
    }
    bank = bank << 1;				// next EEPROM page
  }

  st62_Mcycle(VPP0(0x00), &rxdata);		// NOP
  rxbuf[chars_rxed++] = (uint8_t) rxdata;	// data from very last ld
  st62_Mcycle(VPP0(0x00), &rxdata);
}


	// print out receive (dump) buffer
void print_rxbuf() {
  printf("chars_rxed=%d\r\n", chars_rxed);
  for (int i=0; i<chars_rxed; i++) {
    printf("%02X ", (unsigned char) rxbuf[i] );
    if ((i & 0xf) == 0xf) printf("\r\n");
  }
  printf("\r\n");
}


int main() {
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 1);			// turn on LED

  stdio_init_all();

  uint offsetA = pio_add_program(pio, &onecycle_program);
  hard_assert(offsetA >= 0);

  uint offsetB = pio_add_program(pio, &mainloop_program);
  hard_assert(offsetB >= 0);

  // setup io pins and sm1 registers
  mainloop_program_init(pio, sm1, offsetB, PIO_OUTPUT_PIN_BASE, PIO_INPUT_PIN_BASE);
  // start clock
  onecycle_program_init(pio, sm0, offsetA, PIO_SIDESET_PIN_BASE);

  // TIMING CRITICAL CODE BEGINS with st62_rdrw*() and ends with its return as
  // those routines finish by setting TROMIN=0 for endless NOP execution
  // (sm1 halts but sm0 outputs OSCIN clock forever)

  // can't program both EPROM and EEPROM in same session because of sync checks
#ifndef PROGRAM_EEPROM
  st62_rdwrPROM();
	// successful programming has dbg_maxpcnt=1 and dbg_progfail=0
  printf("dbg_maxpcnt=%d, dbg_progfail=%d\r\n", dbg_maxpcnt, dbg_progfail);
#else
  st62_rdwrEEPROM();
	// successful write has dbg_idlestat=0
  printf("dbg_idlestat=%d\r\n", dbg_idlestat);
#endif

  if (!st62_syncfail)
    print_rxbuf();
  else
    printf("Sync check failed:  expected %02x  got %02x  cur_pc=%02x\r\n",
	   st62_syncfail >> 16, st62_syncfail & 0xff, cur_pc);

  while(true) {};
}

