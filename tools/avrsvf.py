#!/usr/bin/env python3
"""
avrsvf.py — build an SVF that programs an ATmega32A over JTAG.

WHY THIS EXISTS

The Pico firmware already contains a proven SVF player, used for the MAX V
CPLD boards. `cmd_svf()` never looks at the IDCODE — it just executes
statements. So if the *host* turns an Intel HEX file into an SVF, the AVR
can be programmed over JTAG with no new firmware driver at all: the same
code path that programs a CPLD programs the microcontroller.

That is why this is a host-side generator rather than an `avr_jtag.c`
alongside `avr.c`. The JTAG bit-banging, the TAP state machine, the TDO
compares and the RUNTEST waits are all code that is already known to work
on real silicon.

CREDIT

The structure of the generated file — the order of operations, the
page-load trick, the RUNTEST placement — follows Andreas Schweizer's
avrsvf0 (https://github.com/SvenRoederer/avrsvf0, GPL-2.0), which is the
reference for doing this at all. avrsvf0 targets the ATmega16/128/1284P/
169P and not this part, so every command word below was re-checked against
the ATmega32A's own datasheet rather than carried across:

    ATmega32A, DS40002072A, Table 27-15 "JTAG Programming Instruction Set"
    (section 27.10, "Programming via the JTAG Interface")

They turned out to agree with the ATmega16 values avrsvf0 uses, which is
reassuring but was not safe to assume — see CLAUDE.md on not reconstructing
a programming table from a neighbouring part.

WHAT IS DELIBERATE HERE

  * The generated SVF checks the IDCODE and the signature bytes before it
    erases anything. A file that lands on the wrong board should stop, not
    scribble.

  * Fuse writes refuse to unprogram JTAGEN. Over ISP that fuse is merely
    inconvenient; over JTAG it is the branch being sawn off — the write
    would take effect and kill the interface partway through the file.

  * Flash verify has two forms. See VERIFY MODES below.

VERIFY MODES

  "page"  One 1032-bit PROG_PAGEREAD compare per 128-byte page. Compact
          and quick, but the statement carries TDO and MASK vectors of
          258 hex digits each and comes to 537 characters — more than the
          512 the firmware's parser used to accept.

  "word"  Two 8-bit compares per flash word, five statements per word.
          Every statement is tiny, so this works against any firmware, but
          it costs roughly ten times the bytes over USB.

  "auto"  Pick "page" if the firmware says it can take the statement
          (INFO reports max_stmt_bytes), otherwise "word". This is what
          the callers use, so an older Pico degrades to the slow path
          instead of failing with a TOO_LONG a student cannot act on.
"""

import argparse
import os
import sys


class SvfError(Exception):
    """Something about the request cannot safely be turned into an SVF."""


# ---------------------------------------------------------------------
# Device
#
# One entry per part, and every field has to come from that part's own
# datasheet. The AVR JTAG programming table is NOT common across the
# family — page size and the presence of an extended fuse byte both vary,
# and getting either wrong corrupts flash rather than failing cleanly.
# ---------------------------------------------------------------------

class Device:
    def __init__(self, name, signature, flash, page, idcode, extended_fuse):
        self.name = name
        self.signature = signature      # the three signature bytes
        self.flash = flash              # bytes
        self.page = page                # bytes per flash page
        self.idcode = idcode            # JTAG IDCODE, version nibble zeroed
        self.extended_fuse = extended_fuse

    @property
    def pages(self):
        return self.flash // self.page


# ATmega32A, DS40002072A:
#   signature       1E 95 02        (section 27.6)
#   flash / page    32 KB, 64 words = 128 bytes per page   (Table 27-5)
#   IDCODE          version:4 | part 0x9502 | manufacturer 0x01F | 1
#                                                   (Tables 25-1, 25-2)
#   extended fuse   none — this part has only a high and a low fuse byte
ATMEGA32A = Device(
    name="ATmega32A",
    signature=(0x1E, 0x95, 0x02),
    flash=32768,
    page=128,
    idcode=0x0950203F,
    extended_fuse=False,
)

# The version/revision nibble counts up with each silicon revision, so it
# must not be part of the comparison or a later stepping stops working.
IDCODE_MASK = 0x0FFFFFFF

