#!/usr/bin/env python3
"""
test_avr_jtag.py — play a generated SVF against a simulated ATmega32A.

WHY A SIMULATOR RATHER THAN CHECKING THE TEXT

Reading the generated file and asserting it contains the right hex strings
would pass just as happily against a file that programs the wrong address,
loads a page backwards, or verifies bytes it never wrote. The interesting
failures are all semantic.

So this builds a model of the AVR's JTAG programming interface out of the
datasheet's instruction table (ATmega32A DS40002072A, Table 27-15), plays
the generated SVF into it the way the firmware would, and then asks the
only question worth asking: does the model's flash now hold the program?

The SVF player here deliberately matches the subset firmware/svf.c
implements, including its statement-length limit, so a file that passes
here is one that Pico can actually play.

    python3 test_avr_jtag.py
"""

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))

import avrsvf                                          # noqa: E402
from avr import parse_hex                              # noqa: E402


# ---------------------------------------------------------------------
# The model
# ---------------------------------------------------------------------

class AvrJtagError(Exception):
    pass


class SimAvr:
    """An ATmega32A as seen through its JTAG port, while being programmed."""

    # Modes set by the "enter X" commands. Several command words mean
    # different things depending on which one is active — 0x3300 reads a
    # signature byte in one mode and a fuse byte in another — so the mode
    # is what disambiguates, exactly as it does in the silicon.
    IDLE, ERASE, FLASH_WRITE, FLASH_READ, FUSE_WRITE, FUSE_READ, SIG_READ = range(7)

    PAGE = 128
    FLASH = 32768

    def __init__(self, idcode=0x0950203F, signature=(0x1E, 0x95, 0x02)):
        self.idcode = idcode
        self.signature = signature
        self.flash = bytearray(b"\xFF" * self.FLASH)
        self.lfuse = 0xE1               # factory default: internal 1 MHz
        self.hfuse = 0x99
        self.reset = False
        self.enabled = False
        self.ir = 0x0                   # after TLR the TAP loads IDCODE
        self.after_reset = True
        self.mode = self.IDLE
        self.addr = 0
        self.data_low = 0
        self.page_buf = bytearray(b"\xFF" * self.PAGE)
        self.pending = []               # strobe sequence seen so far
        self.erased = False
        self.pages_written = 0

    # -- TAP -----------------------------------------------------------

    def shift_ir(self, value, nbits):
        if nbits != 4:
            raise AvrJtagError(f"IR is 4 bits on this part, got {nbits}")
        self.ir = value & 0xF
        self.after_reset = False
        # Capture-IR always loads 0b0001 on an IEEE 1149.1 TAP.
        return 0x1

    def shift_dr(self, value, nbits):
        ir = 0x1 if self.after_reset else self.ir      # IDCODE after TLR

        if ir == 0x1 or (self.after_reset and ir == 0x0):
            if nbits != 32:
                raise AvrJtagError(f"IDCODE is 32 bits, got {nbits}")
            return self.idcode

        if ir == avrsvf.IR_AVR_RESET:
            if nbits != 1:
                raise AvrJtagError(f"Reset Register is 1 bit, got {nbits}")
            self.reset = bool(value & 1)
            if not self.reset:
                self.mode = self.IDLE
            return 0

        if ir == avrsvf.IR_PROG_ENABLE:
            if nbits != 16:
                raise AvrJtagError(
                    f"Programming Enable Register is 16 bits, got {nbits}")
            self.enabled = (value == avrsvf.PROG_ENABLE_SIGNATURE)
            return 0

        if ir == avrsvf.IR_PROG_COMMANDS:
            if nbits != 15:
                raise AvrJtagError(
                    f"Programming Command Register is 15 bits, got {nbits}")
            return self._command(value)

        if ir == avrsvf.IR_PROG_PAGELOAD:
            return self._pageload(value, nbits)

        if ir == avrsvf.IR_PROG_PAGEREAD:
            return self._pageread(nbits)

        raise AvrJtagError(f"unsupported JTAG instruction 0x{ir:X}")

    def tap_reset(self):
        self.after_reset = True
        self.ir = 0x1

    # -- Programming commands ------------------------------------------

    def _require(self, what):
        if not self.reset:
            raise AvrJtagError(f"{what} while the AVR is not held in reset")
        if not self.enabled:
            raise AvrJtagError(f"{what} before programming was enabled")

    def _command(self, cmd):
        self._require("programming command")
        high, low = cmd >> 8, cmd & 0xFF

        # "Enter X" commands, 0x23xx / 0x21xx family (instructions Na).
        if high == 0x23:
            self.pending = []
            self.mode = {
                0x80: self.ERASE,
                0x10: self.FLASH_WRITE,
                0x02: self.FLASH_READ,
                0x40: self.FUSE_WRITE,
                0x04: self.FUSE_READ,
                0x08: self.SIG_READ,
                0x00: self.IDLE,        # 11a, no-operation
            }.get(low, None)
            if self.mode is None:
                raise AvrJtagError(f"unknown enter-command 0x{cmd:04X}")
            return 0

        if high == 0x07:                        # load address high byte
            self.addr = (self.addr & 0x00FF) | (low << 8)
            return 0
        if high == 0x03:                        # load address low byte
            self.addr = (self.addr & 0xFF00) | low
            return 0
        if high == 0x13:                        # load data low byte
            self.data_low = low
            return 0

        return self._strobe(cmd)

    def _strobe(self, cmd):
        """The 0x3n00 / 0x7700 words that clock an operation along."""
        m = self.mode

        if m == self.ERASE:
            self.pending.append(cmd)
            if self.pending == [0x3180, 0x3380, 0x3380]:
                self.flash = bytearray(b"\xFF" * self.FLASH)
                self.lfuse, self.hfuse = self.lfuse, self.hfuse
                self.erased = True
                self.pending = []
            return 0

        if m == self.FLASH_WRITE:
            self.pending.append(cmd)
            # 2g: write the loaded page. The leading 0x3700 is shared with
            # 2f's latch, so the sequence is matched rather than the word.
            if self.pending[-4:] == [0x3700, 0x3500, 0x3700, 0x3700]:
                self._write_page()
                self.pending = []
            elif len(self.pending) > 8:
                self.pending = self.pending[-4:]
            return 0

        if m == self.FLASH_READ:
            if cmd == 0x3200:
                return 0
            if cmd == 0x3600:
                return self._flash_byte(self.addr * 2)
            if cmd == 0x3700:
                return self._flash_byte(self.addr * 2 + 1)
            raise AvrJtagError(f"unexpected flash-read command 0x{cmd:04X}")

        if m == self.SIG_READ:
            if cmd == 0x3200:
                return 0
            if cmd == 0x3300:
                # Instruction 9b loads only the low address byte, and Note 9
                # makes the bits above it don't-care. So whatever the high
                # byte was left holding by an earlier flash operation is
                # ignored here, as it is in the silicon.
                index = self.addr & 0xFF
                if index >= len(self.signature):
                    raise AvrJtagError(f"signature index {index}")
                return self.signature[index]
            raise AvrJtagError(f"unexpected signature command 0x{cmd:04X}")

        if m == self.FUSE_READ:
            return {0x3E00: 0, 0x3F00: self.hfuse,
                    0x3200: 0, 0x3300: self.lfuse}.get(cmd, None) \
                if cmd in (0x3E00, 0x3F00, 0x3200, 0x3300) \
                else self._bad(cmd)

        if m == self.FUSE_WRITE:
            self.pending.append(cmd)
            if self.pending[-4:] == [0x3700, 0x3500, 0x3700, 0x3700]:
                self.hfuse = self.data_low
                self.pending = []
            elif self.pending[-4:] == [0x3300, 0x3100, 0x3300, 0x3300]:
                self.lfuse = self.data_low
                self.pending = []
            return 0

        if m == self.IDLE and cmd == 0x3300:    # 11a's second word
            return 0

        return self._bad(cmd)

    def _bad(self, cmd):
        raise AvrJtagError(
            f"command 0x{cmd:04X} in mode {self.mode} is not in Table 27-15")

    def _flash_byte(self, byte_addr):
        if byte_addr >= self.FLASH:
            raise AvrJtagError(f"read past end of flash at 0x{byte_addr:X}")
        return self.flash[byte_addr]

    def _write_page(self):
        base = self.addr * 2
        if base % self.PAGE:
            raise AvrJtagError(
                f"page write to 0x{base:X}, which is not a page boundary "
                "(PCWORD must be zero)")
        if base + self.PAGE > self.FLASH:
            raise AvrJtagError(f"page write past end of flash at 0x{base:X}")
        # Flash programming can only clear bits; only an erase sets them.
        # Modelled honestly so a file that forgets to erase fails here.
        for i in range(self.PAGE):
            self.flash[base + i] &= self.page_buf[i]
        self.page_buf = bytearray(b"\xFF" * self.PAGE)
        self.pages_written += 1

    def _pageload(self, value, nbits):
        self._require("page load")
        if nbits != 8 * self.PAGE:
            raise AvrJtagError(
                f"PROG_PAGELOAD is {8 * self.PAGE} bits, got {nbits}")
        # Shifted in LSB first, so the low byte of the vector is the first
        # byte of the page.
        for i in range(self.PAGE):
            self.page_buf[i] = (value >> (8 * i)) & 0xFF
        return 0

    def _pageread(self, nbits):
        self._require("page read")
        if nbits != 8 * self.PAGE + 8:
            raise AvrJtagError(
                f"PROG_PAGEREAD is {8 * self.PAGE + 8} bits, got {nbits}")
        base = self.addr * 2
        if base % self.PAGE:
            raise AvrJtagError(f"page read from 0x{base:X}, not page-aligned")
        out = 0
        for i in range(self.PAGE):
            out |= self.flash[base + i] << (8 * (i + 1))
        return out                       # low 8 bits are the dummy byte


