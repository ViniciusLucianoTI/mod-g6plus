#!/usr/bin/env python3
"""Keys and touches for the Moto G6 Plus running iOS, sent from the Mac over the USB cable.

The events go as UDP text lines to usbdisk_server.py --input-port (ios_usb_boot.sh starts it on 5606), which
hands them to the kernel when it polls (SWDB INPUT). Coordinates are fractions of the panel: 0 0 is the top
left corner, 1 1 the bottom right one.

  g6input.py home | power | volup | voldown     press and release a key
  g6input.py tap X Y [MS]                       touch at X Y for MS milliseconds (default 80)
  g6input.py swipe X1 Y1 X2 Y2 [MS]             drag in MS milliseconds (default 300)
  g6input.py up                                 swipe up from the bottom edge (home gesture / unlock)
  g6input.py pad [HTTP_PORT]                    serve a click pad on http://127.0.0.1:HTTP_PORT (default 5607):
                                                click and drag inside the phone outline, buttons for the keys
Option: --port N (default 5606 or $G6_INPUT_PORT).
"""
import http.server, os, socket, sys, time

PORT = int(os.environ.get('G6_INPUT_PORT', '5606'))
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def send(line):
    sock.sendto(line.encode(), ('127.0.0.1', PORT))


def key(name, ms=100):
    send(f'key {name} 1')
    time.sleep(ms / 1000)
    send(f'key {name} 0')


def swipe(x1, y1, x2, y2, ms=300):
    steps = max(2, int(ms / 16))
    for i in range(steps + 1):
        f = i / steps
        send(f'touch 0 1 {x1 + (x2 - x1) * f:.5f} {y1 + (y2 - y1) * f:.5f}')
        time.sleep(ms / 1000 / steps)
    send(f'touch 0 0 {x2:.5f} {y2:.5f}')


PAD = b"""<!doctype html><meta charset=utf-8><title>G6 input</title>
<style>body{font:14px system-ui;background:#222;color:#ddd;display:flex;gap:24px;padding:24px}
#p{width:360px;height:720px;border:2px solid #888;border-radius:18px;background:#111;touch-action:none;cursor:crosshair}
button{display:block;margin:0 0 10px;padding:10px 18px;font:inherit}</style>
<div id=p></div><div><button data-k=home>Home</button><button data-k=power>Power</button>
<button data-k=volup>Volume +</button><button data-k=voldown>Volume -</button>
<button id=up>Swipe up (home)</button><p id=s>click or drag inside the outline</p></div>
<script>
const p=document.getElementById('p'),s=document.getElementById('s');let down=false,last=0;
const post=l=>fetch('/e',{method:'POST',body:l});
const pos=e=>{const r=p.getBoundingClientRect();return [(e.clientX-r.left)/r.width,(e.clientY-r.top)/r.height]
  .map(v=>Math.min(1,Math.max(0,v)).toFixed(5)).join(' ')};
p.onpointerdown=e=>{p.setPointerCapture(e.pointerId);down=true;post('touch 0 1 '+pos(e));s.textContent=pos(e)};
p.onpointermove=e=>{if(!down)return;const t=performance.now();if(t-last<16)return;last=t;post('touch 0 1 '+pos(e))};
p.onpointerup=p.onpointercancel=e=>{if(!down)return;down=false;post('touch 0 0 '+pos(e))};
document.querySelectorAll('button[data-k]').forEach(b=>{
  b.onpointerdown=()=>post('key '+b.dataset.k+' 1');b.onpointerup=()=>post('key '+b.dataset.k+' 0')});
document.getElementById('up').onclick=()=>post('up');
</script>"""


class Pad(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(PAD)))
        self.end_headers()
        self.wfile.write(PAD)

    def do_POST(self):
        line = self.rfile.read(min(200, int(self.headers.get('Content-Length', '0')))).decode('ascii', 'replace').strip()
        w = line.split()
        if w and w[0] in ('key', 'touch'):
            send(line)
        elif line == 'up':
            swipe(0.5, 0.995, 0.5, 0.45, 250)
        self.send_response(204)
        self.end_headers()

    def log_message(self, *a):
        pass


def main():
    global PORT
    a = sys.argv[1:]
    if '--port' in a:
        i = a.index('--port')
        PORT = int(a[i + 1])
        del a[i:i + 2]
    if not a:
        sys.exit(__doc__)
    c, n = a[0], [float(v) for v in a[1:]]
    if c in ('home', 'power', 'volup', 'voldown'):
        key(c)
    elif c == 'tap':
        send(f'touch 0 1 {n[0]} {n[1]}')
        time.sleep((n[2] if len(n) > 2 else 80) / 1000)
        send(f'touch 0 0 {n[0]} {n[1]}')
    elif c == 'swipe':
        swipe(*n[:4], *(n[4:5] or [300]))
    elif c == 'up':
        swipe(0.5, 0.995, 0.5, 0.45, 250)
    elif c == 'pad':
        port = int(n[0]) if n else 5607
        print(f'click pad on http://127.0.0.1:{port} (events to UDP 127.0.0.1:{PORT}); Ctrl-C to stop')
        http.server.ThreadingHTTPServer(('127.0.0.1', port), Pad).serve_forever()
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
