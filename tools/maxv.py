#!/usr/bin/env python3
"""
maxv.py — program a MAX V CPLD through a Raspberry Pi Pico.

Quick start:

    # 1. Export an SVF from your Quartus project (once per build):
    quartus_cpf -c output_files/design.pof design.svf

    # 2. Program the board:
    python3 maxv.py design.svf

Other things you can do:

    python3 maxv.py --id                 check the board is detected
    python3 maxv.py --list               list serial ports
    python3 maxv.py design.svf -p COM4   pick the port manually
    python3 maxv.py design.svf --slow    slow the clock down (flaky wiring)

WIRING (Pico pins 3-7). The CPLD-side numbers are the 64-pin DIL
package pins:

    Pico phys  GPIO         CPLD board (64-pin DIL)
    ---------  ----         -----------------------
        3      GND    --    GND
        4      GP2    ->    pin 16   TCK
        5      GP3    ->    pin 14   TMS
        6      GP4    ->    pin 15   TDI
        7      GP5    <-    pin 17   TDO

TDI and TDO are the pair to get wrong — adjacent at both ends, and
swapping them looks exactly like a dead chip.

Requires pyserial:   pip install pyserial
"""

import argparse
import os
import re
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
        "then copy pico1_programmer.uf2 — or pico2_programmer.uf2 for a "
        "Pico 2 — to the RPI-RP2 drive)\n"
        "  - Run 'python3 maxv.py --list' to see all serial ports.")


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
        line = self.ser.readline().decode(errors="replace").strip()
        return line

    def ping(self):
        self.command("PING")
        return self.readline()

    def read_id(self):
        self.command("ID")
        return self.readline()

    def set_speed(self, us):
        self.command(f"SPEED {us}")
        return self.readline()

    @staticmethod
    def fnv1a(data):
        """Same hash the firmware computes over what it receives."""
        h = 2166136261
        for b in data:
            h = ((h ^ b) * 16777619) & 0xFFFFFFFF
        return h

    def program(self, data, show_progress=True):
        """Stream an SVF and report the result. Returns True on success."""
        self.command(f"SVF {len(data)}")
        ready = self.readline()
        if not ready.startswith("READY"):
            print(f"Unexpected reply: {ready}", file=sys.stderr)
            return False

        # Write in modest chunks so the Pico's 64-byte USB packets never
        # back up, and so we can show progress as we go.
        chunk = 512
        sent = 0
        start = time.time()

        while sent < len(data):
            block = data[sent:sent + chunk]
            self.ser.write(block)
            sent += len(block)
            if show_progress:
                pct = 100 * sent // len(data)
                bar = "#" * (pct // 4) + "-" * (25 - pct // 4)
                print(f"\r  [{bar}] {pct:3d}%  {sent}/{len(data)} bytes",
                      end="", flush=True)

        self.ser.flush()
        if show_progress:
            print()

        # Now wait for the verdict. Progress lines may arrive first, and
        # programming continues after the last byte is sent, so the read
        # timeout here has to be generous.
        self.ser.timeout = 120
        while True:
            line = self.readline()
            if not line:
                print("Timed out waiting for a result.", file=sys.stderr)
                return False
            if line.startswith("PROGRESS"):
                if show_progress:
                    print(f"\r  programming... {line.split()[1]} statements",
                          end="", flush=True)
                continue
            if show_progress:
                print()
            elapsed = time.time() - start
            # Compare the firmware's hash of what it received against
            # ours. A mismatch means the failure is in the link, not in
            # the programming — completely different fix.
            m = re.search(r"rx=(\d+) hash=([0-9A-Fa-f]{8})", line)
            if m:
                rx, got_hash = int(m.group(1)), int(m.group(2), 16)
                want = self.fnv1a(data)
                if rx != len(data) or got_hash != want:
                    print(f"\n  {line}")
                    print(f"\n*** TRANSFER CORRUPTED ***", file=sys.stderr)
                    print(f"  sent {len(data)} bytes, hash {want:08X}",
                          file=sys.stderr)
                    print(f"  device got {rx} bytes, hash {got_hash:08X}",
                          file=sys.stderr)
                    print("  The file did not arrive intact, so any "
                          "programming error above is a symptom, not the "
                          "cause.", file=sys.stderr)
                    return False
                print(f"\n  (transfer verified: {rx} bytes, "
                      f"hash {got_hash:08X})")

            if line.startswith("DONE"):
                print(f"  {line}")
                print(f"\nPASS — programmed in {elapsed:.1f}s")
                return True
            print(f"  {line}", file=sys.stderr)
            print("\nFAIL", file=sys.stderr)
            return False


def explain_id(reply):
    """Turn an ID reply into something a student can act on."""
    if reply.startswith("ERR NO_TARGET"):
        print("No CPLD detected.", file=sys.stderr)
        print("  - Check TCK/TMS/TDI/TDO are not swapped (TDI and TDO are "
              "the easy pair to get backwards)", file=sys.stderr)
        print("  - Check the target board has power", file=sys.stderr)
        print("  - Check GND is connected between the Pico and the board",
              file=sys.stderr)
        return False
    print(reply)
    return True


def main():
    ap = argparse.ArgumentParser(
        description="Program a MAX V CPLD through a Raspberry Pi Pico.")
    ap.add_argument("svf", nargs="?", help="SVF file to program")
    ap.add_argument("-p", "--port", help="Serial port (auto-detected if omitted)")
    ap.add_argument("--id", action="store_true",
                    help="Just read the IDCODE and exit")
    ap.add_argument("--info", action="store_true",
                    help="Show firmware info and exit")
    ap.add_argument("--diag", action="store_true",
                    help="Run JTAG diagnostics and exit")
    ap.add_argument("--list", action="store_true",
                    help="List serial ports and exit")
    ap.add_argument("--slow", action="store_true",
                    help="Slow the JTAG clock — try this if programming "
                         "fails intermittently on long jumper wires")
    ap.add_argument("--speed-us", type=int, default=None,
                    help="Explicit per-edge delay in microseconds")
    ap.add_argument("--no-check", action="store_true",
                    help="Skip the IDCODE check before programming")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return 0

    if not args.svf and not (args.id or args.info or args.diag):
        ap.error("give an SVF file, or use --id / --info / --diag / --list")

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
        pong = prog.ping()
        if not pong.startswith("PONG"):
            print(f"No response from {port} — is this the right device?",
                  file=sys.stderr)
            return 1
        print(f"Connected on {port} ({pong})")

        if args.info:
            prog.command("INFO")
            print(prog.readline())
            return 0

        if args.diag:
            # Four independent checks. Reading them together is the point:
            # they use different registers and instructions, so agreement
            # between them is evidence the TAP itself is sound.
            prog.command("DIAG")
            prog.ser.timeout = 30
            ok = True
            while True:
                line = prog.readline()
                if not line:
                    print("Timed out.", file=sys.stderr)
                    return 1
                print("  " + line)
                if "BAD" in line or "unexpected" in line:
                    ok = False
                if line.startswith("DIAG done"):
                    break
            print("\nAll checks passed." if ok else
                  "\nSomething is off - see above.")
            return 0 if ok else 1

        delay = args.speed_us if args.speed_us is not None else (2 if args.slow else 0)
        if delay:
            prog.set_speed(delay)
            print(f"JTAG edge delay set to {delay} us")

        if args.id:
            return 0 if explain_id(prog.read_id()) else 1

        # Check the target is there before sending a few hundred KB at it.
        if not args.no_check:
            if not explain_id(prog.read_id()):
                return 1

        try:
            with open(args.svf, "rb") as f:
                data = f.read()
        except OSError as e:
            print(f"Cannot read {args.svf}: {e}", file=sys.stderr)
            return 1

        print(f"Sending {args.svf} ({len(data)} bytes)")
        return 0 if prog.program(data) else 1

    finally:
        prog.close()


if __name__ == "__main__":
    sys.exit(main())
