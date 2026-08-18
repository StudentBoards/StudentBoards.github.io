#!/usr/bin/env python3
"""
avr.py — program an ATmega32A through a Raspberry Pi Pico.

Quick start:

    # Build your project, then program the board:
    python3 avr.py main.hex

Other things you can do:

    python3 avr.py --id                      check the board is detected
    python3 avr.py --fuses                   read and decode the fuses
    python3 avr.py --diag                    diagnose a board that won't talk
    python3 avr.py --verify main.hex         compare flash, change nothing
    python3 avr.py --set-fuses 8mhz          apply a named fuse preset
    python3 avr.py --list                    list serial ports
    python3 avr.py main.hex -p COM4          pick the port manually

WIRING (Pico pins 9-12, the block below the JTAG one):

    Pico    AVR board
    ------  -------------
    GP6     SCK    (PB7)
    GP7     MOSI   (PB5)
    GP8     MISO   (PB6)
    GP9     RESET  (pin 9)
    GND     GND

The Pico's GPIO is 3.3 V and NOT 5 V tolerant. An ATmega32A running at
5 V will drive MISO at 5 V and damage the Pico — only connect a board
running at 3.3 V, or fit a level shifter.

This file is deliberately standalone rather than importing the shared
bits from maxv.py: these scripts get copied out of the repo one at a
time, and a tool that fails because its sibling was left behind is
exactly the kind of thing that turns into a support email.

Requires pyserial:   pip install pyserial
"""

import argparse
import os
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("This needs pyserial. Install it with:\n\n    pip install pyserial\n",
          file=sys.stderr)
    sys.exit(1)


# The Pico's USB vendor/product IDs. Used to find the board automatically
# so students never have to work out which COM port it landed on — the
# single most common source of "it doesn't work" reports.
PICO_VID = 0x2E8A
PICO_PIDS = (0x000A, 0x0009, 0x0005)   # stdio CDC, and the RP2350 variants

AVR_FLASH = 32768        # ATmega32A flash, bytes
AVR_PAGE = 128           # bytes per flash page

ATMEGA32_SIG = 0x1E9502          # ATmega32 and ATmega32A share this


def sig_value(text):
    """Parse a '0x1E9502' signature token, or return None."""
    try:
        return int(text, 16)
    except (TypeError, ValueError):
        return None


# ---------------------------------------------------------------------
# Port discovery
# ---------------------------------------------------------------------

def find_port(explicit=None):
    """Locate the Pico, or raise with a helpful message."""
    if explicit:
        return explicit

    candidates = []
    for p in serial.tools.list_ports.comports():
        if p.vid == PICO_VID and (p.pid in PICO_PIDS or p.pid is None):
            candidates.append(p.device)

    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        raise RuntimeError(
            "More than one Pico found: " + ", ".join(candidates) +
            "\nPick one with -p, e.g.  -p " + candidates[0])

    raise RuntimeError(
        "No Pico found.\n"
        "  - Is it plugged in?\n"
        "  - Has the firmware been flashed? (hold BOOTSEL while plugging in, "
        "then copy pico_maxv.uf2 to the RPI-RP2 drive)\n"
        "  - Run 'python3 avr.py --list' to see all serial ports.")


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    print(f"{'Port':<20} {'VID:PID':<12} Description")
    print("-" * 60)
    for p in ports:
        vidpid = (f"{p.vid:04X}:{p.pid:04X}"
                  if p.vid is not None and p.pid is not None else "")
        mark = "  <-- Pico" if p.vid == PICO_VID else ""
        print(f"{p.device:<20} {vidpid:<12} {p.description}{mark}")


# ---------------------------------------------------------------------
# Intel HEX
#
# Parsed on the host, not on the Pico — the firmware receives a flat
# binary image, which keeps address-record and checksum handling out of
# it entirely. This is a direct port of the parser in index.html, error
# messages included, so a file that is rejected here is rejected the
# same way on the web page.
# ---------------------------------------------------------------------

