# CLAUDE.md

Context for working on this repository. Read before changing anything.

## What this is

A Raspberry Pi Pico acting as a programmer for two studentboards products,
driven from a web page over Web Serial. Students plug in a Pico, open the
page, and drop a file. No drivers, no toolchain, no install.

- **MAX V CPLD boards** (5M40ZE64 / 5M80ZE64 / 5M160ZE64) — programmed over
  **JTAG** from a Quartus `.svf`.
- **ATmega32A boards** — programmed over **ISP** from an Intel `.hex`, or
  over **JTAG** from the same `.hex` turned into an SVF on the host.

The audience is first-year engineering students. Error messages are part of
the product: a student who cannot work out why their board did not program
becomes a support email. Prefer "check these three things, in this order"
over an error code.

## Status

**The CPLD path is proven on hardware.** A 5M80ZE64 programmed successfully
from a real Quartus SVF: 11,595 statements, 123,611 bits, ~8 seconds, with
all 3,845 of the file's TDO verify vectors passing. That is the device
confirming its own responses thousands of times, not just an absence of
errors.

**Confirmed again on 2026-08-22 with the board-matched pin order** (GP2/3/4/5
= TMS/TDI/TCK/TDO, see below), running the release `.uf2` rebuilt against
SDK 2.3.0. The reorder is verified on hardware, not merely plausible. That
run was a pass/fail check, though, not a repeat of the vector-by-vector
figures above.

**Confirmed a third time on 2026-08-25, after `jtag.c` gained two ports.**
The same 5M80Z file replayed identically — `DONE statements=11595
bits=123611` — with all four `DIAG` checks passing (IDCODE, IRCAPTURE
0x155, IR length 10, BYPASS single-device). Those use different registers
and instructions, so their agreement is what makes this more than one
lucky run.

It took **10.8 s against the ~8 s measured before the refactor**, and that
slowdown is real and expected. The pin masks and the TAP state moved from
compile-time constants to fields reached through the active-port pointer,
so the bit-bang loop now does a few loads per edge where it used to use
immediates — and with over 99% of all clocks coming from `RUNTEST`, any
per-clock cost shows up directly in the total. Nothing about correctness
changes: running slower stretches those flash waits, which is the safe
direction, and the free-running rate is still above the 2.8 MHz `svf.c`
assumes when clamping a declared frequency. Worth recovering only if a
programming run ever feels slow, and not worth destabilising a proven
bit-bang loop for 2.8 s.

**Both boards were wired at once throughout.** 24 interleaved port
switches all read the correct IDCODE, the selection survived unrelated
commands, and after the 10-second CPLD programming run the AVR still
verified over both ISP and JTAG. There is no cross-talk between the three
connectors.

**The AVR ISP path is proven on hardware** as of 2026-08-25. A real
ATmega32A (signature 0x1E9502, silicon revision 10) was erased,
programmed and verified from a 790-byte Intel HEX: `DONE bytes=790
pages=7 ms=233`, then confirmed by an independent read-back over the
*JTAG* interface. Two different programming interfaces agreeing on the
flash contents is worth more than either passing alone.

The first bench run did find something, exactly as this section predicted
it would — see "no POLL RDY/BSY" below. The failure was silent: every
page programmed as 0xFF and the verify reported `mismatch at byte 0`,
which reads like a wiring fault and was entirely a firmware bug.

**The AVR JTAG path is also proven on hardware**, 2026-08-25, on the
second JTAG connector (GP10-13). The same ATmega32A was chip-erased,
programmed and verified entirely from a generated SVF — `DONE
statements=148 bits=15716 ms=156` — and the result was then confirmed by
an independent read-back over ISP. Because the generated file erases
before it writes, a passing run proves the write happened rather than
merely matching what was already there.

Its IDCODE reads `0xA950203F`: part 0x9502, manufacturer 0x01F, and
silicon **revision 10** in the top nibble. A naive full-word IDCODE
compare would have rejected this chip outright, so the version-nibble
masking is load-bearing rather than theoretical.

