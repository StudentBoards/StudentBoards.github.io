# CLAUDE.md

Context for working on this repository. Read before changing anything.

## What this is

A Raspberry Pi Pico acting as a programmer for two studentboards products,
driven from a web page over Web Serial. Students plug in a Pico, open the
page, and drop a file. No drivers, no toolchain, no install.

- **MAX V CPLD boards** (5M40ZE64 / 5M80ZE64 / 5M160ZE64) — programmed over
  **JTAG** from a Quartus `.svf`.
- **ATmega32A boards** — programmed over **ISP** from an Intel `.hex`.

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

**The AVR ISP path is still untested on hardware.** It builds and the Intel
HEX parser is tested, but no ATmega32A has been programmed with it. Expect
the first bench run to find something.

Treat bug reports from real hardware as more authoritative than anything in
the code comments, including these. The one bug found so far
(`ENDIR`/`ENDDR` conflation, see below) passed fourteen host-side tests and
all four `DIAG` checks before hardware caught it.

## Layout

```
index.html          the whole web app: one file, no build step, no deps
firmware/           Pico firmware (C, CMake, Pico SDK)
  main.c            USB CDC protocol, command dispatch, LED states
  jtag.c/.h         JTAG TAP driver — MAX V
  svf.c/.h          streaming SVF parser
  avr.c/.h          AVR ISP driver — ATmega32A
tools/maxv.py       CLI alternative to the page, same serial protocol (CPLD)
tools/avr.py        the same for the ATmega32A, plus a host-composed --diag
tests/              host-side tests, no hardware needed
```

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

**SPIEN is fatal, CKSEL is not.** `avr_fuse_risk()` returns three levels.
Unprogramming SPIEN switches ISP off permanently — refused with no override.
Selecting an external clock is *recoverable* (feed a square wave into XTAL1),
so it needs confirmation rather than refusal, or a legitimate crystal board
could not be programmed. Do not collapse these back into one check.

**The page offers fuse presets, not raw hex entry.** Every preset keeps SPIEN
programmed, so no preset can lock a student out — which makes this safer than
avrdude, where one slipped digit does exactly that. `AVRFUSEW` accepts
arbitrary values over serial for bench use.

**Two cables, not one.** JTAG on Pico pins 4-7, ISP on pins 9-12. They could
share pins with a mode switch; separate blocks mean nothing to get wrong.

## Constraints you cannot see from the code

**MAX V does not encode density in its IDCODE.** All three CPLD parts report
`0x020A50DD` and share the E64 package, so they are indistinguishable over
JTAG. The page reads the target device from the SVF header comment instead
(`!Device #1: 5M80Z`). Do not add code claiming to detect which part is
fitted — it cannot be done.

**Web Serial needs a secure context.** HTTPS or `localhost`; `file://` will
not work. It also cannot run in a cross-origin iframe unless the parent sets
`allow="serial"`, which is why the page is linked rather than embedded in the
Google Sites shop. Chrome/Edge/Opera only.

**The `.uf2` files are committed on purpose** so students can flash without a
toolchain. `.gitignore` explicitly un-ignores them. The current pair was
built with Pico SDK 2.3.0 and arm-none-eabi-gcc 15.2.Rel1; that toolchain
alone accounts for their jump from ~83 KB to ~111 KB of flash — building
the *previous* source with it produces the same size, so the growth is not
from any code change. Rebuild both with `firmware/build.ps1 -Release`
rather than by hand, so the two stay in step with each other.

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
end-to-end parse test (gitignored).

## Open items

- **AVR over JTAG is not implemented.** It would make a SPIEN-disabled chip
  recoverable with this same hardware, and would let both boards share one
  cable. Blocked on the ATmega32A datasheet's Programming Command Register
  table (the 15-bit command words for instructions 1a-4a) — do not
  reconstruct these from memory or from another part's datasheet.
- Everything past "the Pico enumerates" is untested on hardware.
- `SVF_MAX_BITS` caps a single shift at 4096 bits. Fine for these MAX V
  parts; a larger device may need it raised.

## Style

Comments explain **why**, not what. If a line of code needs a comment saying
what it does, the code should be clearer instead. Existing comments that
explain a non-obvious tradeoff are load-bearing — do not strip them as noise.