class HexError(Exception):
    pass


def parse_hex(text):
    """Return (image_bytes, top_address). Image is trimmed to what is used."""
    out = bytearray(b"\xFF" * AVR_FLASH)
    high = 0        # base address from an extended-address record
    top = 0         # one past the highest byte written
    seen = False

    for n, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if line[0] != ":":
            raise HexError(f"Line {n}: not an Intel HEX record.")
        if len(line) < 11:
            raise HexError(f"Line {n}: record too short.")

        try:
            b = bytes.fromhex(line[1:])
        except ValueError:
            raise HexError(f"Line {n}: not valid hex.")

        length, addr, rectype = b[0], (b[1] << 8) | b[2], b[3]
        if len(b) != length + 5:
            raise HexError(f"Line {n}: length mismatch.")

        # Checksum is the two's complement of the sum of preceding bytes.
        if ((~sum(b[:-1]) + 1) & 0xFF) != b[-1]:
            raise HexError(f"Line {n}: checksum error — the file may be corrupt.")

        if rectype == 0x00:                       # data
            base = high + addr
            for i in range(length):
                a = base + i
                if a >= AVR_FLASH:
                    raise HexError(
                        "This program is bigger than the ATmega32A's 32 KB "
                        "of flash.")
                out[a] = b[4 + i]
                top = max(top, a + 1)
                seen = True
        elif rectype == 0x01:                     # end of file
            break
        elif rectype == 0x04:                     # extended linear address
            high = ((b[4] << 8) | b[5]) << 16
        elif rectype == 0x02:                     # extended segment address
            high = ((b[4] << 8) | b[5]) << 4
        # Types 03 and 05 carry a start address for the host loader. An
        # AVR ignores them, so skipping them silently is correct — the
        # avr-gcc toolchain does not emit them, but other tools do.

    if not seen:
        raise HexError("No program data found in this file.")

    return bytes(out[:top]), top


# ---------------------------------------------------------------------
# Fuses
#
# Presets rather than raw hex, matching the web page exactly. Every
# preset keeps SPIEN programmed, so no preset can switch ISP off — which
# makes this safer than typing bytes into avrdude, where one slipped
# digit does precisely that. --lfuse/--hfuse is there for bench use.
# ---------------------------------------------------------------------

FUSE_PRESETS = {
    "1mhz": (0xE1, 0x99,
             "Internal 1 MHz — factory default, works with no crystal fitted"),
    "8mhz": (0xE4, 0x99,
             "Internal 8 MHz — no crystal needed; update F_CPU to match"),
    "crystal": (0xFF, 0x89,
                "External crystal — ONLY if your board has one fitted"),
    "8mhz-freeportc": (0xE4, 0xD9,
                       "Internal 8 MHz, PORTC 2-5 free (JTAG debug off)"),
}
# Which presets need --confirm is not recorded here on purpose: fuse_risk()
# decides, from the values themselves, so a preset edited later cannot end
# up silently exempt from the check it should have triggered.


def describe_fuses(lfuse, hfuse):
    """Decode the handful of bits a student actually cares about.

    Listing all sixteen would bury the two that matter.
    """
    cksel = lfuse & 0x0F
    clock = {
        0x0: "external clock input",
        0x1: "internal 1 MHz",
        0x2: "internal 2 MHz",
        0x3: "internal 4 MHz",
        0x4: "internal 8 MHz",
    }.get(cksel, "external crystal")
    portc = ("PORTC 2-5 free" if (hfuse >> 6) & 1
             else "PORTC 2-5 reserved by the debug interface")
    return f"{clock} · {portc}"