# ---------------------------------------------------------------------
# An SVF player matching what firmware/svf.c accepts
# ---------------------------------------------------------------------

class SvfFailure(Exception):
    pass


def play(svf, dev, max_stmt_bytes=avrsvf.SVF_STMT_LIMIT):
    """Execute an SVF against `dev`. Returns the statement count."""
    text = re.sub(r"//[^\n]*", " ", svf)
    text = re.sub(r"![^\n]*", " ", text)

    count = 0
    for raw in text.split(";"):
        stmt = " ".join(raw.split())
        if not stmt:
            continue
        count += 1

        if len(stmt) > max_stmt_bytes - 1:
            raise SvfFailure(
                f"statement {count} is {len(stmt)} bytes, more than the "
                f"{max_stmt_bytes - 1} the firmware accepts")

        word = stmt.split()[0].upper()
        body = stmt[len(word):].strip()

        if word in ("SIR", "SDR"):
            _shift(word, body, dev, count)
        elif word == "STATE":
            if "RESET" in body.upper():
                dev.tap_reset()
        elif word in ("HIR", "HDR", "TIR", "TDR"):
            if int(body.split()[0]) != 0:
                raise SvfFailure(f"statement {count}: multi-device chain")
        elif word in ("TRST", "ENDIR", "ENDDR", "RUNTEST", "FREQUENCY"):
            pass
        else:
            raise SvfFailure(f"statement {count}: unsupported command {word}")
    return count