It needed no new firmware driver: the host turns the `.hex` into an SVF
and the existing SVF player executes it.

What was checked before any of that, without hardware:

- Every 15-bit command word was read off the ATmega32A's own datasheet
  (DS40002072A, Table 27-15), not carried over from the ATmega16.
- The generated write and verify vectors are byte-for-byte identical to
  avrsvf0's committed reference output for the same image (`test_tc1.ref`,
  62 page-load vectors and 62 page-read TDO/MASK vectors). The two tools
  agree on the parts where agreement is meaningful.
- `tests/test_avr_jtag.py` plays the generated file into a simulated
  ATmega32A built from the datasheet's instruction table and then checks
  the resulting flash, rather than checking the text of the file.

Note that avrsvf0's own `make test` does **not** pass at the commit this
was ported from: `do_flash` in its `main.c` is read but never assigned, so
the tool emits no flash data at all. Its `.ref` files predate that and are
still good. Do not "fix" this port to match the current avrsvf0 binary.

Treat bug reports from real hardware as more authoritative than anything in
the code comments, including these. Both bugs found so far
(`ENDIR`/`ENDDR` conflation and the RDY/BSY poll, see below) passed every
host-side test and all four `DIAG` checks before hardware caught them.

## Layout

```
index.html          the whole web app: one file, no build step, no deps
firmware/           Pico firmware (C, CMake, Pico SDK)
  main.c            USB CDC protocol, command dispatch, LED states
  jtag.c/.h         JTAG TAP driver — MAX V
  svf.c/.h          streaming SVF parser
  avr.c/.h          AVR ISP driver — ATmega32A
  build.ps1         Windows build wrapper; -Release updates the .uf2 files
tools/maxv.py       CLI alternative to the page, same serial protocol (CPLD)
tools/avr.py        the same for the ATmega32A: ISP, or --jtag
tools/avrsvf.py     HEX -> SVF for the AVR over JTAG. No firmware involved
tests/              host-side tests, no hardware needed
```

There are deliberately **two** copies of the HEX-to-SVF generator: the
`AVRSVF` block in `index.html` and `tools/avrsvf.py`. The page cannot
import the Python and the CLI must not need a browser, so parity is kept
by test instead: `tests/test_svf_parity.py` lifts the JS straight out of
`index.html`, runs it under node, and diffs its output against the Python
byte for byte. Change one, change the other, or that test fails — which is
the point, because a page that programs a board slightly differently from
the command line is a bug nobody reports.

## Decisions that look wrong but are not

Each of these was made deliberately. Change them only with a reason that
addresses the point, not because they look like oversights.

**SVF is streamed, never buffered.** A MAX V SVF is a few hundred KB of
ASCII — more than the RP2040's 264 KB of SRAM once USB buffers are counted.
`svf_feed()` executes statements as they complete and holds at most one.
Do not "simplify" this into reading the file into an array; it will work on
a Pico 2 and fail on a Pico 1.

**The AVR image *is* buffered**, in contrast — 32 KB is small, and buffering
lets us erase once, write every page, then verify the whole image against
the same copy. The inconsistency with the SVF path is intentional.

**ISP clock speed auto-negotiates.** ISP requires SCK below a quarter of the
target's clock; a factory ATmega32A on its 1 MHz internal RC caps at 250 kHz.
`avr_isp_enter()` starts fast and backs off through a ladder of delays.
A fixed fast clock is the single most common reason ISP "doesn't work" on a
new chip. `avr_read_signature_stable()` does the same and requires two
consecutive matching reads — one read can return plausible garbage.

**Device ID is read before anything destructive.** Both paths identify the
target first. Catches an absent board, swapped data pins, or a missing ground
in under a second instead of after a chip erase.

**Floating inputs read as all-ones**, so `0xFFFFFFFF` and `0x00000000` are
both reported as "no target" rather than as a device with an odd ID. There
are deliberate pull-ups on TDO and MISO to make this reading stable.

**Multi-device JTAG chains are rejected, not ignored.** Non-zero
`HIR`/`HDR`/`TIR`/`TDR` means the SVF targets a chain this parser will not
pad for. Silently continuing would misprogram the part.

