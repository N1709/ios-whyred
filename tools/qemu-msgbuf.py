#!/usr/bin/env python3
"""Dump xnu's kernel message buffer from a running QEMU (QMP socket).

  msgbuf.py QMP_SOCK OUT.txt

Scans guest RAM for struct msgbuf (MSG_MAGIC 0x063061), translates its
buffer pointer with gva2gpa, and writes the ring in order.
"""
import json, os, re, socket, struct, sys

RAM, RAM_SIZE, CHUNK = 0x40000000, 0x100000000, 0x4000000
TMP = '/tmp/msgbuf.chunk'
XNU_CPU = 16


def qmp(sock, cmd, args=None):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    f = s.makefile('rw')
    f.readline()
    if cmd == 'human-monitor-command':
        args = dict(args, **{'cpu-index': XNU_CPU})
    for c, a in (('qmp_capabilities', {}), (cmd, args or {})):
        f.write(json.dumps({'execute': c, 'arguments': a}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                break
    s.close()
    return r


def pread(sock, pa, size):
    qmp(sock, 'pmemsave', {'val': pa, 'size': size, 'filename': TMP})
    data = open(TMP, 'rb').read()
    os.unlink(TMP)
    return data


def main():
    sock, out = sys.argv[1], sys.argv[2]
    magic = struct.pack('<I', 0x063061)
    for base in range(RAM, RAM + RAM_SIZE, CHUNK):
        data = pread(sock, base, CHUNK)
        for m in re.finditer(re.escape(magic), data):
            o = m.start()
            if o % 8 or o + 24 > len(data):
                continue
            _, size, bufx, bufr, bufc = struct.unpack_from('<iiiiQ', data, o)
            if os.environ.get('DEBUG'):
                print(f'cand {base + o:#x} size {size:#x} bufx {bufx:#x} bufc {bufc:#x}')
            if not (0x1000 <= size <= 0x1000000 and 0 <= bufx < size and bufc >> 40 == 0xfffffe):
                continue
            r = qmp(sock, 'human-monitor-command', {'command-line': f'gva2gpa {bufc:#x}'})
            txt = r.get('return', '')
            if 'gpa:' not in txt:
                continue
            pa = int(txt.split('gpa:')[1].split()[0], 16)
            ring = pread(sock, pa, size)
            log = ring[bufx:] + ring[:bufx]
            open(out, 'wb').write(log.replace(b'\0', b''))
            print(f'msgbuf at {base + o:#x}: size {size:#x} bufx {bufx:#x} buf {bufc:#x} -> pa {pa:#x}')
            return
    raise SystemExit('msgbuf not found')


if __name__ == '__main__':
    main()