DEVICES = {"atmega32a": ATMEGA32A}


# ---------------------------------------------------------------------
# JTAG instructions — 4-bit instruction register (section 27.10)
# ---------------------------------------------------------------------

IR_PROG_ENABLE   = 0x4
IR_PROG_COMMANDS = 0x5
IR_PROG_PAGELOAD = 0x6
IR_PROG_PAGEREAD = 0x7
IR_AVR_RESET     = 0xC

# Shifted into the 16-bit Programming Enable Register to unlock
# programming; binary 1010_0011_0111_0000 (section 27.10.9).
PROG_ENABLE_SIGNATURE = 0xA370


# ---------------------------------------------------------------------
# Programming commands — 15-bit Programming Command Register
#
# Table 27-15. Written here as the 4 hex digits SVF wants, so they can be
# read straight off against the datasheet's "TDI sequence" column: the
# table's 0100011_10000000 is this file's 0x2380.
# ---------------------------------------------------------------------

CHIP_ERASE           = (0x2380, 0x3180, 0x3380, 0x3380)   # 1a
ENTER_FLASH_WRITE    = 0x2310                             # 2a
LOAD_ADDR_HIGH       = 0x0700                             # 2b/3b  | addr
LOAD_ADDR_LOW        = 0x0300                             # 2c/3c  | addr
LOAD_DATA_LOW        = 0x1300                             # 2d/6b  | data
LOAD_DATA_HIGH       = 0x1700                             # 2e     | data
LATCH_DATA           = (0x3700, 0x7700, 0x3700)           # 2f
WRITE_FLASH_PAGE     = (0x3700, 0x3500, 0x3700, 0x3700)   # 2g
ENTER_FLASH_READ     = 0x2302                             # 3a
READ_DATA_SETUP      = 0x3200                             # 3d, first word
READ_DATA_LOW        = 0x3600                             # 3d, low byte out
READ_DATA_HIGH       = 0x3700                             # 3d, high byte out
ENTER_FUSE_WRITE     = 0x2340                             # 6a
WRITE_FUSE_HIGH      = (0x3700, 0x3500, 0x3700, 0x3700)   # 6c
WRITE_FUSE_LOW       = (0x3300, 0x3100, 0x3300, 0x3300)   # 6f
ENTER_FUSE_READ      = 0x2304                             # 8a
READ_FUSE_HIGH       = (0x3E00, 0x3F00)                   # 8b
READ_FUSE_LOW        = (0x3200, 0x3300)                   # 8c
ENTER_SIGNATURE_READ = 0x2308                             # 9a
READ_SIGNATURE       = (0x3200, 0x3300)                   # 9c
NO_OPERATION         = (0x2300, 0x3300)                   # 11a


# ---------------------------------------------------------------------
# Waits
#
# Table 27-11 gives tWLRH (flash page write, fuse write) a maximum of
# 4.5 ms and tWLRH_CE (chip erase) a maximum of 9 ms on this part. The
# values used are avrsvf0's, which are longer than both.
#
# That slack is deliberate and matches how RUNTEST is treated everywhere
# else in this project: overshooting one of these costs milliseconds,
# undershooting truncates a flash write and fails intermittently in a way
# that looks like bad wiring.
# ---------------------------------------------------------------------

WAIT_PAGE_WRITE_SEC = 7e-3
WAIT_CHIP_ERASE_SEC = 13e-3


# The parser's statement cap before SVF_MAX_STMT was raised. A firmware
# that does not report max_stmt_bytes in its INFO reply predates the
# change, so this is what it can be assumed to accept.
LEGACY_MAX_STMT_BYTES = 512

# What current firmware reports. Kept in step with SVF_MAX_STMT in
# firmware/svf.h; used as a default when there is no board to ask.
SVF_STMT_LIMIT = 1024