**All bytes of a transfer are consumed even after an error.** Otherwise the
host's byte count and the firmware's diverge and the next command lands
mid-file. See `cmd_svf()`.

**`RUNTEST` honours both TCK counts and `SEC` minimums.** On MAX V these are
flash erase and program waits. Overshooting is harmless; undershooting
corrupts the cycle.

**`FREQUENCY` only ever slows the clock down, never speeds it up.** Those
`RUNTEST` waits are TCK counts Quartus computed from the frequency declared
at the top of the SVF — in a real file they are over 99% of all clocks
(33,992,706 `RUNTEST` clocks against 123,611 shift bits, measured). So the
declared frequency is the unit those delays are denominated in. Running
slower stretches them (harmless); running faster shortens them below what
the silicon needs and truncates a flash write, which fails intermittently
and looks like flaky hardware. `svf.c` therefore clamps
`jtag_edge_delay_us` up from the ~2.8 MHz free-running rate when a file
declares less, does nothing when it declares more, and always rounds toward
slower. Current Quartus output declares 12–18 MHz, so in practice no
slowdown is applied — this is insurance, not a fix for a current failure.
Clamp in `double` before the cast: an out-of-range `double`→`uint32_t`
conversion is undefined and was observed wrapping to 0, i.e. full speed,
the exact opposite of the intent. `tests/test_main.c` covers all three
cases and the saturation one fails against the unclamped version.

**`ENDIR` and `ENDDR` are matched by full string, not by a character index.**
They share `E-N-D` and differ only at index 3. An earlier version tested
`cmd[2]`, so `ENDIR` set `end_dr`: every `SDR` then ended in Pause-IR, and
the next `SDR`'s route back to Shift-DR passed through Update-IR, latching
the Capture-IR pattern over the real instruction. The symptom was that the
first shift after any `SIR` worked and the second returned a 1-bit register —
which looks exactly like a hardware fault. `tests/test_main.c` has a
regression test; it fails against the old code, which was verified.

**AVR-over-JTAG is generated on the host, not driven by firmware.** There
is no `avr_jtag.c`. `cmd_svf()` never looks at the IDCODE, so an SVF built
from a `.hex` programs an ATmega32A through exactly the code path that
programs a CPLD. This is why the feature works on a Pico whose firmware
knows nothing about AVRs, and why it reuses the only part of this project
that has been proven on silicon. Do not "promote" it into a firmware
driver; that would throw away both properties and add a second bit-banger
to keep correct.

**The generated SVF checks IDCODE and signature before erasing**, which is
the one place this port deliberately departs from avrsvf0's ordering —
avrsvf0 erases first, then reads the signature. Same principle as the ISP
path: identify before doing anything irreversible. `tests/test_avr_jtag.py`
asserts that a wrong signature leaves the chip unerased.

**Flash verify has two forms, and the host asks which one fits.** A
PROG_PAGEREAD compare covers a whole 128-byte page in one statement, but
that statement is 537 bytes — TDO and MASK vectors of 258 hex digits each
— and SVF has no way to express half a compare. Firmware before 1.1 capped
a statement at 512. So `INFO` now reports `max_stmt_bytes`, and the host
picks the compact page form only when it fits, falling back to a
word-at-a-time verify that costs roughly ten times the bytes over USB but
runs on any firmware. Absence of the field means old firmware, not a large
buffer — guessing high would fail partway through a file with a `TOO_LONG`
that reads like a corrupt upload.

**TDI is omitted from the page-read statement.** avrsvf0 spends 258 hex
digits on all-ones there. PROG_PAGEREAD is a virtual read chain and
nothing is shifted in, so those digits buy nothing and would push the
statement to ~800 bytes. This is the one field-level difference from the
reference output, and it is why 537 rather than 802 is the number the
firmware had to grow to accept.

