"""Small x64 PE helper: find string xrefs and disassemble around them.

  python pex.py <dll> xref <text>          every instruction that addresses <text>
  python pex.py <dll> dis <rva> [count]    disassemble from an RVA (hex)
  python pex.py <dll> func <rva>           disassemble the function containing an RVA (via .pdata)
  python pex.py <dll> callers <rva>        direct calls/jumps to an RVA
"""
import sys, struct, pefile, capstone

pe_path = sys.argv[1]
pe = pefile.PE(pe_path, fast_load=False)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

text = [s for s in pe.sections if s.Characteristics & 0x20000000]

def funcs():
    out = []
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    for i in range(d.Size // 12):
        b, e, u = struct.unpack_from('<III', img, d.VirtualAddress + i * 12)
        out.append((b, e))
    return out

FUNCS = None
def func_of(rva):
    global FUNCS
    FUNCS = FUNCS or funcs()
    best = None
    for b, e in FUNCS:
        if b <= rva < e:
            best = (b, e)
    return best

def find_all(data):
    i = 0
    while True:
        i = img.find(data, i)
        if i < 0:
            return
        yield i
        i += 1

def scan_refs(targets):
    targets = set(targets)
    global FUNCS
    FUNCS = FUNCS or funcs()
    for b, e in FUNCS:
        for ins in md.disasm(img[b:e], base + b):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    t = ins.address + ins.size + op.mem.disp - base
                    if t in targets:
                        yield ins.address - base, ins

def dis(rva, n=60):
    for ins in md.disasm(img[rva:rva + n * 15], base + rva, count=n):
        print(f'{ins.address - base:08x}  {ins.mnemonic:8} {ins.op_str}')

cmd = sys.argv[2]
if cmd == 'xref':
    t = sys.argv[3]
    hits = list(find_all(t.encode() + b'\0')) + list(find_all(t.encode('utf-16le') + b'\0\0'))
    print('string at', [hex(h) for h in hits])
    seen = set()
    for ia, ins in scan_refs(hits):
        if ia in seen: continue
        seen.add(ia)
        f = func_of(ia)
        print(f'{ia:08x} {ins.mnemonic} {ins.op_str}   func={hex(f[0]) if f else None}')
elif cmd == 'dis':
    dis(int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 60)
elif cmd == 'func':
    f = func_of(int(sys.argv[3], 16))
    print('func', hex(f[0]), hex(f[1]))
    rva = f[0]
    for ins in md.disasm(img[f[0]:f[1]], base + f[0]):
        print(f'{ins.address - base:08x}  {ins.mnemonic:8} {ins.op_str}')
elif cmd == 'callers':
    t = int(sys.argv[3], 16)
    for s in text:
        st = s.VirtualAddress
        code = img[st:st + s.Misc_VirtualSize]
        for off in range(len(code) - 5):
            if code[off] in (0xE8, 0xE9):
                d = struct.unpack_from('<i', code, off + 1)[0]
                if st + off + 5 + d == t:
                    f = func_of(st + off)
                    print(f'{st + off:08x} {"call" if code[off] == 0xE8 else "jmp"}  func={hex(f[0]) if f else None}')
elif cmd == 'vtable':
    # RTTI: TypeDescriptor name -> COL -> vtable; print slots
    name = sys.argv[3].encode() + b'\0'
    ti = img.find(name)
    td = ti - 0x10
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 12
    for i in range(0, len(img) - 24, 4):
        sig, off, cd, tdr = struct.unpack_from('<IIII', img, i)
        if sig == 1 and tdr == td:
            col = i
            vt = img.find(struct.pack('<Q', base + col))
            while vt >= 0:
                print(f'COL {col:x} off {off:x} vtable {vt + 8:x}')
                for k in range(n):
                    p = struct.unpack_from('<Q', img, vt + 8 + k * 8)[0]
                    print(f'  [{k:2}] {p - base:08x}')
                vt = img.find(struct.pack('<Q', base + col), vt + 1)