class _Svf:
    """Accumulates statements, refusing to emit one too long to be played.

    The cap is checked here rather than trusted, because an over-long
    statement is not something a bench run diagnoses well: the firmware
    reports TOO_LONG against a statement number, halfway through a file,
    which reads like a corrupt upload. Failing at generation time names
    the actual cause instead.
    """

    def __init__(self, max_stmt_bytes):
        self.lines = []
        self.max_stmt_bytes = max_stmt_bytes

    def comment(self, text):
        self.lines.append("// " + text)

    def blank(self):
        self.lines.append("")

    def stmt(self, text):
        # The firmware squeezes runs of whitespace out as it accumulates,
        # and stops one short of the buffer, so this is the length that
        # actually has to fit.
        if len(text) > self.max_stmt_bytes - 1:
            raise SvfError(
                f"generated a {len(text)}-byte statement but the "
                f"programmer accepts {self.max_stmt_bytes - 1}: {text[:60]}...")
        self.lines.append(text + ";")

    # -- shorthands ----------------------------------------------------

    def sir(self, ir):
        self.stmt("SIR 4 TDI(%x)" % ir)

    def cmd(self, word):
        """One 15-bit programming command, result ignored."""
        self.stmt("SDR 15 TDI(%04x)" % word)

    def cmds(self, words):
        for w in words:
            self.cmd(w)

    def cmd_expect(self, word, expect):
        """One programming command whose low byte is compared against TDO."""
        self.stmt("SDR 15 TDI(%04x) TDO(00%02x) MASK(00ff)" % (word, expect))

    def runtest(self, seconds):
        # Parked in Run-Test/Idle explicitly. The bare "RUNTEST n SEC" form
        # avrsvf0 emits means the same thing, but naming the state makes
        # the file readable without knowing the default.
        self.stmt("RUNTEST IDLE %.1fE-3 SEC" % (seconds * 1e3))

    def text(self):
        return "\n".join(self.lines) + "\n"


def _fuse_write_blocked(hfuse):
    """Why this fuse value must not be written over JTAG, or None.

    JTAGEN is hfuse bit 6, active low: a 1 unprograms it and switches the
    JTAG interface off. Over ISP that is recoverable and the existing risk
    model treats it as a normal setting. Over JTAG it is not — the fuse
    takes effect immediately, so the remaining statements in the file are
    shifted into a TAP that has stopped listening. The student sees a
    mid-file verify failure and a board that no longer answers.

    Refused rather than confirmed, because unlike an external-clock
    setting there is no wiring change that recovers it: it has to be
    undone over ISP, which is a different cable.
    """
    if (hfuse >> 6) & 1:
        return ("This setting unprograms JTAGEN, which switches the JTAG "
                "interface off. Writing it over JTAG would cut the "
                "connection partway through, so it is refused here. Set it "
                "over ISP instead, where the board stays reachable.")
    return None


# ---------------------------------------------------------------------
# The sections, in the order avrsvf0 emits them
# ---------------------------------------------------------------------

def _preamble(s, device, check_idcode):
    s.stmt("TRST ABSENT")
    s.stmt("ENDIR IDLE")
    s.stmt("ENDDR IDLE")

    # Single device on the chain. The firmware rejects non-zero padding
    # rather than ignoring it, so stating zero explicitly is the honest
    # declaration that this file assumes nothing else is in the way.
    for reg in ("HIR", "HDR", "TIR", "TDR"):
        s.stmt(reg + " 0")

    s.stmt("STATE RESET")

    if check_idcode:
        # Test-Logic-Reset loads IDCODE into DR on every TAP, so this needs
        # no instruction shift. Doing it inside the file rather than only
        # over the serial protocol means the check travels with the SVF —
        # it stops a file aimed at the CPLD board from running here, and
        # vice versa, whoever plays it.
        s.comment("Is this an %s? Version nibble masked off." % device.name)
        s.stmt("SDR 32 TDO(%08x) MASK(%08x)" % (device.idcode, IDCODE_MASK))

    s.stmt("STATE IDLE")
    s.blank()

    s.comment("Hold the AVR in reset, then unlock programming")
    s.sir(IR_AVR_RESET)
    s.stmt("SDR 1 TDI(1)")
    s.sir(IR_PROG_ENABLE)
    s.stmt("SDR 16 TDI(%04x)" % PROG_ENABLE_SIGNATURE)
    s.blank()


