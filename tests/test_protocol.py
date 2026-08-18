"""Exercise the host script's protocol handling against a fake Pico."""
import sys, types
sys.modules['serial'] = fake = types.ModuleType('serial')
sys.modules['serial.tools'] = types.ModuleType('serial.tools')
sys.modules['serial.tools.list_ports'] = lp = types.ModuleType('list_ports')
class SerialException(Exception): pass
fake.SerialException = SerialException
class FakePort:
    def __init__(self, script):
        self.script = list(script); self.buf=b''; self.timeout=1; self.written=0
    def reset_input_buffer(self): pass
    def write(self,d): self.written+=len(d); return len(d)
    def flush(self): pass
    def readline(self):
        return (self.script.pop(0)+"\n").encode() if self.script else b''
    def close(self): pass
fake.Serial = lambda *a,**k: FakePort(fake._script)
lp.comports = lambda: []
import importlib.util
spec = importlib.util.spec_from_file_location("maxv","../tools/maxv.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

def scenario(name, script, data=b"STATE RESET;\n", expect=None):
    fake._script = script
    p = m.Programmer("fake")
    ok = p.program(data, show_progress=False)
    status = "PASS" if ok == expect else "*** FAIL ***"
    print(f"{name:<34} returned {ok!s:<6} {status}")
    return ok == expect

allok = True
allok &= scenario("successful program", ["READY 14","DONE statements=1 bits=0 ms=5"], expect=True)
allok &= scenario("TDO mismatch reported", ["READY 14","ERR TDO_MISMATCH statement=3 detail=bit 7"], expect=False)
allok &= scenario("progress lines then done",
    ["READY 14","PROGRESS 500","PROGRESS 1000","DONE statements=1200 bits=99 ms=800"], expect=True)
allok &= scenario("bad READY handshake", ["ERR BAD_LENGTH"], expect=False)
allok &= scenario("silent device -> timeout", ["READY 14"], expect=False)

fake._script = ["PONG 1.0"]
p = m.Programmer("fake"); print(f"\nping: {p.ping()}")
fake._script = ["IDCODE 0x020A50DD MAX V 5M40Z/5M80Z/5M160Z/5M240Z"]
p = m.Programmer("fake"); print(f"id ok:      {m.explain_id(p.read_id())}")
fake._script = ["ERR NO_TARGET"]
p = m.Programmer("fake"); print(f"id absent:  {m.explain_id(p.read_id())}")
print("\n" + ("ALL PROTOCOL TESTS PASSED" if allok else "SOME FAILED"))