**A fuse write that unprograms JTAGEN is refused over JTAG, with no
override.** JTAGEN is hfuse bit 6, and the `8mhz-freeportc` preset (`0xD9`)
sets it — that preset exists precisely to free PORTC 2-5 by switching the
JTAG interface off. Written over JTAG it takes effect immediately and the
rest of the file is shifted into a TAP that has stopped listening. Over
ISP the same value is ordinary and stays allowed, so this check lives in
the SVF generator rather than in `avr_fuse_risk()`. That keeps the
firmware's three-level model exactly as it was.

**The ATmega32A has no POLL RDY/BSY instruction over SPI, so writes wait a
fixed time.** This is the bug that made the ISP path fail on its first
bench run, and it is worth understanding before anyone "improves" the
delays back into a poll.

`wait_ready()` used to send `0xF0 0x00 0x00 0x00` and return as soon as
the reply's low bit was clear. That opcode is not in Table 27-14 — the bit
pattern `1111 0000` appears *nowhere* in DS40002072A — and every RDY/BSY
in that datasheet is the physical pin on the **parallel** programming
interface, not something reachable over SPI. It belongs to later AVRs
(ATmega48/88/168/328). This part simply does not implement it, so the poll
was reading an undefined reply and returning immediately.

The symptom was not slowness. The next page's load commands began while
the previous page was still being written, and section 27.9.1 is explicit
that "accessing the SPI Serial Programming interface before the Flash
write operation completes can result in incorrect programming". Every page
was disturbed, the whole image stayed 0xFF, and the verify failed at byte
0 — indistinguishable from a wiring fault, and the reported reason for
weeks. `avr_write_page()` and `avr_chip_erase()` now sleep tWD_FLASH and
tWD_ERASE (Table 27-13: 4.5 ms and 9.0 ms) with roughly double margin.

The fuse path had already been patched around this in a previous session
without the cause being identified — its comment claimed polling was
"documented for the Flash/EEPROM page path", which was never true. That
comment is corrected in place. If a wait here ever needs shortening, find
a poll instruction in *this part's* datasheet first; there is not one.

**`AVRID` reports the negotiated SCK, not a retry count, and falling back
one rung is not a fault.** ISP requires SCK below a quarter of the target's
clock. `spi_byte()` spends two `busy_wait_us(avr_sck_delay_us)` per bit, so
the ladder `{1, 5, 20, 50}` us gives roughly **492, 99, 24 and 9 kHz** —
measured on hardware, not computed: a 24,576-byte bulk read (786,432 SCK
cycles) took 1594 ms at the fastest rung, i.e. 493 kHz.

A factory ATmega32A runs from its 1 MHz internal RC, which caps SCK at
250 kHz. So the fastest rung is about twice what it can take, it *always*
fails there, and it *always* settles at ~99 kHz. That is the part working
exactly as specified. The old reply said `retries=1` and the page turned
any non-zero value into "Weak connection — shorten your jumper wires",
which accused every brand-new board of bad wiring. Confirmed on hardware:
the same board reports 492 kHz with 8 MHz fuses and 99 kHz with 1 MHz
fuses, and programs correctly at both.

Hence `sck=<n>kHz`. Hosts warn only below 50 kHz, because no clock option
on this part requires going that slow, so getting there does implicate the
link. Do not restore a warning on "it used a slower rung" — that is the
negotiation succeeding.

The ladder deliberately still starts fast rather than starting safe for
1 MHz: a crystal or 8 MHz board then programs at ~492 kHz instead of
~99 kHz, which for a full 32 KB image is the difference between about 7
and 20 seconds. The cost is one failed rung (~32 ms) on a 1 MHz part.

**SPIEN is fatal, CKSEL is not.** `avr_fuse_risk()` returns three levels.
Unprogramming SPIEN switches ISP off permanently — refused with no override.
Selecting an external clock is *recoverable* (feed a square wave into XTAL1),
so it needs confirmation rather than refusal, or a legitimate crystal board
could not be programmed. Do not collapse these back into one check.

**The page offers fuse presets, not raw hex entry.** Every preset keeps SPIEN
programmed, so no preset can lock a student out — which makes this safer than
avrdude, where one slipped digit does exactly that. `AVRFUSEW` accepts
arbitrary values over serial for bench use.

