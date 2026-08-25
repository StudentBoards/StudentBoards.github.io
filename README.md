# studentboards Board Programmer

Program a studentboards MAX V CPLD board or ATmega32A board from a web page,
using a Raspberry Pi Pico as the programmer. No drivers, no toolchain, no
software to install.

**→ [studentboards.github.io](https://studentboards.github.io/)**

---

## For students

1. Flash the Pico once. The programmer page has a **First time? Set up your
   Pico** panel with the download links and steps, or do it directly: hold
   BOOTSEL while plugging the Pico in, then copy
   `firmware/pico1_programmer.uf2` (or `pico2_programmer.uf2` for a Pico 2)
   onto the `RPI-RP2` drive that appears.
2. Wire the Pico to your board — see below.
3. Open the page in Chrome or Edge, connect, check your board, drop your file.

### Wiring

The CPLD board is programmed over JTAG and the AVR board over ISP or JTAG,
and each interface has its own connector on the Pico. They never conflict, so
you can leave a CPLD board and an AVR board wired at the same time and just
pick the mode on the page.

Pico pin numbers below are *physical* pins, counted from the corner nearest
the USB socket. Board pin numbers are the package pins you can count on the
board itself.

**MAX V CPLD — JTAG (64-pin DIL)**

```
   Raspberry Pi Pico                  MAX V CPLD board
                                      (64-pin DIL package)
   ─────────────────────              ────────────────────
   pin 4   GP2   ────────  TMS  ───▶  pin 14
   pin 5   GP3   ────────  TDI  ───▶  pin 15
   pin 6   GP4   ────────  TCK  ───▶  pin 16
   pin 7   GP5   ◀───────  TDO  ────  pin 17
   pin 3   GND   ────────  GND  ────  GND
```

| Pico pin | Signal | CPLD board pin |
|---|---|---|
| 4 (GP2) | TMS | 14 |
| 5 (GP3) | TDI | 15 |
| 6 (GP4) | TCK | 16 |
| 7 (GP5) | TDO | 17 |
| 3 | GND | GND |

**ATmega32A — ISP (44-pin DIL)**

```
   Raspberry Pi Pico                  ATmega32A board
                                      (44-pin DIL package)
   ─────────────────────              ────────────────────
   pin 9   GP6   ────────  MOSI ───▶  pin 1   (PB5)
   pin 10  GP7   ◀───────  MISO ────  pin 2   (PB6)
   pin 11  GP8   ────────  SCK  ───▶  pin 3   (PB7)
   pin 12  GP9   ────────  RESET ──▶  pin 4
   pin 8   GND   ────────  GND  ────  GND
```

| Pico pin | Signal | AVR board pin |
|---|---|---|
| 9 (GP6) | MOSI | 1 (PB5) |
| 10 (GP7) | MISO | 2 (PB6) |
| 11 (GP8) | SCK | 3 (PB7) |
| 12 (GP9) | RESET | 4 |
| 8 | GND | GND |

Note that most ATmega32A examples online use the DIP-40 pinout, where RESET
is pin 9. On the 44-pin DIL board it is pin 4. The `PBn` names are the same
either way, so go by those if you are cross-checking a datasheet.

**ATmega32A — JTAG (10-pin header)**

The AVR board can also be programmed through its JTAG header, which is
usually easier to reach than the ISP pins. This is a **second, separate JTAG
connector** on the Pico, so a CPLD board and an AVR board can both stay
wired — nothing is swapped to change target.

```
   Raspberry Pi Pico                  JTAG header        ATmega32A
   ─────────────────────              ───────────        ─────────
   pin 14  GP10  ────────  TCK  ───▶  pin 1              PC2
   pin 15  GP11  ◀───────  TDO  ────  pin 3              PC4
   pin 16  GP12  ────────  TMS  ───▶  pin 5              PC3
   pin 17  GP13  ────────  TDI  ───▶  pin 9              PC5
   pin 18  GND   ────────  GND  ────  pin 2 or 10

   Looking at the header, pin 1 marked:

        TCK   1 ● ○  2   GND
        TDO   3 ● ○  4   VTref
        TMS   5 ● ○  6   nSRST
       Vsup   7 ○ ○  8   nTRST
        TDI   9 ● ○ 10   GND
```

| Pico pin | Signal | Header pin | AVR pin |
|---|---|---|---|
| 14 (GP10) | TCK | 1 | PC2 |
| 15 (GP11) | TDO | 3 | PC4 |
| 16 (GP12) | TMS | 5 | PC3 |
| 17 (GP13) | TDI | 9 | PC5 |
| 18 | GND | 2 or 10 | GND |

All four signals are in the odd-numbered row of the header, so Pico pins
14-17 run straight down it in order and skip only pin 7. Prefer header pin 2
for ground — it sits next to TCK and gives the clock the shortest return
path.

**Only five wires are needed.** Leave `VTref`, `Vsupply`, `nSRST` and
`nTRST` unconnected: the datasheet states that programming needs control of
TCK, TMS, TDI and TDO only, and that the reset and clock pins are not
required. `Vsupply` in particular would try to power your board from the
Pico.

Because the clock pin is not needed, JTAG also does not care about the clock
fuses — a board whose fuses select a crystal that is not fitted is silent
over ISP but programs normally over JTAG.

JTAG works only while the **JTAGEN** fuse is programmed, which it is on a
chip from the factory. The *PORTC 2-5 free* fuse preset switches it off to
free those pins; a board set that way has to be programmed over ISP.

One more case to know about: if the program running on the board sets the
**JTD** bit to use PC2-PC5 as ordinary pins, JTAG disappears once that
program runs, even with JTAGEN programmed. Holding the board's RESET line
low while programming clears it, or use ISP.

Firmware before 1.1 has only one JTAG connector. There, the AVR JTAG cable
goes to Pico pins 4-7 instead, and the CPLD board has to come off first.

Always connect ground between the programmer and your board — a shared power
supply is not enough. Both boards must run at 3.3 V; the Pico's pins are not
5 V tolerant.

**Every cable runs straight across.** Pico pins 4-7 go to CPLD board pins
14-17 in order, Pico pins 9-12 go to AVR board pins 1-4 in order, and Pico
pins 14-17 go down the odd-numbered row of the AVR's JTAG header in order.
No crossovers anywhere — if a jumper is diagonal in your photo, it is wrong.

### Making the files

**CPLD** — tell Quartus to write an SVF alongside its usual output, and every
compile from then on produces one:

1. Check the right device is selected under **Assignments → Device** —
   5M40ZE64, 5M80ZE64 or 5M160ZE64, to match the marking on your chip.
2. In that same dialog, go to **Device and Pin Options → Programming Files**
   and tick **Serial Vector Format File (.svf)**.
3. Compile as usual (**Processing → Start Compilation**). The `.svf` lands in
   your project's `output_files` folder next to the `.pof`.

That setting is saved in the project, so it's a one-off.

To produce one by hand from an existing `.pof` instead, `quartus_cpf` does the
same job from the Quartus Prime Command Prompt:

```
quartus_cpf -c -q 12.0MHz -g 3.3 -n p design.pof design.svf
```

The `-q` value sets the clock speed recorded in the file, which the checkbox
method leaves at its default. Either way the programmer never runs faster than
the file declares, so a lower figure only makes programming slower.

**AVR** — use the `.hex` your compiler or IDE produces (not the `.elf`). The
same file works for both the ISP and JTAG modes; the page turns it into an
SVF itself when you pick JTAG.

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
tools/maxv.py                 command-line alternative to the page (CPLD)
tools/avr.py                  command-line alternative to the page (ATmega32A)
tools/avrsvf.py               turns a .hex into an SVF, for the AVR over JTAG
tests/                        host-side tests, no hardware needed
```

The AVR JTAG path adds no firmware driver. `tools/avrsvf.py` (and its twin
inside `index.html`) converts the `.hex` into an SVF, and the Pico plays it
through the same SVF player the CPLD boards use — it never learns it is
talking to an AVR. Both generators are kept byte-identical by
`tests/test_svf_parity.py`.

## Building the firmware

```bash
git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git
export PICO_SDK_PATH=$PWD/pico-sdk
cd firmware
mkdir build && cd build
cmake .. -DPICO_BOARD=pico      # or -DPICO_BOARD=pico2
make -j4
```

Produces `pico_programmer.uf2`. Builds warning-free; roughly 111 KB flash and
38 KB RAM on RP2040 (109 KB / 37 KB on RP2350), comfortable on either Pico.

On Windows, `firmware/build.ps1` does all of the above in one step, using the
SDK and toolchain the Pico VS Code extension installs under
`%USERPROFILE%\.pico-sdk`:

```powershell
cd firmware
.\build.ps1                  # both boards
.\build.ps1 -Board pico      # one board
.\build.ps1 -Board both -Release   # also update the committed .uf2 files
```

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

`tests/run_tests.sh` runs everything, including the two AVR JTAG tests:

- `test_avr_jtag.py` plays a generated SVF into a simulated ATmega32A built
  from the datasheet's programming instruction table, then checks the flash
  it ended up with. It asserts on what the file *does*, not what it says.
- `test_svf_parity.py` lifts the generator out of `index.html`, runs it under
  node, and diffs its output against `tools/avrsvf.py` byte for byte.

Drop a real `.hex` in as `tests/sample.hex` to run the first of those against
your own toolchain's output.

## Serial protocol

The page, `maxv.py` and `avr.py` all speak this over USB CDC at 115200.

There is no AVR-over-JTAG command: that path uses plain `ID` and `SVF` after
pointing `PORT` at the AVR connector, and the firmware never knows what it is
talking to. Which connector is selected persists until it changes or the Pico
resets, so every tool claims the one it wants at the start of a run rather
than assuming — otherwise programming an AVR and then a CPLD would read an
empty socket and blame the wiring. `max_stmt_bytes` in the `INFO` reply
is how the host decides which of the two verify forms it can send; firmware
older than 1.1 omits it, and the host then assumes the smaller limit.

| Command | Reply |
|---|---|
| `PING` | `FIRMWARE <version>` (`PONG <version>` before 1.1) |
| `INFO` | `INFO version= pins= max_shift_bits= max_stmt_bytes= …` |
| `ID` | `IDCODE 0x… <name>` / `ERR NO_TARGET` |
| `SVF <bytes>` | `READY`, then raw bytes, then `DONE` / `ERR` |
| `AVRID` | `SIG 0x…… <name> retries=<n>` / `ERR NO_TARGET` |
| `AVRFUSES` | `FUSES lfuse= hfuse= lock= risk=` |
| `AVRFUSEW <l> <h> [CONFIRM]` | `FUSEOK …` / `ERR FUSE_FATAL` / `ERR FUSE_CONFIRM` |
| `AVRFLASH <bytes>` | `READY`, then raw bytes, then `DONE` / `ERR` |
| `AVRVERIFY <bytes>` | `READY`, then raw bytes, then `DONE match` / `ERR MISMATCH` |
| `PORT [CPLD\|AVR]` | `PORT <name>` / `ERR BAD_PORT` (absent before 1.1) |
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

**The AVR can be programmed over JTAG with no firmware driver.** The host
turns the `.hex` into an SVF and the existing SVF player executes it, so the
AVR JTAG path reuses the JTAG bit-banging, TAP walk, TDO compares and RUNTEST
waits that already program CPLD boards. Its command words come from the
ATmega32A datasheet's Table 27-15; the file layout follows
[avrsvf0](https://github.com/SvenRoederer/avrsvf0) by Andreas Schweizer.

**A fuse write that would switch JTAG off is refused over JTAG.** Unprogramming
JTAGEN takes effect immediately, so the rest of the file would be shifted into
a TAP that had stopped listening. The same value is ordinary over ISP and stays
allowed there.

**SPIEN can't be written from here.** Unprogramming it switches ISP off
permanently, so `avr_fuse_risk()` classes it as fatal with no override. An
external clock selection is a separate, lesser category: it needs explicit
confirmation, because it is correct on a board with a crystal fitted and
recoverable elsewhere by feeding a square wave into XTAL1.

**JTAG needs neither the reset nor the clock pin**, which the datasheet states
outright. So a board whose clock fuses select a crystal that is not fitted —
the most common way to make one of these look dead — is silent over ISP but
programs normally over JTAG, with no square wave into XTAL1 required.

**There are two JTAG connectors**, so a CPLD board and an AVR board can both
stay wired. Which one is active persists in the firmware, so each tool claims
the connector it wants at the start of a run; otherwise programming one board
and then the other would read an empty socket and report a wiring fault on a
board that is correctly connected.

## Known limitations

- MAX V does not encode density in its IDCODE. 5M40ZE64, 5M80ZE64 and
  5M160ZE64 all report `0x020A50DD` and share a package, so they cannot be
  told apart over JTAG. The page reads the target device from the SVF header
  instead.
- Neither AVR path has been run on real hardware yet. The JTAG one at least
  rides on the SVF player that has, but that proves the transport, not the
  AVR side of it.
- Fuses cannot be *read* over JTAG. An SVF can compare a value but never
  report one, so the page's fuse panel is ISP-only. Writing fuses over JTAG
  works and checks the readback (`avr.py --jtag --set-fuses`).
- EEPROM and lock bits are not implemented over JTAG, only flash and fuses.
- A single SIR/SDR shift is capped at `SVF_MAX_BITS` (4096), and a single
  statement at `SVF_MAX_STMT` (1024 bytes). Quartus writes the MAX V CFM in
  pages rather than one large vector, and the AVR's page operations are
  1024 and 1032 bits, so both are comfortable here.