def fuse_risk(lfuse, hfuse):
    """Mirror of avr_fuse_risk() in the firmware. Returns (level, why).

    Checked here as well as on the Pico so a bad value is refused before
    it is ever sent, and so the explanation can be longer than a serial
    reply. The firmware check is the one that counts; this one exists to
    explain, not to protect.

    AVR fuses are ACTIVE LOW throughout — 0 means programmed, feature ON.
    That inversion is the usual reason parts get bricked, so both checks
    below spell out the sense.
    """
    # SPIEN, hfuse bit 5. A 1 means unprogrammed: ISP off for good.
    if (hfuse >> 5) & 1:
        return "fatal", ("SPIEN would be unprogrammed, which switches ISP off "
                         "permanently. Recovery needs a high-voltage "
                         "programmer. Refused — there is no override.")

    # CKSEL, lfuse bits 3:0. 0x1..0x4 are the internal RC and always work.
    cksel = lfuse & 0x0F
    if cksel < 0x1 or cksel > 0x4:
        return "confirm", ("CKSEL selects an external clock or crystal. If the "
                           "board has one fitted this is correct; if not, the "
                           "chip goes silent until you feed a square wave into "
                           "its XTAL1 pin. Pass --confirm if you mean it.")

    return "fine", None


# ---------------------------------------------------------------------
# Serial protocol
# ---------------------------------------------------------------------