def _postamble(s):
    s.comment("No-operation, then release programming and reset")
    s.sir(IR_PROG_COMMANDS)
    s.cmds(NO_OPERATION)

    # Clearing the enable register is not optional housekeeping: the
    # datasheet says it should always be reset when leaving programming
    # mode (section 27.10.9).
    s.sir(IR_PROG_ENABLE)
    s.stmt("SDR 16 TDI(0000)")
    s.sir(IR_AVR_RESET)
    s.stmt("SDR 1 TDI(0)")
    s.stmt("STATE RESET")


def _verify_signature(s, device):
    # ASCII only in generated output: the page emits the same file and
    # tests/test_svf_parity.py compares them byte for byte.
    s.comment("Signature bytes - expect %s" %
              " ".join("%02X" % b for b in device.signature))
    s.sir(IR_PROG_COMMANDS)
    s.cmd(ENTER_SIGNATURE_READ)
    for i, byte in enumerate(device.signature):
        s.cmd(LOAD_ADDR_LOW | i)
        s.cmd(READ_SIGNATURE[0])
        s.cmd_expect(READ_SIGNATURE[1], byte)
    s.blank()


def _erase(s):
    s.comment("Chip erase")
    s.sir(IR_PROG_COMMANDS)
    s.cmds(CHIP_ERASE)
    s.runtest(WAIT_CHIP_ERASE_SEC)
    s.blank()


def _write_flash(s, device, image, skip_blank_pages):
    """Load each page through PROG_PAGELOAD, then write it.

    The byte-at-a-time alternative (2d/2e/2f per word) is three statements
    per word against one shift per page, so this is the form worth having.
    """
    pages = _split_pages(device, image)

    s.comment("Program %d of %d flash pages" % (
        sum(1 for _, data in pages if not _blank(data) or not skip_blank_pages),
        device.pages))
    s.sir(IR_PROG_COMMANDS)
    s.cmd(ENTER_FLASH_WRITE)

    for byte_addr, data in pages:
        # A chip-erased device already reads 0xFF everywhere, so a page
        # that is entirely 0xFF has nothing to do. The datasheet says as
        # much (section 27.9.2). Only safe because we erased: with the
        # erase skipped, the old contents are still there.
        if skip_blank_pages and _blank(data):
            continue

        # PCWORD must be zero, i.e. the address is the page's first word.
        word_addr = byte_addr // 2
        s.cmd(LOAD_ADDR_HIGH | ((word_addr >> 8) & 0xFF))
        s.cmd(LOAD_ADDR_LOW | (word_addr & 0xFF))

        s.sir(IR_PROG_PAGELOAD)
        s.stmt("SDR %d TDI(%s)" % (8 * device.page, _vector(data)))
        s.sir(IR_PROG_COMMANDS)

        s.cmds(WRITE_FLASH_PAGE)
        s.runtest(WAIT_PAGE_WRITE_SEC)

    s.blank()


def _verify_flash_page(s, device, image):
    """Compare a whole page per statement, using PROG_PAGEREAD."""
    s.comment("Verify flash, one page per compare")
    s.sir(IR_PROG_COMMANDS)
    s.cmd(ENTER_FLASH_READ)

    nbits = 8 * device.page + 8
    # The first 8 bits shifted out are undefined and must be ignored
    # (section 27.10.18), which is what the trailing 00 in the mask does —
    # it sits at the LSB end, and the LSB is what comes out first.
    mask = "ff" * device.page + "00"

    for byte_addr, data in _split_pages(device, image):
        word_addr = byte_addr // 2
        s.cmd(LOAD_ADDR_HIGH | ((word_addr >> 8) & 0xFF))
        s.cmd(LOAD_ADDR_LOW | (word_addr & 0xFF))

        s.sir(IR_PROG_PAGEREAD)
        # TDI is omitted rather than padded: this is a virtual read chain
        # and nothing is shifted in, so the 258 hex digits avrsvf0 spends
        # on all-ones here buy nothing and would push the statement well
        # past what the parser accepts.
        s.stmt("SDR %d TDO(%sff) MASK(%s)" % (nbits, _vector(data), mask))
        s.sir(IR_PROG_COMMANDS)

    s.blank()


