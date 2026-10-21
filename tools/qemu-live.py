#!/usr/bin/env python3
"""Interactive live screen for iOS in QEMU: watch fb.png and touch it with the mouse.

  qemu-live.py OUTDIR PORT

Serves OUTDIR/fb.png (written by qemu-shim.py --live) and a page that turns
mouse clicks and drags into touches and has Home/Power/volume buttons, all
sent over OUTDIR/qmp.sock to the virtio tablet and keyboard
(bsd/dev/sweet_vinput.c). Keys: Esc or H = Home, P = Power, +/- = volume.
qemu-shim.py --live uses this page; run it by hand to add input to an
already running QEMU.
"""
import functools, http.server, json, os, socket, sys, threading

FB_W, FB_H = 1080, 2340
KEYS = {'home': 'home', 'power': 'power', 'volumeup': 'volumeup', 'volumedown': 'volumedown'}

PAGE = b'''<!doctype html><meta charset="utf-8"><title>ginkgo QEMU iOS</title>
<style>
body{margin:0;background:#222;color:#ddd;font:14px sans-serif;display:flex;justify-content:center;gap:12px;
  height:100vh;align-items:center;user-select:none}
#s{height:98vh;cursor:pointer;touch-action:none;-webkit-user-drag:none}
#b{display:flex;flex-direction:column;gap:8px}
button{padding:10px 14px;font-size:15px;border-radius:8px;border:0;background:#444;color:#fff;cursor:pointer}
button:active{background:#666}
small{color:#888;max-width:150px}
</style>
<img id="s" src="fb.png" draggable="false">
<div id="b">
<button data-k="home">Home</button>
<button data-k="power">Power</button>
<button data-k="volumeup">Vol +</button>
<button data-k="volumedown">Vol &minus;</button>
<small>Click = tap, drag = swipe.<br>Esc/H = Home, P = Power, +/&minus; = volume.<br>
Unlock: drag up from the bottom edge.<br>The screen updates about once a second; iOS here is slow, so wait between taps.</small>
</div>
<script>
const s = document.getElementById('s');
setInterval(() => { const n = new Image(); n.onload = () => { s.src = n.src; };
  n.src = 'fb.png?' + Date.now(); }, 700);
let q = Promise.resolve();
const send = ev => { q = q.then(() => fetch('input', {method: 'POST', body: JSON.stringify(ev)})).catch(() => {}); };
const pos = e => { const r = s.getBoundingClientRect();
  return {x: Math.round((e.clientX - r.left) / r.width * 1080), y: Math.round((e.clientY - r.top) / r.height * 2340)}; };
let down = false, last = 0;
s.addEventListener('pointerdown', e => { down = true; s.setPointerCapture(e.pointerId); send({t: 'down', ...pos(e)}); });
s.addEventListener('pointermove', e => { if (!down) return; const now = Date.now();
  if (now - last < 25) return; last = now; send({t: 'move', ...pos(e)}); });
const up = e => { if (!down) return; down = false; send({t: 'move', ...pos(e)}); send({t: 'up'}); };
s.addEventListener('pointerup', up); s.addEventListener('pointercancel', up);
document.querySelectorAll('button').forEach(b => b.onclick = () => send({t: 'key', k: b.dataset.k}));
document.addEventListener('keydown', e => { const k = {Escape: 'home', h: 'home', p: 'power', '+': 'volumeup',
  '=': 'volumeup', '-': 'volumedown'}[e.key]; if (k && !e.repeat) send({t: 'key', k}); });
</script>'''


class Qmp:
    """One QMP connection, reopened on error; input events are serialized."""

    def __init__(self, path):
        self.path, self.f, self.lock = path, None, threading.Lock()

    def _open(self):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.path)
        self.f = s.makefile('rw')
        self.f.readline()
        self._call('qmp_capabilities', {})

    def _call(self, cmd, args):
        self.f.write(json.dumps({'execute': cmd, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def events(self, events):
        with self.lock:
            for attempt in (0, 1):
                try:
                    if self.f is None:
                        self._open()
                    return self._call('input-send-event', {'events': events})
                except (OSError, ValueError):
                    self.f = None
                    if attempt:
                        raise

    def close(self):
        """Release the socket so other QMP clients (qemu-shim's screen grabs) can connect."""
        with self.lock:
            if self.f is not None:
                try:
                    self.f.close()
                except OSError:
                    pass
                self.f = None


def abs_events(x, y):
    x = min(max(float(x), 0), FB_W - 1)
    y = min(max(float(y), 0), FB_H - 1)
    return [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (FB_W - 1))}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (FB_H - 1))}}]


def btn(down):
    return {'type': 'btn', 'data': {'down': down, 'button': 'left'}}


def key(name, down):
    return {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': KEYS[name]}}}


def handle(qmp, ev):
    t = ev.get('t')
    if t == 'down':
        qmp.events(abs_events(ev['x'], ev['y']) + [btn(True)])
    elif t == 'move':
        qmp.events(abs_events(ev['x'], ev['y']))
    elif t == 'up':
        qmp.events([btn(False)])
        qmp.close()
    elif t == 'key' and ev.get('k') in KEYS:
        qmp.events([key(ev['k'], True)])
        threading.Event().wait(0.15)
        qmp.events([key(ev['k'], False)])
        qmp.close()


def serve(out, port, qmp_sock=None):
    """Serve the interactive page and OUT/fb.png on 0.0.0.0:PORT (in a background thread)."""
    qmp = Qmp(qmp_sock or f'{out}/qmp.sock')

    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def end_headers(self):
            self.send_header('Cache-Control', 'no-store')
            super().end_headers()

        def do_GET(self):
            if self.path.split('?')[0] in ('/', '/index.html'):
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(PAGE)))
                self.end_headers()
                self.wfile.write(PAGE)
            else:
                super().do_GET()

        def do_POST(self):
            if self.path != '/input':
                self.send_error(404)
                return
            try:
                handle(qmp, json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0)))))
                self.send_response(204)
            except Exception as e:
                self.send_response(500)
                print(f'input: {e}', file=sys.stderr)
            self.end_headers()

    srv = http.server.ThreadingHTTPServer(('0.0.0.0', port), functools.partial(Handler, directory=out))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(f'live screen: http://localhost:{port}/', file=sys.stderr)
    return srv


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    serve(os.path.abspath(sys.argv[1]), int(sys.argv[2]))
    threading.Event().wait()