**Three connectors, not one.** CPLD JTAG on Pico pins 4-7, AVR ISP on pins
9-12, AVR JTAG on pins 14-17. They could share pins with a mode switch;
separate blocks mean nothing to get wrong, and it means a CPLD board and an
AVR board can both stay wired while the student switches modes on the page.
Swapping a four-wire cable between two boards every time is exactly the
fiddling that ends with a jumper in the wrong hole.

**`PING` is answered `FIRMWARE <version>`, and hosts accept `PONG` too.**
The reply shows up verbatim in a serial monitor, so it reads as an
instrument identifying itself rather than a party game, and it matches the
uppercase-keyword shape of every other reply. The *command* stays `PING` —
renaming it would break older hosts against new firmware for nothing.

Every host matches `(?:FIRMWARE|PONG)\s+(\S+)` rather than the new word
alone. That is not clutter to tidy up: the page is served from GitHub
Pages and updates the moment it is pushed, while the Pico on a student's
desk was flashed whenever they last bothered. Matching only `FIRMWARE`
would greet every one of them with "wrong device" and send them off
checking cables. Drop the alternation only once no pre-1.1 board is left
in circulation, which is not a date anyone can know.

**Which JTAG connector is selected persists in the firmware** until `PORT`
changes it or the Pico resets. So every tool claims the one it wants at the
start of a run rather than assuming — `maxv.py` sends `PORT CPLD` on
connect, `avr.py --jtag` sends `PORT AVR`, and the page re-claims before
both the ID read and the programming run. Without that, programming an AVR
and then a CPLD would read an empty socket and report a wiring fault on a
board that is perfectly well connected. One command removes the whole
class of confusion; do not "optimise" it away.

**Each port keeps its own TAP state.** `current_state` is per-port in
`jtag.c`, not a single global. The two ports drive genuinely separate state
machines, and a shared variable would mean selecting one port and then
walking a TMS path computed from where the *other* board was left standing.

**The two JTAG ports use different signal orders, deliberately.** CPLD is
TMS/TDI/TCK/TDO on GP2-5; AVR is TCK/TDO/TMS/TDI on GP10-13. That looks
inconsistent and is the same rule applied twice: each matches its own
connector. All four AVR JTAG signals sit in the odd-numbered row of the
10-pin header in the order TCK, TDO, TMS, (Vsupply), TDI — so Pico 14-17
runs straight down that row, skipping only header pin 7. Making the two
ports agree with each other would break that for no gain.

**The GPIO order is chosen to match the target boards, not to look tidy.**

```
CPLD JTAG                     AVR ISP                       AVR JTAG
Pico       CPLD board         Pico       AVR board          Pico       header
 4  GP2 -> pin 14  TMS         9  GP6 -> pin 1  MOSI        14  GP10 -> pin 1  TCK
 5  GP3 -> pin 15  TDI        10  GP7 <- pin 2  MISO        15  GP11 <- pin 3  TDO
 6  GP4 -> pin 16  TCK        11  GP8 -> pin 3  SCK         16  GP12 -> pin 5  TMS
 7  GP5 <- pin 17  TDO        12  GP9 -> pin 4  RESET       17  GP13 -> pin 9  TDI
 3  GND                        8  GND                       18  GND -> pin 2 or 10
```

All three cables run straight across with no crossovers, which is the whole
point: students wire this from a photograph, and a cable that cannot be
plugged in twisted beats any error message. The AVR JTAG one runs down the
odd-numbered row of the 10-pin header, skipping only pin 7 (Vsupply) — so
it is four parallel jumpers with one gap, not a crossover.

Resist "fixing" the defines into a conventional order like TCK/TMS/TDI/TDO,
or into agreement between the two JTAG ports — the drivers bit-bang SIO and
do not care which GPIO is which, so the only thing the order affects is
whether the jumpers cross. On the CPLD port it also happens to separate TDI
from TDO, which used to be the classic swap.

GP10-13 were free; GP14 upward and GP0-1 remain so if a fourth connector is
ever needed.

## Constraints you cannot see from the code