def _verify_flash_word(s, device, image):
    """Compare two bytes at a time, for firmware with a small statement cap."""
    s.comment("Verify flash, one word per compare")
    s.sir(IR_PROG_COMMANDS)
    s.cmd(ENTER_FLASH_READ)

    # Only the pages we actually programmed are worth reading back, and
    # the image is already trimmed to the last used byte, so a small
    # program costs a small verify.
    padded = _pad(device, image)
    high = None

    for byte_addr in range(0, len(padded), 2):
        word_addr = byte_addr // 2
        want_high = (word_addr >> 8) & 0xFF

        # Note 1 to Table 27-15: a command may be skipped when the seven
        # most significant bits are already right from the last one. The
        # high address byte only moves every 256 words.
        if want_high != high:
            s.cmd(LOAD_ADDR_HIGH | want_high)
            high = want_high

        s.cmd(LOAD_ADDR_LOW | (word_addr & 0xFF))
        s.cmd(READ_DATA_SETUP)
        s.cmd_expect(READ_DATA_LOW, padded[byte_addr])
        s.cmd_expect(READ_DATA_HIGH, padded[byte_addr + 1])

    s.blank()


def _write_fuses(s, device, lfuse, hfuse):
    blocked = _fuse_write_blocked(hfuse)
    if blocked:
        raise SvfError(blocked)

    s.comment("Fuses: low 0x%02X, high 0x%02X  (0 = programmed)" %
              (lfuse, hfuse))
    s.sir(IR_PROG_COMMANDS)
    s.cmd(ENTER_FUSE_WRITE)

    if device.extended_fuse:
        raise SvfError(
            "%s has an extended fuse byte and this generator does not "
            "write one." % device.name)

    # High byte first, then low, each followed by its own wait. Both use
    # instruction 6b to load, and differ only in the write strobe.
    s.cmd(LOAD_DATA_LOW | hfuse)
    s.cmds(WRITE_FUSE_HIGH)
    s.runtest(WAIT_PAGE_WRITE_SEC)

    s.cmd(LOAD_DATA_LOW | lfuse)
    s.cmds(WRITE_FUSE_LOW)
    s.runtest(WAIT_PAGE_WRITE_SEC)
    s.blank()


def _verify_fuses(s, device, lfuse, hfuse):
    s.comment("Read the fuses back")
    s.sir(IR_PROG_COMMANDS)
    s.cmd(ENTER_FUSE_READ)
    s.cmd(READ_FUSE_HIGH[0])
    s.cmd_expect(READ_FUSE_HIGH[1], hfuse)
    s.cmd(READ_FUSE_LOW[0])
    s.cmd_expect(READ_FUSE_LOW[1], lfuse)
    s.blank()


# ---------------------------------------------------------------------
# Image helpers
# ---------------------------------------------------------------------

def _pad(device, image):
    """Round the image up to a whole number of pages, filling with 0xFF.

    0xFF is the erased state, so padding a part-used final page writes
    nothing that was not already there.
    """
    if len(image) > device.flash:
        raise SvfError(
            "This program is %d bytes, more than the %s's %d bytes of "
            "flash." % (len(image), device.name, device.flash))
    short = (-len(image)) % device.page
    return bytes(image) + b"\xFF" * short


def _split_pages(device, image):
    padded = _pad(device, image)
    return [(a, padded[a:a + device.page])
            for a in range(0, len(padded), device.page)]


def _blank(data):
    return all(b == 0xFF for b in data)


def _vector(data):
    """Page bytes as an SVF hex vector.

    Reversed, because SVF writes a vector most significant bit first while
    the page is shifted in least significant bit first — "starting with the
    LSB of the first instruction in the page" (section 27.10.11). So the
    last byte of the page has to appear at the left-hand end of the string
    for the first byte to arrive first.
    """
    return bytes(reversed(data)).hex()


def choose_verify_mode(max_stmt_bytes, device=ATMEGA32A):
    """The compact verify if it will fit, otherwise the bulky one."""
    needed = len("SDR %d TDO(%sff) MASK(%s)" % (
        8 * device.page + 8, "0" * 2 * device.page, "f" * (2 * device.page + 2)))
    return "page" if max_stmt_bytes - 1 >= needed else "word"


# ---------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------

