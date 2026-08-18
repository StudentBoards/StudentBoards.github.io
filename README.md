# studentboards Board Programmer

Program a studentboards MAX V CPLD board or ATmega32A board from a web page,
using a Raspberry Pi Pico as the programmer. No drivers, no toolchain, no
software to install.

**→ [studentboards.github.io](https://studentboards.github.io/)**

---

## For students

1. Flash the Pico once: hold BOOTSEL while plugging it in, then copy
   `firmware/pico1_programmer.uf2` (or `pico2_programmer.uf2` for a Pico 2)
   onto the `RPI-RP2` drive that appears.
2. Wire the Pico to your board — see below.
3. Open the page in Chrome or Edge, connect, check your board, drop your file.

### Wiring

The two boards use different interfaces, so each needs its own cable.

**MAX V CPLD — JTAG**

| Pico pin | Signal |
|---|---|
| 3 | GND |
| 4 (GP2) | TCK |
| 5 (GP3) | TMS |
| 6 (GP4) | TDI |
| 7 (GP5) | TDO |

**ATmega32A — ISP**

| Pico pin | Signal |
|---|---|
| 8 | GND |
| 9 (GP6) | SCK |
| 10 (GP7) | MOSI |
| 11 (GP8) | MISO |
| 12 (GP9) | RESET |

Always connect ground between the programmer and your board — a shared power
supply is not enough. Both boards must run at 3.3 V; the Pico's pins are not
5 V tolerant.

### Making the files

**CPLD** — in Quartus, once per build:

```
quartus_cpf -c -q 12.0MHz -g 3.3 -n p design.pof design.svf
```

**AVR** — use the `.hex` your compiler or IDE produces (not the `.elf`).

### Browser support

The page talks to the Pico over Web Serial, which works in Chrome, Edge and
Opera on desktop. Safari does not support it.

---

## Working on this with Claude Code

`CLAUDE.md` in the repo root records the design decisions, the constraints
that aren't visible from the code, and what has and hasn't been tested on
hardware. Claude Code reads it automatically at the start of each session.

## Repository layout

```
index.html                    the programmer page (served by GitHub Pages)
firmware/                     Pico firmware, C + CMake
  main.c                      USB protocol, commands, LED states
  jtag.c/.h                   JTAG TAP driver (MAX V)
  svf.c/.h                    streaming SVF parser
  avr.c/.h                    AVR ISP driver (ATmega32A)
  *.uf2                       prebuilt, ready to flash
tools/maxv.py                 command-line alternative to the web page
tests/                        host-side tests, no hardware needed
```

## Building the firmware

```bash
git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git
export PICO_SDK_PATH=$PWD/pico-sdk
cd firmware
mkdir build && cd build
cmake .. -DPICO_BOARD=pico      # or -DPICO_BOARD=pico2
make -j4
```

Produces `pico_maxv.uf2`. Builds warning-free; roughly 84 KB flash and 38 KB
RAM, comfortable on either Pico.

The two `.uf2` files are **not** interchangeable — they carry different UF2
family IDs (`0xE48BFF56` for RP2040, `0xE48BFF57` for RP2350). Copying the
wrong one is a harmless no-op; the bootloader rejects it.

## Running the tests

No hardware required. The SVF parser runs against a simulated MAX V TAP:

```bash
cd tests
gcc -c -w -I. -I../firmware sim_jtag.c -o sim_jtag.o
gcc -Wall -I. -I../firmware -include shim.h -o test \
    test_main.c ../firmware/svf.c sim_jtag.o shim.c
./test
```

Covers IDCODE and USERCODE readback, MASK handling, wrapped hex vectors, both
comment styles, RUNTEST forms, multi-device chain rejection, oversize shifts,
and byte-at-a-time streaming.

`hex_test.js` (`node hex_test.js`) covers the Intel HEX parser — checksums,
CRLF, sparse addresses, extended-address records, oversize images.
`test_protocol.py` covers the CLI against a fake device.

## Serial protocol

The page and `maxv.py` both speak this over USB CDC at 115200.

| Command | Reply |
|---|---|
| `PING` | `PONG <version>` |
| `INFO` | `INFO version= pins= limits=` |
| `ID` | `IDCODE 0x… <name>` / `ERR NO_TARGET` |
| `SVF <bytes>` | `READY`, then raw bytes, then `DONE` / `ERR` |
| `AVRID` | `SIG 0x…… <name> retries=<n>` / `ERR NO_TARGET` |
| `AVRFUSES` | `FUSES lfuse= hfuse= lock= risk=` |
| `AVRFUSEW <l> <h> [CONFIRM]` | `FUSEOK …` / `ERR FUSE_FATAL` / `ERR FUSE_CONFIRM` |
| `AVRFLASH <bytes>` | `READY`, then raw bytes, then `DONE` / `ERR` |
| `AVRVERIFY <bytes>` | `READY`, then raw bytes, then `DONE match` / `ERR MISMATCH` |
| `SPEED <us>` | `OK` |
| `BOOTSEL` | drops to the UF2 bootloader |

## Design notes

**SVF is streamed, not buffered.** A MAX V SVF runs to a few hundred KB of
ASCII — more than the RP2040's 264 KB of SRAM once USB buffers are accounted
for. The parser executes each statement as it completes, so file size is
unbounded and programming overlaps the upload.

**The board is identified before anything destructive happens.** Both paths
read the device ID first, so an absent board, swapped data pins or a missing
ground is caught in under a second rather than after a chip erase.

**A floating input reads as all-ones**, so both `0xFFFFFFFF` and `0x00000000`
are reported as "no target" rather than as a device with a strange ID.

**AVR detection retries with clock backoff.** ISP needs SCK below a quarter
of the target's clock; a factory ATmega32A on its 1 MHz internal RC caps at
250 kHz. Rather than assume, `avr_isp_enter()` starts fast and slows down
until it syncs, and reports how much retrying it took — a board that only
reads at the slowest setting is flagged as marginal before programming.

**Multi-device JTAG chains are rejected, not ignored.** Non-zero
`HIR`/`HDR`/`TIR`/`TDR` means the SVF targets a chain this player will not pad
for, and continuing would misprogram the device.

**SPIEN can't be written from here.** Unprogramming it switches ISP off
permanently, so `avr_fuse_risk()` classes it as fatal with no override. An
external clock selection is a separate, lesser category: it needs explicit
confirmation, because it is correct on a board with a crystal fitted and
recoverable elsewhere by feeding a square wave into XTAL1.

## Known limitations

- MAX V does not encode density in its IDCODE. 5M40ZE64, 5M80ZE64 and
  5M160ZE64 all report `0x020A50DD` and share a package, so they cannot be
  told apart over JTAG. The page reads the target device from the SVF header
  instead.
- Programming the AVR over its JTAG interface is not implemented. It would
  make a SPIEN-disabled chip recoverable with this same hardware.
- A single SIR/SDR shift is capped at `SVF_MAX_BITS` (4096). Quartus writes
  the MAX V CFM in pages rather than one large vector, so this is comfortable
  for these parts.