**MAX V does not encode density in its IDCODE.** All three CPLD parts report
`0x020A50DD` and share the E64 package, so they are indistinguishable over
JTAG. The page reads the target device from the SVF header comment instead
(`!Device #1: 5M80Z`). Do not add code claiming to detect which part is
fitted — it cannot be done.

**The AVR's 10-pin JTAG header does NOT run straight across.** It is the
standard 2x5 Atmel pinout, which interleaves grounds between signals: TCK
is pin 1, TDO pin 3, TMS pin 5, TDI pin 9, grounds on 2 and 10. This is the
sole exception to the straight-across rule the other two cables follow, so
it is called out explicitly everywhere it appears rather than quietly
breaking the promise made about the other two. `nSRST` and `nTRST` stay
unconnected — JTAG holds the part in reset through its own Reset Register
(`SIR 4 TDI(c)`, `SDR 1 TDI(1)`), which is also why the JTAG path needs no
RESET wire at all.

**JTAG programming needs four pins and nothing else.** Section 27.10 opens
by saying so outright: "Programming through the JTAG interface requires
control of the four JTAG specific pins: TCK, TMS, TDI and TDO. Control of
the reset and clock pins is not required." So the 10-pin header is wired
with five conductors — the four signals and a ground — and `nSRST`,
`nTRST`, `VTref` and `Vsupply` are all left alone. Do not add them back
"for completeness"; each one is either unused by a bit-banged SIO
programmer or actively unwanted (`Vsupply` would power a target from the
Pico). The one case that would justify wiring `nSRST` is the JTD bit,
below.

Prefer header pin 2 for the ground over pin 10: it is adjacent to TCK and
gives the clock the shortest return path, which is the return that matters
when a verify starts failing intermittently.

**Because the clock pin is not required, JTAG does not care about the
CKSEL fuses.** A board whose clock fuses select a crystal that is not
fitted is silent over ISP — ISP needs SCK below a quarter of the target's
clock, and there is no clock — but programs normally over JTAG. That makes
the JTAG path the recovery route for the single most common way a student
bricks a board, and it is a better answer than the "feed a square wave
into XTAL1" advice given elsewhere. That advice is still correct for ISP
and still what the fuse-confirmation dialog says; it is no longer the only
option.

**JTAG programming requires the JTAGEN fuse, which is programmed from the
factory.** A board set to `8mhz-freeportc` has JTAG switched off and can
only be reached over ISP. This is recoverable and not a lockout, but it
does mean the JTAG path cannot be the only path offered.

**The JTD bit in MCUCSR disables JTAG from software, at runtime.** This is
separate from the JTAGEN fuse and is the one JTAG failure that appears
*after* a board has been programmed successfully: a student whose program
sets JTD to use PC2-PC5 as ordinary I/O will find the JTAG mode stops
detecting the board, with nothing about the wiring having changed. The
datasheet's escape is to hold the external RESET pin low, which clears JTD
after two chip clocks and frees the pins for programming — so this is the
one and only reason to run a wire to header pin 6 (`nSRST`). Note that
route needs a running chip clock, unlike everything else about JTAG
programming, so a board with both JTD set and dead clock fuses is an ISP
job. ISP is unaffected by JTD entirely.

**An SVF can compare a value but never report one.** So there is no way to
*read* the fuses over JTAG — only to write them and check the readback
against what was written. Reading fuses stays an ISP feature, and the web
page's fuse panel is therefore ISP-only. Do not add a "read fuses over
JTAG" command; the format cannot carry the answer back.

**The AVR board is 44-pin DIL, not DIP-40.** Nearly every ATmega32A example
online uses the DIP-40 pinout, where RESET is pin 9. On this board it is
pin 4. The `PBn` names (PB5 MOSI, PB6 MISO, PB7 SCK) are identical across
both packages, so cross-check against those and not against pin numbers
copied from a tutorial.

**Web Serial needs a secure context.** HTTPS or `localhost`; `file://` will
not work. It also cannot run in a cross-origin iframe unless the parent sets
`allow="serial"`, which is why the page is linked rather than embedded in the
Google Sites shop. Chrome/Edge/Opera only.