def _field(body, name):
    m = re.search(name + r"\s*\(([^)]*)\)", body)
    if not m:
        return None
    return int(re.sub(r"\s+", "", m.group(1)) or "0", 16)


def _shift(word, body, dev, count):
    nbits = int(body.split()[0])
    if nbits > 4096:
        raise SvfFailure(f"statement {count}: {nbits} bits exceeds SVF_MAX_BITS")

    tdi = _field(body, "TDI") or 0
    tdo = _field(body, "TDO")
    mask = _field(body, "MASK")

    got = dev.shift_ir(tdi, nbits) if word == "SIR" else dev.shift_dr(tdi, nbits)
    got &= (1 << nbits) - 1

    if tdo is not None:
        m = mask if mask is not None else (1 << nbits) - 1
        if (got ^ tdo) & m:
            raise SvfFailure(
                f"statement {count}: TDO mismatch, got 0x{got & m:X} "
                f"expected 0x{tdo & m:X}")


# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

PASS, FAIL = 0, 0


def check(name, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  {name:<52} PASS")
    else:
        FAIL += 1
        print(f"  {name:<52} *** FAIL ***  {detail}")


def sample_image(n=1000):
    # Not random: a repeatable pattern makes a reported byte index mean
    # something when this fails.
    return bytes(((i * 37) ^ (i >> 5)) & 0xFF for i in range(n))


def programs_correctly(mode, image):
    dev = SimAvr()
    svf = avrsvf.build_svf(image=image, verify_mode=mode, max_stmt_bytes=1024)
    play(svf, dev)
    padded = image + b"\xFF" * ((-len(image)) % 128)
    return dev, svf, bytes(dev.flash[:len(padded)]) == padded


def main():
    print("\nProgramming a simulated ATmega32A over JTAG\n")
    image = sample_image()

    for mode in ("page", "word"):
        dev, svf, ok = programs_correctly(mode, image)
        check(f"{mode}-mode: flash matches the image", ok)
        check(f"{mode}-mode: chip was erased first", dev.erased)
        check(f"{mode}-mode: programming left disabled", not dev.enabled)
        check(f"{mode}-mode: AVR released from reset", not dev.reset)

    # The two verify strategies must agree about what is on the chip.
    # They express it completely differently, so this is a real check.
    dev_p, _, _ = programs_correctly("page", image)
    dev_w, _, _ = programs_correctly("word", image)
    check("page and word modes leave identical flash",
          dev_p.flash == dev_w.flash)

    # A chip holding the wrong program must fail the verify, in both
    # modes. Without this the verify could be vacuous and nothing else
    # here would notice.
    for mode in ("page", "word"):
        dev = SimAvr()
        svf = avrsvf.build_svf(image=image, verify_mode=mode,
                               max_stmt_bytes=1024)
        play(svf, dev)
        dev.flash[500] ^= 0xFF
        verify = avrsvf.build_svf(image=image, erase=False, write_flash=False,
                                  verify_mode=mode, max_stmt_bytes=1024)
        try:
            play(verify, dev)
            check(f"{mode}-mode: verify catches a changed byte", False,
                  "the corrupted byte passed")
        except SvfFailure:
            check(f"{mode}-mode: verify catches a changed byte", True)

    # ...and must pass against a chip that does hold it.
    for mode in ("page", "word"):
        dev, _, _ = programs_correctly(mode, image)
        verify = avrsvf.build_svf(image=image, erase=False, write_flash=False,
                                  verify_mode=mode, max_stmt_bytes=1024)
        try:
            play(verify, dev)
            check(f"{mode}-mode: verify-only passes on a good chip", True)
        except SvfFailure as e:
            check(f"{mode}-mode: verify-only passes on a good chip", False, e)

    # The signature check has to stop a file aimed at the wrong part.
    dev = SimAvr(signature=(0x1E, 0x94, 0x03))          # an ATmega16
    try:
        play(avrsvf.build_svf(image=image, max_stmt_bytes=1024), dev)
        check("wrong signature is refused", False, "it programmed anyway")
    except SvfFailure:
        check("wrong signature is refused", True)
        check("  ...and nothing was erased first", not dev.erased,
              "the erase happened before the check")

    # Likewise a different device on the chain entirely.
    dev = SimAvr(idcode=0x020A50DD)                     # a MAX V CPLD
    try:
        play(avrsvf.build_svf(image=image, max_stmt_bytes=1024), dev)
        check("wrong IDCODE is refused", False, "it programmed anyway")
    except SvfFailure:
        check("wrong IDCODE is refused", True)

    # The version nibble counts up with each silicon revision.
    dev = SimAvr(idcode=0x3950203F)
    try:
        play(avrsvf.build_svf(image=image, max_stmt_bytes=1024), dev)
        check("a later silicon revision still programs", True)
    except SvfFailure as e:
        check("a later silicon revision still programs", False, e)

    # Fuses.
    dev = SimAvr()
    play(avrsvf.build_svf(fuses=(0xE4, 0x99), max_stmt_bytes=1024), dev)
    check("fuse write lands and verifies",
          (dev.lfuse, dev.hfuse) == (0xE4, 0x99),
          f"got {dev.lfuse:02X}/{dev.hfuse:02X}")

    try:
        avrsvf.build_svf(fuses=(0xE4, 0xD9), max_stmt_bytes=1024)
        check("a fuse that kills JTAG is refused", False, "it was generated")
    except avrsvf.SvfError:
        check("a fuse that kills JTAG is refused", True)

    # Statement limits. The generator promises never to emit one the
    # firmware cannot take, so ask it for the impossible.
    try:
        avrsvf.build_svf(image=image, verify_mode="page", max_stmt_bytes=512)
        check("page mode is refused on a 512-byte parser", False,
              "it generated an unplayable file")
    except avrsvf.SvfError:
        check("page mode is refused on a 512-byte parser", True)

    svf = avrsvf.build_svf(image=image, verify_mode="word",
                           max_stmt_bytes=512)
    longest = max(len(" ".join(s.split()))
                  for s in re.sub(r"//[^\n]*", " ", svf).split(";"))
    check("word mode fits an unmodified 512-byte parser", longest <= 511,
          f"longest statement is {longest}")

    # A part-used final page is padded with the erased value, so the whole
    # page still verifies.
    odd = sample_image(129)
    dev, _, ok = programs_correctly("page", odd)
    check("a part-used final page verifies", ok)

    # An image that does not fit must be refused, not truncated.
    try:
        avrsvf.build_svf(image=bytes(33000), max_stmt_bytes=1024)
        check("an oversized image is refused", False, "it was generated")
    except avrsvf.SvfError:
        check("an oversized image is refused", True)

    # Real toolchain output, if a HEX file is sitting here.
    for name in ("sample.hex", "main.hex"):
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
        if os.path.exists(path):
            with open(path) as f:
                img, _ = parse_hex(f.read())
            dev, svf, ok = programs_correctly("page", img)
            check(f"real file {name} ({len(img)} bytes) programs", ok)
            break
    else:
        print("\n  (drop a sample.hex here to test against real toolchain "
              "output)")

    print(f"\n{PASS} passed, {FAIL} failed\n")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