class Programmer:
    def __init__(self, port, timeout=15):
        self.ser = serial.Serial(port, 115200, timeout=timeout)
        # The Pico reboots its CDC stack on open; give it a moment before
        # talking, or the first command gets swallowed.
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def command(self, text):
        self.ser.write((text + "\n").encode())
        self.ser.flush()

    def readline(self):
        return self.ser.readline().decode(errors="replace").strip()

    def ask(self, text, timeout=15):
        """Send a command and return the first non-PROGRESS reply."""
        old, self.ser.timeout = self.ser.timeout, timeout
        try:
            self.command(text)
            while True:
                line = self.readline()
                if not line or not line.startswith("PROGRESS"):
                    return line
        finally:
            self.ser.timeout = old

    def read_signature(self):
        return self.ask("AVRID", 20)

    def read_fuses(self):
        return self.ask("AVRFUSES", 20)

    # -- bulk transfers -----------------------------------------------

    def _send_image(self, verb, data, label):
        """Shared front half of AVRFLASH and AVRVERIFY.

        Both take `<verb> <bytes>`, answer READY, then swallow exactly
        that many raw bytes. Returns True once the image is away.
        """
        self.command(f"{verb} {len(data)}")
        ready = self.readline()
        if not ready.startswith("READY"):
            print(f"Unexpected reply: {ready or '(nothing)'}", file=sys.stderr)
            return False

        # Modest chunks so the Pico's 64-byte USB packets never back up,
        # and so progress can be shown as we go.
        chunk = 512
        sent = 0
        while sent < len(data):
            self.ser.write(data[sent:sent + chunk])
            sent += min(chunk, len(data) - sent)
            pct = 100 * sent // len(data)
            bar = "#" * (pct // 4) + "-" * (25 - pct // 4)
            print(f"\r  [{bar}] {pct:3d}%  {label}", end="", flush=True)
        self.ser.flush()
        print()
        return True

    def _await_result(self, unit="bytes"):
        """Read PROGRESS lines until DONE or ERR. Returns the final line."""
        # Chip erase, 256 page writes and the firmware's own verify all
        # happen after the last byte is sent, so this wait is generous.
        old, self.ser.timeout = self.ser.timeout, 180
        try:
            while True:
                line = self.readline()
                if not line:
                    return None
                if line.startswith("PROGRESS"):
                    parts = line.split()
                    if len(parts) > 1:
                        print(f"\r  working... {parts[1]} {unit}",
                              end="", flush=True)
                    continue
                print()
                return line
        finally:
            self.ser.timeout = old

    def flash(self, data):
        start = time.time()
        if not self._send_image("AVRFLASH", data, f"sending {len(data)} bytes"):
            return False
        line = self._await_result()
        if line is None:
            print("Timed out waiting for a result.", file=sys.stderr)
            return False
        if line.startswith("DONE"):
            print(f"  {line}")
            print(f"\nPASS — programmed and verified in {time.time() - start:.1f}s")
            return True
        explain_error(line)
        return False

    def verify(self, data):
        start = time.time()
        if not self._send_image("AVRVERIFY", data, f"sending {len(data)} bytes"):
            return False
        line = self._await_result()
        if line is None:
            print("Timed out waiting for a result.", file=sys.stderr)
            return False
        if line.startswith("DONE"):
            print(f"  {line}")
            print(f"\nMATCH — flash is identical to the file "
                  f"({time.time() - start:.1f}s)")
            return True
        explain_error(line)
        return False


# ---------------------------------------------------------------------
# Turning replies into something a student can act on
# ---------------------------------------------------------------------

WIRING_HELP = (
    "  - Check SCK/MOSI/MISO/RESET are not swapped (MOSI and MISO are the\n"
    "    easy pair to get backwards)\n"
    "  - Check the target board has power\n"
    "  - Check GND is connected between the Pico and the board\n"
    "  - Check the board runs at 3.3 V — a 5 V board will not read reliably\n"
    "    and can damage the Pico\n"
    "  - If the chip's clock fuses were changed to an external crystal that\n"
    "    is not fitted, it cannot answer at all until a clock is fed into\n"
    "    XTAL1"
)


def explain_error(line):
    """Print a failure with the three things worth checking, in order."""
    print(f"  {line}", file=sys.stderr)

    if "NO_TARGET" in line or "NO_SYNC" in line:
        print("\nThe ATmega32A did not answer.", file=sys.stderr)
        print(WIRING_HELP, file=sys.stderr)
    elif "SIGNATURE" in line:
        print("\nSomething answered, but it is not an ATmega32A.",
              file=sys.stderr)
        print("  - Check you are on the AVR board, not the CPLD one\n"
              "  - A garbled signature usually means a marginal link rather\n"
              "    than the wrong chip: shorten the jumper wires and retry",
              file=sys.stderr)
    elif "MISMATCH" in line:
        print("\nThe flash does not match this file. That is expected if the\n"
              "board was last programmed with something else — program it to\n"
              "bring it into line.", file=sys.stderr)
    elif "VERIFY" in line:
        print("\nThe programming was accepted but read back wrong. This is\n"
              "almost always a marginal link rather than a bad chip:",
              file=sys.stderr)
        print("  - Shorten the jumper wires, especially SCK\n"
              "  - Check the ground connection\n"
              "  - Retry; if it fails at the same byte every time, suspect\n"
              "    the chip", file=sys.stderr)
    elif "TOO_BIG" in line:
        print("\nThe program does not fit in the ATmega32A's 32 KB of flash.",
              file=sys.stderr)
    print("\nFAIL", file=sys.stderr)


def show_signature(reply):
    """Report the signature. Returns True if a usable part answered."""
    if reply.startswith("ERR") or not reply.startswith("SIG"):
        print("No ATmega32A detected.", file=sys.stderr)
        print(WIRING_HELP, file=sys.stderr)
        return False

    print(reply)

    # Say so here rather than leaving it to the firmware's pre-erase check.
    # Programming will be refused either way, but a student reading "not an
    # ATmega32A" at the --id stage has been told before they wonder why
    # nothing happened.
    parts = reply.split()
    if len(parts) > 1 and sig_value(parts[1]) != ATMEGA32_SIG:
        print(f"\nThis is not an ATmega32A (expected 0x{ATMEGA32_SIG:06X}).\n"
              "Check you are on the AVR board, not the CPLD one.",
              file=sys.stderr)
        return False

    # The firmware reports how much extra effort the read took. Zero means
    # it read cleanly at full speed; a high count means the board works but
    # is marginal — and a marginal board is the one whose programming fails
    # later, so it is worth saying so while things still look fine.
    for tok in reply.split():
        if tok.startswith("retries="):
            try:
                n = int(tok.split("=")[1])
            except ValueError:
                break
            if n >= 4:
                print("\nNote: the signature only read back after backing the\n"
                      "clock right off. The chip is fine, but the link is\n"
                      "marginal — shorter wires and a solid ground will make\n"
                      "programming more reliable.")
            break
    return True


def show_fuses(reply):
    """Decode a FUSES reply. Returns (lfuse, hfuse) or None."""
    if not reply.startswith("FUSES"):
        print("Could not read the fuses.", file=sys.stderr)
        print(WIRING_HELP, file=sys.stderr)
        return None

    vals = {}
    for tok in reply.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            vals[k] = v

    try:
        lfuse = int(vals["lfuse"], 16)
        hfuse = int(vals["hfuse"], 16)
    except (KeyError, ValueError):
        print(f"Unexpected reply: {reply}", file=sys.stderr)
        return None

    lock = vals.get("lock", "?")
    print(f"lfuse=0x{lfuse:02X}  hfuse=0x{hfuse:02X}  lock={lock}")
    print(f"  {describe_fuses(lfuse, hfuse)}")

    level, why = fuse_risk(lfuse, hfuse)
    if level != "fine":
        print(f"\n  note: {why}")
    return lfuse, hfuse


# ---------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------

def do_set_fuses(prog, lfuse, hfuse, confirmed, description=None):
    level, why = fuse_risk(lfuse, hfuse)

    if level == "fatal":
        print(f"Refused: 0x{lfuse:02X} / 0x{hfuse:02X}", file=sys.stderr)
        print(f"  {why}", file=sys.stderr)
        return 1
    if level == "confirm" and not confirmed:
        print(f"Needs confirmation: 0x{lfuse:02X} / 0x{hfuse:02X}",
              file=sys.stderr)
        print(f"  {why}", file=sys.stderr)
        return 1

    if description:
        print(f"Setting fuses to {description}")
    print(f"  lfuse=0x{lfuse:02X} hfuse=0x{hfuse:02X} "
          f"({describe_fuses(lfuse, hfuse)})")

    cmd = f"AVRFUSEW {lfuse:x} {hfuse:x}" + (" CONFIRM" if confirmed else "")
    reply = prog.ask(cmd, 30)

    if reply.startswith("FUSEOK"):
        print(f"  {reply}")
        print("\nPASS — fuses changed.")
        if (lfuse & 0x0F) != 0x01:
            print("If you changed the clock speed, update F_CPU in your code "
                  "to match,\nor timing and serial baud rates will be wrong.")
        return 0

    explain_error(reply or "ERR no reply")
    return 1


def do_diag(prog):
    """Diagnose an AVR board that will not program.

    The firmware's DIAG command exercises the JTAG TAP and says nothing
    about ISP, so this is composed on the host out of the AVR commands
    that do exist. The point is the same: several independent readings
    that, taken together, separate a wiring problem from a chip problem.
    """
    print("Diagnostics\n")
    ok = True

    print("1. Firmware")
    info = prog.ask("INFO", 10)
    print(f"   {info or '(no reply)'}")
    if not info.startswith("INFO"):
        print("   The Pico is not running this firmware.", file=sys.stderr)
        return 1

    # Three independent detection attempts. One read can return plausible
    # garbage; three that agree is evidence. Three that disagree points at
    # the link rather than the chip, which is a different fix entirely.
    print("\n2. Signature, read three times")
    sigs = []
    for i in range(3):
        reply = prog.read_signature()
        print(f"   {reply or '(no reply)'}")
        sigs.append(reply.split()[1] if reply.startswith("SIG") else None)

    good = [s for s in sigs if s]
    if not good:
        print("\n   Nothing answered on the ISP pins.", file=sys.stderr)
        print(WIRING_HELP, file=sys.stderr)
        return 1
    if len(good) < 3:
        print("\n   Intermittent: the chip answered some of the time. That is\n"
              "   a marginal link, not a dead part — shorten the wires,\n"
              "   especially SCK, and check the ground.")
        ok = False
    elif len({sig_value(s) for s in good}) > 1:
        print("\n   The signature changed between reads. The chip is being\n"
              "   misread — shorten the wires and check the ground.")
        ok = False
    elif sig_value(good[0]) != ATMEGA32_SIG:
        print(f"\n   {good[0]} is not an ATmega32A (0x{ATMEGA32_SIG:06X}).\n"
              "   Check you are on the AVR board, not the CPLD one.")
        ok = False

    print("\n3. Fuses")
    fuses = show_fuses(prog.read_fuses())
    if fuses is None:
        return 1

    lfuse, hfuse = fuses
    # Cross-check the two readings against each other. A board whose fuses
    # say "external crystal" but which reads fine tells you the crystal is
    # really there; one that says so and reads badly tells you it is not.
    print("\n4. Cross-check")
    cksel = lfuse & 0x0F
    if 0x1 <= cksel <= 0x4:
        print("   Clock comes from the internal RC oscillator, so the chip\n"
              "   will answer regardless of what is fitted to XTAL1/XTAL2.")
    else:
        print("   Fuses select an external clock or crystal. Since the chip\n"
              "   answered, that clock is present and working.")
    if (hfuse >> 5) & 1:
        print("   SPIEN reads as unprogrammed, which should make ISP\n"
              "   impossible — yet the chip answered. Treat this reading as\n"
              "   suspect and check the wiring before writing anything.")
        ok = False

    print("\nAll checks passed." if ok else "\nSomething is off — see above.")
    return 0 if ok else 1


def load_hex(path):
    """Read and parse a .hex file, or exit with a clear message."""
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print(f"Cannot read {path}: {e}", file=sys.stderr)
        return None

    try:
        data, top = parse_hex(text)
    except HexError as e:
        print(f"{path}: {e}", file=sys.stderr)
        return None

    pages = (top + AVR_PAGE - 1) // AVR_PAGE
    print(f"{path}: {top} bytes, {pages} pages, "
          f"{100.0 * top / AVR_FLASH:.1f}% of flash")
    return data


# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Program an ATmega32A through a Raspberry Pi Pico.",
        epilog="Fuse presets: " + ", ".join(FUSE_PRESETS))
    ap.add_argument("hex", nargs="?", help="Intel HEX file to program")
    ap.add_argument("-p", "--port",
                    help="Serial port (auto-detected if omitted)")
    ap.add_argument("--id", action="store_true",
                    help="Just read the signature and exit")
    ap.add_argument("--info", action="store_true",
                    help="Show firmware info and exit")
    ap.add_argument("--diag", action="store_true",
                    help="Diagnose a board that will not program")
    ap.add_argument("--list", action="store_true",
                    help="List serial ports and exit")
    ap.add_argument("--fuses", action="store_true",
                    help="Read and decode the fuses, then exit")
    ap.add_argument("--set-fuses", metavar="PRESET",
                    choices=sorted(FUSE_PRESETS),
                    help="Apply a named fuse preset")
    ap.add_argument("--lfuse", help="Raw low fuse in hex, e.g. E4 (bench use)")
    ap.add_argument("--hfuse", help="Raw high fuse in hex, e.g. 99 (bench use)")
    ap.add_argument("--confirm", action="store_true",
                    help="Allow a fuse setting that needs an external clock")
    ap.add_argument("--verify", action="store_true",
                    help="Compare flash against the HEX file without writing "
                         "anything — answers 'is this board already running "
                         "the right build?'")
    ap.add_argument("--verify-after", action="store_true",
                    help="After programming, re-send the image and compare "
                         "(catches a corrupted transfer, which the firmware's "
                         "own verify cannot)")
    ap.add_argument("--no-check", action="store_true",
                    help="Skip the signature check before programming")
    ap.add_argument("--bootsel", action="store_true",
                    help="Put the Pico into its bootloader for reflashing")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return 0

    raw_fuses = args.lfuse is not None or args.hfuse is not None
    if raw_fuses and not (args.lfuse and args.hfuse):
        ap.error("--lfuse and --hfuse must be given together")
    if raw_fuses and args.set_fuses:
        ap.error("give either --set-fuses or --lfuse/--hfuse, not both")

    actions = (args.id or args.info or args.diag or args.fuses or
               args.set_fuses or raw_fuses or args.bootsel)
    if not args.hex and not actions:
        ap.error("give a HEX file, or use --id / --fuses / --diag / --list")
    if args.verify and not args.hex:
        ap.error("--verify needs a HEX file to compare against")

    # Parse everything the user typed before opening the port: a typo in a
    # filename or a fuse byte should not leave the target sitting in reset.
    image = load_hex(args.hex) if args.hex else None
    if args.hex and image is None:
        return 1

    fuses = None
    if args.set_fuses:
        lfuse, hfuse, desc = FUSE_PRESETS[args.set_fuses]
        fuses = (lfuse, hfuse, desc)
    elif raw_fuses:
        try:
            fuses = (int(args.lfuse, 16), int(args.hfuse, 16), None)
        except ValueError:
            print("--lfuse and --hfuse take hex bytes, e.g. E4 and 99",
                  file=sys.stderr)
            return 1
        if not all(0 <= v <= 0xFF for v in fuses[:2]):
            print("--lfuse and --hfuse take a single byte each (00 to FF)",
                  file=sys.stderr)
            return 1

    try:
        port = find_port(args.port)
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1

    try:
        prog = Programmer(port)
    except serial.SerialException as e:
        print(f"Could not open {port}: {e}", file=sys.stderr)
        if os.name == "posix":
            print("On Linux you may need to be in the 'dialout' group:\n"
                  "    sudo usermod -a -G dialout $USER   (then log out and "
                  "back in)", file=sys.stderr)
        return 1

    try:
        pong = prog.ask("PING", 5)
        if not pong.startswith("PONG"):
            print(f"No response from {port} — is this the right device?",
                  file=sys.stderr)
            return 1
        print(f"Connected on {port} ({pong})")

        if args.bootsel:
            print(prog.ask("BOOTSEL", 5))
            print("The Pico should now appear as a USB drive.")
            return 0

        if args.info:
            print(prog.ask("INFO", 10))
            return 0

        if args.diag:
            return do_diag(prog)

        if args.fuses:
            return 0 if show_fuses(prog.read_fuses()) else 1

        if fuses is not None:
            # Naming a preset is not the same as consenting to it: the one
            # setting that can leave a board apparently dead still requires
            # --confirm, exactly as the web page still shows a dialog.
            lfuse, hfuse, desc = fuses
            return do_set_fuses(prog, lfuse, hfuse, args.confirm, desc)

        if args.id:
            return 0 if show_signature(prog.read_signature()) else 1

        if args.verify:
            return 0 if prog.verify(image) else 1

        # Check the target is there before erasing it. An erase against an
        # absent or wrong device is the one irreversible step here.
        if not args.no_check:
            if not show_signature(prog.read_signature()):
                return 1

        if not prog.flash(image):
            return 1

        if args.verify_after:
            # The firmware verifies flash against the copy it received, so
            # a transfer that arrived corrupted would still pass. Sending
            # the image a second time and comparing closes that gap: two
            # independent transfers corrupting identically is not a case
            # worth worrying about.
            print("\nRe-sending the image to check the transfer itself...")
            return 0 if prog.verify(image) else 1
        return 0

    finally:
        prog.close()


if __name__ == "__main__":
    sys.exit(main())
