#!/usr/bin/env python3
"""Touch and buttons for iOS in QEMU (bsd/dev/sweet_vinput.c) over QMP.

  qemu-input.py QMP_SOCK tap X Y
  qemu-input.py QMP_SOCK swipe X1 Y1 X2 Y2 [MS]
  qemu-input.py QMP_SOCK key power|home|volumeup|volumedown [HOLD_MS]

X/Y are screenshot pixels (fb.png, 1080x2340). The tablet's 0..32767 range
spans the whole screen; the kernel scales it to the touch panel's pixels.
"""
import json, socket, sys, time

FB_W, FB_H = 1080, 2340


def qmp(sock, *cmds):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    f = s.makefile('rw')
    f.readline()
    for c, a in (('qmp_capabilities', {}),) + cmds:
        f.write(json.dumps({'execute': c, 'arguments': a}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'error' in r:
                raise SystemExit(f'{c}: {r["error"]}')
            if 'return' in r:
                break
    s.close()


def abs_events(x, y):
    return [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (FB_W - 1))}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (FB_H - 1))}}]


def touch(sock, events):
    qmp(sock, ('input-send-event', {'events': events}))


def button(down):
    return {'type': 'btn', 'data': {'down': down, 'button': 'left'}}


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    sock, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    if cmd == 'tap':
        x, y = float(args[0]), float(args[1])
        touch(sock, abs_events(x, y) + [button(True)])
        time.sleep(0.12)
        touch(sock, [button(False)])
    elif cmd == 'swipe':
        x1, y1, x2, y2 = map(float, args[:4])
        ms = float(args[4]) if len(args) > 4 else 400
        steps = max(4, int(ms / 20))
        touch(sock, abs_events(x1, y1) + [button(True)])
        for i in range(1, steps + 1):
            time.sleep(ms / 1000 / steps)
            touch(sock, abs_events(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps))
        time.sleep(0.05)
        touch(sock, [button(False)])
    elif cmd == 'key':
        hold = float(args[1]) if len(args) > 1 else 150
        key = {'type': 'qcode', 'data': args[0]}
        qmp(sock, ('input-send-event', {'events': [{'type': 'key', 'data': {'down': True, 'key': key}}]}))
        time.sleep(hold / 1000)
        qmp(sock, ('input-send-event', {'events': [{'type': 'key', 'data': {'down': False, 'key': key}}]}))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