def build_svf(image=None, device=ATMEGA32A, erase=True, check_idcode=True,
              check_signature=True, write_flash=True, verify_flash=True,
              fuses=None, verify_fuses=True, verify_mode="auto",
              max_stmt_bytes=LEGACY_MAX_STMT_BYTES, note=None):
    """Return SVF text for the requested operations.

    `image` is a flat binary starting at address 0, as parse_hex() returns
    it — trimmed to the last used byte, not padded to 32 KB. `fuses` is
    (lfuse, hfuse) or None.
    """
    if verify_mode == "auto":
        verify_mode = choose_verify_mode(max_stmt_bytes, device)
    if verify_mode not in ("page", "word", "none"):
        raise SvfError("unknown verify mode %r" % (verify_mode,))

    if image is None and (write_flash or verify_flash):
        write_flash = verify_flash = False

    s = _Svf(max_stmt_bytes)
    # Kept free of any tool name or timestamp so the browser and the CLI
    # produce byte-identical files. tests/test_svf_parity.py compares them,
    # and that check is worth more than a provenance line.
    s.comment("%s over JTAG, generated from an Intel HEX file." % device.name)
    s.comment("Command words: %s datasheet DS40002072A, Table 27-15." %
              device.name)
    s.comment("Layout after avrsvf0 by Andreas Schweizer (GPL-2.0).")
    if note:
        s.comment(note)
    if image is not None:
        s.comment("Program: %d bytes, %d of %d pages used." % (
            len(image), (len(image) + device.page - 1) // device.page,
            device.pages))
    s.blank()

    _preamble(s, device, check_idcode)

    # Before anything destructive, as everywhere else in this project.
    if check_signature:
        _verify_signature(s, device)

    if erase:
        _erase(s)

    if write_flash:
        _write_flash(s, device, image, skip_blank_pages=erase)

    if verify_flash and verify_mode != "none":
        if verify_mode == "page":
            _verify_flash_page(s, device, image)
        else:
            _verify_flash_word(s, device, image)

    if fuses is not None:
        _write_fuses(s, device, fuses[0], fuses[1])
        if verify_fuses:
            _verify_fuses(s, device, fuses[0], fuses[1])

    _postamble(s)
    return s.text()


# ---------------------------------------------------------------------
# Standalone use
#
# tools/avr.py --jtag is the normal way in. This exists so an SVF can be
# written to a file and read, diffed or replayed with maxv.py, which is
# how you check a change to this file without a board on the bench.
# ---------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Build an SVF that programs an ATmega32A over JTAG.")
    ap.add_argument("hex", nargs="?", help="Intel HEX file to convert")
    ap.add_argument("-o", "--out", required=True, help="SVF file to write")
    ap.add_argument("--no-erase", action="store_true")
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--verify-only", action="store_true",
                    help="Compare against the file without changing the chip")
    ap.add_argument("--verify-mode", choices=("auto", "page", "word"),
                    default="page")
    ap.add_argument("--max-stmt-bytes", type=int, default=1024)
    ap.add_argument("--lfuse", type=lambda v: int(v, 0))
    ap.add_argument("--hfuse", type=lambda v: int(v, 0))
    args = ap.parse_args()

    # Reuse the page's HEX parser rather than porting avrsvf0's, which
    # accepts no checksums and rejects records this toolchain emits.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from avr import parse_hex, HexError

    image = None
    if args.hex:
        try:
            with open(args.hex) as f:
                image, _ = parse_hex(f.read())
        except (OSError, HexError) as e:
            print(e, file=sys.stderr)
            return 1

    fuses = None
    if args.lfuse is not None or args.hfuse is not None:
        if args.lfuse is None or args.hfuse is None:
            print("--lfuse and --hfuse go together.", file=sys.stderr)
            return 1
        fuses = (args.lfuse, args.hfuse)

    try:
        svf = build_svf(
            image=image,
            erase=not (args.no_erase or args.verify_only),
            write_flash=not args.verify_only,
            verify_flash=not args.no_verify,
            fuses=fuses,
            verify_mode="none" if args.no_verify else args.verify_mode,
            max_stmt_bytes=args.max_stmt_bytes,
        )
    except SvfError as e:
        print(e, file=sys.stderr)
        return 1

    with open(args.out, "w") as f:
        f.write(svf)
    print("Wrote %s (%d bytes, %d statements)" %
          (args.out, len(svf), svf.count(";")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