**The `.uf2` files are committed on purpose** so students can flash without a
toolchain. `.gitignore` explicitly un-ignores them. The current pair is
firmware 1.1, built with Pico SDK 2.3.0 and arm-none-eabi-gcc 15.2.Rel1;
that toolchain alone accounts for their jump from ~83 KB to ~111 KB of
flash — building the *previous* source with it produces the same size, so
the growth is not from any code change. Rebuild both with
`firmware/build.ps1 -Release` rather than by hand, so the two stay in step
with each other.

A 1.0 board still works, and the host degrades rather than failing: it has
one JTAG connector, so `PORT` returns an error and the tools say the AVR
cable has to go on pins 4-7 instead of 14-17, and it omits
`max_stmt_bytes` from `INFO`, so the host falls back to the slower AVR
verify. Reflashing buys the second connector and the quicker verify; it is
not required for correctness. Note that `build.ps1` sets `$ErrorActionPreference =
"Stop"`, and Windows PowerShell 5.1 turns cmake's ordinary stderr chatter
into a terminating error — so run it as `powershell.exe -File build.ps1`
from outside PowerShell if an invocation dies on the first cmake line
without a real error.

**RP2040 and RP2350 UF2s are not interchangeable** — different family IDs
(`0xE48BFF56` / `0xE48BFF57`). The bootloader rejects a mismatch, so it is
harmless, just confusing.

## Build and test

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cd firmware && mkdir build && cd build
cmake .. -DPICO_BOARD=pico        # or pico2
make -j4
```

On Windows, `firmware/build.ps1` wraps that and finds the SDK and toolchain
the Pico VS Code extension installs under `%USERPROFILE%\.pico-sdk`:
`.\build.ps1 -Board both -Release`.

```bash
cd tests && ./run_tests.sh
```

Tests compile the firmware's own `svf.c` against a simulated MAX V TAP,
rather than a reimplementation — a test must not be able to pass while the
firmware is broken. Drop a real `.svf` in as `tests/sample.svf` to enable the
end-to-end parse test (gitignored), or a real `.hex` as `tests/sample.hex`
for the AVR JTAG one.

The same rule shapes the AVR JTAG tests. `test_avr_jtag.py` does not check
that the generated file *contains* the right hex strings — that would pass
just as happily against a file that programs the wrong address or loads a
page backwards. It plays the file into a simulated ATmega32A and checks the
flash afterwards.

## Open items

- All three paths have now run on hardware, with both boards wired at
  once (see Status). Do not restate this as a blanket "nothing is tested"
  — that was true once and stopped being true, which is how this line
  rotted before. What is *not* covered: EEPROM, lock bits, and any AVR
  other than the ATmega32A.
- **A SPIEN-disabled chip is now recoverable**, since JTAG programming does
  not depend on SPIEN. This is untried, and the firmware still refuses to
  write that fuse, but it is no longer the dead end the ISP-only note
  described.
- **EEPROM is not implemented over JTAG.** The command table for it
  (instructions 4a-5d) is in the same datasheet section and was read but
  not ported, because nothing here needs it. If you add it, note that
  PROG_PAGELOAD/PAGEREAD cannot be used for EEPROM — the datasheet says so
  explicitly and the byte-wise path is the only option.
- **Lock bits are not implemented** either, for the same reason. Be careful
  if you add them: a lock bit that blocks further programming would be the
  one genuinely irreversible thing this tool could do.
- `SVF_MAX_BITS` caps a single shift at 4096 bits. Fine for these MAX V
  parts and for the AVR's 1032-bit page reads; a larger device may need it
  raised.
- `SVF_MAX_STMT` caps a statement at 1024 bytes, which the AVR page verify
  needs 537 of. A part with a 256-byte flash page would need ~1060 and
  would not fit — raise it there rather than falling back to word mode.

## Style

Comments explain **why**, not what. If a line of code needs a comment saying
what it does, the code should be clearer instead. Existing comments that
explain a non-obvious tradeoff are load-bearing — do not strip them as noise.
