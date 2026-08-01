"""Shared helpers for analyzing the dumped NieR:Automata runtime image.

The dump is in image layout: file offset == RVA. It was captured at the
executable's preferred base, so RVA + IMAGE_BASE == runtime virtual address.
"""
import re
import struct

DUMP = r"D:\Documents\NieRAutomata\analysis\nier_dump.bin"

data = open(DUMP, "rb").read()

_e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
IMAGE_BASE = struct.unpack_from("<Q", data, _e_lfanew + 24 + 24)[0]
_nsec = struct.unpack_from("<H", data, _e_lfanew + 6)[0]
_opt = struct.unpack_from("<H", data, _e_lfanew + 20)[0]
_sec_off = _e_lfanew + 24 + _opt

SECTIONS = []
for _i in range(_nsec):
    _o = _sec_off + _i * 40
    _name = data[_o:_o + 8].rstrip(b"\0").decode("latin1")
    _vsz, _va, _rsz, _ra = struct.unpack_from("<IIII", data, _o + 8)
    _ch = struct.unpack_from("<I", data, _o + 36)[0]
    SECTIONS.append({"name": _name, "va": _va, "vsize": _vsz,
                     "exec": bool(_ch & 0x20000000)})

TEXT = next(s for s in SECTIONS if s["name"] == ".text")


def exec_ranges():
    return [(s["va"], s["va"] + s["vsize"]) for s in SECTIONS if s["exec"]]


def parse_sig(text):
    """'48 8B ? ? C3' -> [0x48, 0x8B, None, None, 0xC3]"""
    return [None if t in ("?", "??") else int(t, 16) for t in text.split()]


def find_sig(text, start=0, end=None, limit=None):
    """Find a wildcard byte signature. Returns RVAs."""
    sig = parse_sig(text)
    n = len(sig)
    end = len(data) if end is None else end
    out = []
    first = sig[0]
    i = start
    while True:
        j = data.find(bytes([first]), i, end)
        if j < 0 or j + n > end:
            break
        if all(sig[k] is None or data[j + k] == sig[k] for k in range(n)):
            out.append(j)
            if limit and len(out) >= limit:
                break
        i = j + 1
    return out


def find_string(s):
    """RVAs of a NUL-terminated ASCII string."""
    needle = s.encode() + b"\0"
    out, i = [], 0
    while True:
        j = data.find(needle, i)
        if j < 0:
            break
        # require the byte before to be a terminator so we get string starts
        if j == 0 or data[j - 1] in (0, 0xFF) or not (32 <= data[j - 1] <= 126):
            out.append(j)
        i = j + 1
    return out


def find_refs(target_rva, ranges=None):
    """RVAs of 4-byte RIP-relative displacements pointing at target_rva.

    A disp32 stored at RVA p resolves to p + 4 + disp, so a reference exists
    wherever disp == target_rva - (p + 4). Returns the RVA of the displacement
    field itself; the referencing instruction starts a few bytes earlier.
    """
    import numpy as np
    ranges = ranges or exec_ranges()
    out = []
    for lo, hi in ranges:
        blob = np.frombuffer(data[lo:hi], dtype=np.uint8)
        n = len(blob) - 4
        if n <= 0:
            continue
        # disp32 at every byte offset, little-endian
        disp = (blob[0:n].astype(np.int64)
                | (blob[1:n + 1].astype(np.int64) << 8)
                | (blob[2:n + 2].astype(np.int64) << 16)
                | (blob[3:n + 3].astype(np.int64) << 24))
        disp = disp.astype(np.int32).astype(np.int64)  # sign-extend
        p = np.arange(n, dtype=np.int64)
        hits = np.nonzero(lo + p + 4 + disp == target_rva)[0]
        out.extend(int(lo + h) for h in hits)
    return out


def find_calls_to(target_rva, ranges=None):
    """RVAs of `call rel32` (E8) instructions whose target is target_rva."""
    out = []
    for ref in find_refs(target_rva, ranges):
        if ref >= 1 and data[ref - 1] == 0xE8:
            out.append(ref - 1)
    return out


def hexdump(rva, length=64):
    blob = data[rva:rva + length]
    lines = []
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        hexpart = " ".join(f"{b:02X}" for b in chunk)
        lines.append(f"{rva + i:08X}  {hexpart}")
    return "\n".join(lines)


def rva_to_va(rva):
    return IMAGE_BASE + rva


def va_to_rva(va):
    return va - IMAGE_BASE


def rel32_target(rva_of_disp):
    """Target of a rel32 at rva_of_disp (disp occupies 4 bytes)."""
    disp = struct.unpack_from("<i", data, rva_of_disp)[0]
    return rva_of_disp + 4 + disp


def load_functions():
    """Parse .pdata RUNTIME_FUNCTION entries -> sorted list of (start, end)."""
    pd = next(s for s in SECTIONS if s["name"] == ".pdata")
    out = []
    for off in range(pd["va"], pd["va"] + pd["vsize"], 12):
        start, end, unwind = struct.unpack_from("<III", data, off)
        if start == 0 and end == 0:
            continue
        out.append((start, end))
    out.sort()
    return out


_FUNCS = None


def func_of(rva):
    """(start, end) of the function containing rva, or None."""
    global _FUNCS
    if _FUNCS is None:
        _FUNCS = load_functions()
    import bisect
    i = bisect.bisect_right(_FUNCS, (rva, 1 << 62)) - 1
    if i >= 0 and _FUNCS[i][0] <= rva < _FUNCS[i][1]:
        return _FUNCS[i]
    return None


def disasm(rva, end=None, count=None):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    end = end or (rva + 400)
    lines = []
    for i, insn in enumerate(md.disasm(data[rva:end], rva)):
        lines.append((insn.address, insn.mnemonic, insn.op_str))
        if count and i + 1 >= count:
            break
    return lines


def print_func(rva, mark=None):
    fn = func_of(rva)
    if not fn:
        print(f"  (no .pdata entry for {rva:08X})")
        return
    start, end = fn
    print(f"  --- function {start:08X}..{end:08X} ---")
    for addr, mn, ops in disasm(start, end):
        m = " <==" if mark is not None and addr <= mark < addr + 1 + len(ops) and addr <= mark else ""
        m = " <==" if (mark is not None and addr == mark) else m
        print(f"  {addr:08X} {mn:10s} {ops}{m}")
