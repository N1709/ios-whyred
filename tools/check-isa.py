#!/usr/bin/env python3
"""Count instructions an ARMv8.0 core (Cortex-A53/A73, ginkgo) cannot execute.

Scans the executable parts of arm64 Mach-O files (kernel, daemons, dylibs)
and dyld shared caches (main file and every .NN subcache), linearly, word by
word. Data embedded in code can produce a few false hits; real compiler use
of a feature shows up as hundreds or thousands.

  check-isa.py FILE [FILE...] [--samples N]

Classes:
  lse     ARMv8.1 LSE atomics (CAS*, CASP*, LDADD/LDCLR/LDEOR/LDSET/LD[SU]MAX/MIN*, SWP*)
  rcpc    ARMv8.3 LDAPR* (load-acquire RCpc)
  pan     ARMv8.1 MSR PAN / MRS/MSR of the PAN register
  pauth   ARMv8.3 pointer authentication outside the HINT space (BRAA, RETAA, LDRAA, ...)
  rdm     ARMv8.1 SQRDMLAH / SQRDMLSH (scalar and vector)
  dotprod ARMv8.2 SDOT / UDOT (vector)
  fp16    ARMv8.2 half-precision data processing (FADD/FMUL/... .4h/.8h, scalar h)
"""
import struct, sys

LC_SEGMENT_64 = 0x19
VM_PROT_EXECUTE = 4
MH_MAGIC_64 = 0xfeedfacf
FAT_MAGIC = 0xcafebabe
CPU_TYPE_ARM64 = 0x0100000c


def classify(w):
    # LDAPR/LDAPRH/LDAPRB: size 111 0 00 1 0 1 11111 1 100 00 Rn Rt
    if w & 0x3ffffc00 == 0x38bfc000:
        return 'rcpc'
    # Atomic memory operations: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt
    if w & 0x3f200c00 == 0x38200000:
        return 'lse'
    # CAS{A,L,AL}{B,H}: size 001000 1 L 1 Rs o0 11111 Rn Rt
    if w & 0x3fa07c00 == 0x08a07c00:
        return 'lse'
    # CASP{A,L,AL}: 0 sz 001000 0 L 1 Rs o0 11111 Rn Rt
    if w & 0xbfa07c00 == 0x08207c00:
        return 'lse'
    # MSR PAN, #imm (op1=000 CRn=0100 op2=100)
    if w & 0xfffff0ff == 0xd500409f:
        return 'pan'
    # MRS/MSR Xt, PAN (S3_0_C4_C2_3)
    if w & 0xffdfffe0 == 0xd5184260:
        return 'pan'
    # BRAA/BRAB/BLRAA/BLRAB/BRAAZ/BLRAAZ/RETAA/RETAB/ERETAA: 1101011 x 0 x 11111 0000 1M Rn Rm
    if w & 0xfe9ff800 == 0xd61f0800:
        return 'pauth'
    # LDRAA/LDRAB: 11 111 0 00 M S 1 imm9 W 1 Rn Rt
    if w & 0xff200400 == 0xf8200400:
        return 'pauth'
    # PACIA/PACIB/AUTIA/... with register (data-processing 1 source): 1 1 0 11010110 00001 00 Z ...
    if w & 0xffffc000 == 0xdac10000:
        return 'pauth'
    # PACGA Xd, Xn, Xm|SP
    if w & 0xffe0fc00 == 0x9ac03000:
        return 'pauth'
    # SQRDMLAH/SQRDMLSH vector: 0 Q U 01110 size 0 Rm 1000 S 1 Rn Rd (U=1)
    if w & 0xbf20f400 == 0x2e008400 and (w >> 22) & 3 in (1, 2):
        return 'rdm'
    # SDOT/UDOT vector: 0 Q U 01110 10 0 Rm 1001 01 Rn Rd
    if w & 0x9fe0fc00 == 0x0e809400:
        return 'dotprod'
    # FP16 3-same vector: 0 Q U 01110 a 1 0 Rm 00 opc 1 Rn Rd
    if w & 0x9f60c400 == 0x0e400400:
        return 'fp16'
    return None


def scan(blob, out, samples, base, where):
    n = len(blob) // 4
    for i, (w,) in enumerate(struct.iter_unpack('<I', blob[:n * 4])):
        c = classify(w)
        if c:
            out[c] = out.get(c, 0) + 1
            s = samples.setdefault(c, [])
            if len(s) < SAMPLES:
                s.append((where, base + i * 4, w))
    return n


def macho_exec_ranges(data, off=0):
    """(vmaddr, bytes) of executable segments of a thin arm64 Mach-O at off."""
    magic, cputype, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', data, off)
    if magic != MH_MAGIC_64:
        raise ValueError('not a 64-bit Mach-O')
    if cputype != CPU_TYPE_ARM64:
        raise ValueError(f'cputype 0x{cputype:x} is not arm64')
    p = off + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', data, p)
        if cmd == LC_SEGMENT_64:
            name = data[p + 8:p + 24].split(b'\0')[0].decode()
            vmaddr, _, fileoff, filesize, maxprot, initprot = struct.unpack_from('<4Q2i', data, p + 24)
            if initprot & VM_PROT_EXECUTE and filesize:
                yield name, vmaddr, data[off + fileoff:off + fileoff + filesize]
        p += size


def thin_slices(data):
    if struct.unpack_from('>I', data)[0] == FAT_MAGIC:
        n = struct.unpack_from('>I', data, 4)[0]
        for k in range(n):
            cputype, _, off, size, _ = struct.unpack_from('>5I', data, 8 + 20 * k)
            if cputype == CPU_TYPE_ARM64:  # plain arm64 only; arm64e needs ARMv8.3 anyway
                yield off
    else:
        yield 0


def shared_cache_exec_ranges(data):
    moff, mcount = struct.unpack_from('<2I', data, 0x10)
    for k in range(mcount):
        addr, size, fileoff, maxprot, initprot = struct.unpack_from('<3Q2I', data, moff + 32 * k)
        if initprot & VM_PROT_EXECUTE:
            yield f'mapping{k}', addr, data[fileoff:fileoff + size]


def check(path):
    data = open(path, 'rb').read()
    counts, samples, words = {}, {}, 0
    if data[:6] == b'dyld_v':
        arch = data[:16].split(b'\0')[0].decode().split()[-1]
        print(f'{path}: dyld shared cache ({arch})')
        if arch == 'arm64e':
            print('  arm64e cache: needs pointer authentication (ARMv8.3), cannot run on ARMv8.0')
        ranges = shared_cache_exec_ranges(data)
    else:
        offs = list(thin_slices(data))
        if not offs:
            print(f'{path}: no plain arm64 slice')
            return
        print(f'{path}: Mach-O')
        ranges = (r for o in offs for r in macho_exec_ranges(data, o))
    for name, vmaddr, blob in ranges:
        words += scan(blob, counts, samples, vmaddr, name)
    print(f'  {words} instruction words scanned')
    if not counts:
        print('  OK: no ARMv8.1+ instructions found')
    for c in sorted(counts, key=lambda k: -counts[k]):
        print(f'  {c:8s} {counts[c]}')
        for where, addr, w in samples[c]:
            print(f'           {where} 0x{addr:x}: {w:08x}')


SAMPLES = 3

if __name__ == '__main__':
    args = sys.argv[1:]
    if '--samples' in args:
        i = args.index('--samples')
        SAMPLES = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    for a in args:
        check(a)
