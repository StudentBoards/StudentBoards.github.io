"""Exercise avr.py's protocol handling and HEX parser against a fake Pico.

The AVR path is the one that has never run on hardware, so this leans on
the parts that can be checked without a board: that a HEX file becomes the
right bytes, that a fuse value is judged the same way the firmware judges
it, and that every reply the firmware can send is turned into the right
pass/fail.

Same shape as test_protocol.py, and like it this stubs out pyserial rather
than requiring it — nothing here opens a port.
"""
import sys, types, io, contextlib

sys.modules['serial'] = fake = types.ModuleType('serial')
sys.modules['serial.tools'] = types.ModuleType('serial.tools')
sys.modules['serial.tools.list_ports'] = lp = types.ModuleType('list_ports')
class SerialException(Exception): pass
fake.SerialException = SerialException

class FakePort:
    def __init__(self, script):
        self.script = list(script); self.timeout = 1; self.written = 0
    def reset_input_buffer(self): pass
    def write(self, d): self.written += len(d); return len(d)
    def flush(self): pass
    def readline(self):
        return (self.script.pop(0) + "\n").encode() if self.script else b''
    def close(self): pass

fake.Serial = lambda *a, **k: FakePort(fake._script)
lp.comports = lambda: []

import importlib.util
spec = importlib.util.spec_from_file_location("avr", "../tools/avr.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

allok = True

def quiet(fn, *a):
    """Run something that talks to the student, keeping the report readable.

    These functions exist to print advice, and that advice is checked by
    eye elsewhere — here only the verdict matters.
    """
    with contextlib.redirect_stdout(io.StringIO()), \
         contextlib.redirect_stderr(io.StringIO()):
        return fn(*a)

def check(name, got, want):
    global allok
    ok = got == want
    allok &= ok
    print(f"{name:<40} {str(got):<8} {'PASS' if ok else '*** FAIL ***'}")
    return ok


# ---- Intel HEX -------------------------------------------------------
# The same file must produce the same image here as it does in the browser,
# or the CLI and the page would program a board differently from each other.

def hexrec(rectype, addr, data):
    b = [len(data), (addr >> 8) & 0xFF, addr & 0xFF, rectype] + list(data)
    return ':' + ''.join('%02X' % x for x in b + [(-sum(b)) & 0xFF])

print("Intel HEX")
payload = bytes(range(256)) + b'\xAA\xBB\xCC\xDD'
lines = [hexrec(0, off, payload[off:off + 16])
         for off in range(0, len(payload), 16)] + [hexrec(1, 0, b'')]

img, top = m.parse_hex('\n'.join(lines))
check("  round-trips byte for byte", img == payload, True)
check("  reports the used length", top, len(payload))

# CRLF line endings: students on Windows produce these constantly.
img2, _ = m.parse_hex('\r\n'.join(lines) + '\r\n')
check("  tolerates CRLF", img2 == payload, True)

# A sparse file must still read back as an erased 0xFF between the blocks,
# because that is what the chip will hold after a chip erase.
sparse = m.parse_hex('\n'.join([hexrec(0, 0, b'\x01\x02'),
                                hexrec(0, 0x40, b'\x03'),
                                hexrec(1, 0, b'')]))[0]
check("  gaps read back as erased 0xFF",
      sparse[0:2] == b'\x01\x02' and set(sparse[2:0x40]) == {0xFF}, True)

def rejects(name, text):
    try:
        m.parse_hex(text)
        return check(name, "accepted", "rejected")
    except m.HexError:
        return check(name, "rejected", "rejected")

rejects("  rejects a bad checksum", lines[0][:-2] + 'FF')
rejects("  rejects a non-record line", 'hello')
rejects("  rejects a truncated record", ':0102')
rejects("  rejects non-hex digits", ':10000000' + 'ZZ' * 16 + '00')
rejects("  rejects an empty file", '')
# An extended-address record can push data past the end of a 32 KB part.
rejects("  rejects an oversize image",
        hexrec(4, 0, b'\x00\x01') + '\n' + hexrec(0, 0, b'\x01\x02'))


# ---- Fuse safety -----------------------------------------------------
# This mirrors avr_fuse_risk() in the firmware. The firmware's copy is the
# one that protects the chip; this one has to agree with it, or the CLI
# would refuse things the board allows, or worse, promise the opposite.

print("\nFuse risk")
check("  factory default is fine",       m.fuse_risk(0xE1, 0x99)[0], "fine")
check("  internal 8 MHz is fine",        m.fuse_risk(0xE4, 0x99)[0], "fine")
check("  external crystal needs confirm", m.fuse_risk(0xFF, 0x89)[0], "confirm")
check("  external clock needs confirm",  m.fuse_risk(0xE0, 0x99)[0], "confirm")
# SPIEN is hfuse bit 5, active low: a 1 switches ISP off for good.
check("  SPIEN unprogrammed is fatal",   m.fuse_risk(0xE1, 0xB9)[0], "fatal")
check("  fatal outranks confirm",        m.fuse_risk(0xFF, 0xB9)[0], "fatal")

# No shipped preset may be able to lock a student out. That is the whole
# reason the page offers presets instead of a hex box, so it is worth
# asserting rather than trusting.
worst = [n for n, (l, h, _) in m.FUSE_PRESETS.items()
         if m.fuse_risk(l, h)[0] == "fatal"]
check("  no preset can disable ISP", worst, [])


# ---- Serial protocol -------------------------------------------------
# Every reply cmd_avr_flash() and cmd_avr_verify() can produce, checked
# against the pass/fail the student ends up seeing.

print("\nAVRFLASH")
def flash(name, script, expect, data=b'\x01\x02\x03\x04'):
    fake._script = script
    p = m.Programmer("fake")
    return check(name, quiet(p.flash, data), expect)

flash("  programmed and verified",
      ["READY 4", "DONE bytes=4 pages=1 ms=4820"], True)
flash("  progress then done",
      ["READY 4", "PROGRESS 0", "PROGRESS 2048",
       "DONE bytes=4 pages=1 ms=4820"], True)
flash("  no target", ["READY 4", "ERR NO_TARGET"], False)
flash("  wrong chip",
      ["READY 4", "ERR SIGNATURE 0x1E9403 is not an ATmega32A"], False)
flash("  verify failure",
      ["READY 4", "ERR VERIFY mismatch at byte 118"], False)
flash("  image too big", ["ERR TOO_BIG 40000 bytes exceeds 32768"], False)
flash("  silent device -> timeout", ["READY 4"], False)

print("\nAVRVERIFY")
def verify(name, script, expect, data=b'\x01\x02\x03\x04'):
    fake._script = script
    p = m.Programmer("fake")
    return check(name, quiet(p.verify, data), expect)

verify("  flash matches", ["READY 4", "DONE match bytes=4 ms=2100"], True)
verify("  flash differs",
       ["READY 4", "ERR MISMATCH 900 of 4 bytes differ, first at 0x0010"],
       False)

# The whole image must reach the device, or the firmware sits waiting for
# bytes that never come and the next command lands mid-transfer.
print("\nTransfer")
fake._script = ["READY 1024", "DONE bytes=1024 pages=8 ms=4820"]
p = m.Programmer("fake")
quiet(p.flash, b'\xA5' * 1024)
check("  sends every byte of the image", p.ser.written,
      1024 + len("AVRFLASH 1024\n"))


# ---- Replies ---------------------------------------------------------
print("\nReplies")
check("  ATmega32A accepted",
      quiet(m.show_signature, "SIG 0x1E9502 ATmega32/ATmega32A retries=0"), True)
check("  another AVR refused",
      quiet(m.show_signature, "SIG 0x1E9403 unknown AVR retries=0"), False)
check("  absent target refused",
      quiet(m.show_signature, "ERR NO_TARGET after 3 attempts"), False)
check("  fuses decoded",
      quiet(m.show_fuses, "FUSES lfuse=0xE1 hfuse=0x99 lock=0xFF risk=fine"),
      (0xE1, 0x99))
check("  unreadable fuses refused",
      quiet(m.show_fuses, "ERR NO_TARGET"), None)

print("\n" + ("ALL AVR PROTOCOL TESTS PASSED" if allok else "SOME FAILED"))
sys.exit(0 if allok else 1)
