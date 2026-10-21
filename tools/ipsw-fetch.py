#!/usr/bin/env python3
"""List or download single members of a remote IPSW (zip) with HTTP range requests.

  ipsw-fetch.py URL                    list members (name, size)
  ipsw-fetch.py URL OUTDIR NAME...     download those members into OUTDIR

Only what is needed leaves Apple's CDN: the root filesystem and cryptex
images are a fraction of the restore file's other contents (firmware,
ramdisks). Stored and deflated members are supported; ZIP64 sizes too.
"""
import os, struct, sys, time, urllib.request, zlib

CHUNK = 8 << 20


def get(url, start, end, tries=20):
    req = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}'})
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
            if len(data) == end - start + 1:
                return data
        except OSError as e:
            print(f'\nretry {attempt + 1}/{tries}: {e}', file=sys.stderr)
        time.sleep(min(60, 2 ** attempt))
    raise SystemExit(f'range {start}-{end}: giving up')


def size_of(url):
    req = urllib.request.Request(url, method='HEAD')
    with urllib.request.urlopen(req, timeout=60) as r:
        return int(r.headers['Content-Length'])


def central_directory(url):
    total = size_of(url)
    tail = get(url, max(0, total - 65536 - 22), total - 1)
    eocd = tail.rfind(b'PK\x05\x06')
    assert eocd >= 0, 'no end of central directory'
    n, cd_size, cd_off = struct.unpack_from('<HII', tail, eocd + 10)
    if cd_off == 0xffffffff or n == 0xffff:
        loc = tail.rfind(b'PK\x06\x07')
        z64_off, = struct.unpack_from('<Q', tail, loc + 8)
        z64 = get(url, z64_off, z64_off + 55)
        n, cd_size, cd_off = struct.unpack_from('<QQQ', z64, 32)
    cd = get(url, cd_off, cd_off + cd_size - 1)
    out, p = [], 0
    for _ in range(n):
        (method, csize, usize, nlen, xlen, clen, hoff) = (
            struct.unpack_from('<H', cd, p + 10)[0], *struct.unpack_from('<II', cd, p + 20),
            *struct.unpack_from('<HHH', cd, p + 28), struct.unpack_from('<I', cd, p + 42)[0])
        name = cd[p + 46:p + 46 + nlen].decode()
        x = p + 46 + nlen
        xe = x + xlen
        while x < xe:
            tag, ln = struct.unpack_from('<HH', cd, x)
            if tag == 1:
                vals = list(struct.unpack_from(f'<{ln // 8}Q', cd, x + 4))
                if usize == 0xffffffff:
                    usize = vals.pop(0)
                if csize == 0xffffffff:
                    csize = vals.pop(0)
                if hoff == 0xffffffff:
                    hoff = vals.pop(0)
            x += 4 + ln
        out.append((name, method, csize, usize, hoff))
        p = xe + clen
    return out


def fetch(url, outdir, entry):
    name, method, csize, usize, hoff = entry
    lh = get(url, hoff, hoff + 29)
    nlen, xlen = struct.unpack_from('<HH', lh, 26)
    data = hoff + 30 + nlen + xlen
    dest = os.path.join(outdir, os.path.basename(name))
    have = os.path.getsize(dest) if method == 0 and os.path.exists(dest) else 0
    dec = zlib.decompressobj(-15) if method == 8 else None
    with open(dest, 'ab' if have else 'wb') as f:
        pos = have
        while pos < csize:
            buf = get(url, data + pos, data + min(csize, pos + CHUNK) - 1)
            f.write(dec.decompress(buf) if dec else buf)
            pos += len(buf)
            print(f'\r{name}: {pos * 100 // csize}% of {csize >> 20} MiB', end='', file=sys.stderr, flush=True)
        if dec:
            f.write(dec.flush())
    print(file=sys.stderr)
    assert os.path.getsize(dest) == usize, f'{dest}: size mismatch'
    return dest


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    url, entries = sys.argv[1], central_directory(sys.argv[1])
    if len(sys.argv) == 2:
        for name, method, csize, usize, _ in sorted(entries, key=lambda e: -e[3]):
            print(f'{usize:>12} {"stored" if method == 0 else "deflate"} {name}')
        return
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    by_name = {e[0]: e for e in entries}
    for want in sys.argv[3:]:
        print(fetch(url, outdir, by_name[want]))


if __name__ == '__main__':
    main()
