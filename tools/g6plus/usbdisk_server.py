#!/usr/bin/env python3
"""Host side of the kernel's "SWDB" USB interface (xnu patch 1037, bsd/dev/sweet_usb.c).

  usbdisk_server.py [IMAGE] [--rw] [--bs 4096] [--log FILE] [--once] [--secs N]

The phone (kernel booted with sweetusb=2) enumerates as 1209:0001 "Sweet iOS (USB Ethernet)" with a third,
vendor-specific interface (bulk IN 0x83, bulk OUT 0x03). Over it the kernel sends its console text and, with
rd=usbroot0, reads (and with usbrootrw=1 --rw, writes) the blocks of IMAGE: the root file system lives in a
file on this Mac and nothing is stored on the phone.

Without IMAGE only the kernel log is shown. Uses libusb through ctypes (brew install libusb): no pyusb, no
network configuration and no root. Runs until interrupted; it waits for the phone and reconnects.

Message = 512-byte little-endian header [+ data]:
   0 magic 'SWDB'  4 type  8 seq  12 status  16 lba(u64)  24 nblk  28 len  32 nblocks(u64)  40 bsize  44 flags
   64.. inline log text.   Types: 1 HELLO, 2 READ, 3 WRITE, 4 LOG, 5 FLUSH, 6 INPUT; replies carry type | 0x80.
   INPUT (HELLO flag bit 2, --input-port): the kernel polls, the reply has the event count at 16 and 16-byte
   events from 64 on: [0, key, down, 0] (key 0 power, 1 vol+, 2 vol-, 3 home) or [1, finger, down, x | y << 16]
   (x, y 0..65535 across the panel). Events come from tools/g6plus/g6input.py as UDP text lines on 127.0.0.1:
   "key home|power|volup|voldown 0|1" and "touch FINGER 0|1 X Y" with X, Y from 0 to 1.
"""
import argparse, collections, ctypes, ctypes.util, os, socket, struct, sys, threading, time

VID, PID, IFACE, EP_IN, EP_OUT = 0x1209, 0x0001, 2, 0x83, 0x03
MAGIC, HDR = 0x42445753, 512
HELLO, READ, WRITE, LOG, FLUSH, INPUT = 1, 2, 3, 4, 5, 6
KEYS = dict(power=0, volup=1, voldown=2, home=3)
LIBUSB_ERROR_TIMEOUT = -7


def load_libusb():
    for name in ('/opt/homebrew/lib/libusb-1.0.dylib', '/usr/local/lib/libusb-1.0.dylib',
                 ctypes.util.find_library('usb-1.0')):
        if name and (os.path.exists(name) or '/' not in name):
            try:
                return ctypes.CDLL(name)
            except OSError:
                pass
    sys.exit('libusb-1.0 not found (brew install libusb)')


class Link:
    def __init__(self, lib):
        self.lib, self.ctx, self.h = lib, ctypes.c_void_p(), None
        lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
        lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
        lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_close.argtypes = [ctypes.c_void_p]
        lib.libusb_clear_halt.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]
        self.err = ''
        lib.libusb_bulk_transfer.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_int,
                                             ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
        if lib.libusb_init(ctypes.byref(self.ctx)) != 0:
            sys.exit('libusb_init failed')

    def open(self):
        h = self.lib.libusb_open_device_with_vid_pid(self.ctx, VID, PID)
        if not h:
            return False
        rc = self.lib.libusb_claim_interface(h, IFACE)
        if rc != 0:
            self.lib.libusb_close(h)
            self.err = f'claim interface {IFACE}: libusb error {rc}'
            return False
        self.h = h
        # Start from clean pipes: CLEAR_FEATURE(ENDPOINT_HALT) resets the data toggle on both ends (the kernel ends
        # any transfer still queued there and fails its request, which it then retries from the header).
        for ep in (EP_IN, EP_OUT):
            self.lib.libusb_clear_halt(h, ep)
        return True

    def close(self):
        if self.h:
            self.lib.libusb_close(self.h)
            self.h = None

    def read(self, n, timeout_ms):
        """Returns bytes, b'' on timeout; raises OSError when the device is gone.

        timeout_ms 0 = wait forever, and that is what the message loop uses: when a bulk read times out, libusb
        (macOS backend) aborts the pipe and clears its halt, which resets the host's data toggle while the phone
        keeps its own. The next packet is then dropped as a duplicate and the 512-byte framing is lost for good
        (seen as an endless run of "bad magic" and a root device that stops answering)."""
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(self.h, EP_IN, buf, n, ctypes.byref(got), timeout_ms)
        if rc == LIBUSB_ERROR_TIMEOUT and got.value == 0:
            return b''
        if rc not in (0, LIBUSB_ERROR_TIMEOUT):
            raise OSError(f'bulk IN: libusb error {rc}')
        return buf.raw[:got.value]

    def read_exact(self, n, timeout_ms):
        out = b''
        while len(out) < n:
            part = self.read(n - len(out), timeout_ms)
            if not part:
                raise OSError(f'bulk IN: short read {len(out)}/{n}')
            out += part
        return out

    def write(self, data, timeout_ms=0):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        sent = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(self.h, EP_OUT, buf, len(data), ctypes.byref(sent), timeout_ms)
        if rc != 0 or sent.value != len(data):
            raise OSError(f'bulk OUT: libusb error {rc}, {sent.value}/{len(data)} bytes')


def make_key(nbytes=0x10000):
    """The kernel's whitening keystream (bsd/dev/sweet_usb.c swd_key_init): xorshift64*, as one big integer."""
    x, m, out = 0x9E3779B97F4A7C15, (1 << 64) - 1, bytearray()
    for _ in range(nbytes // 8):
        x ^= x >> 12
        x = (x ^ (x << 25)) & m
        x ^= x >> 27
        out += ((x * 0x2545F4914F6CDD1D) & m).to_bytes(8, 'little')
    return bytes(out)


KEY = make_key()


KEY2 = KEY + KEY


def whiten(data, seq):
    """XOR with the keystream (its own inverse), started at a position derived from the message's sequence number
    (swd_scramble in the kernel): a retried message is a different bit pattern on the wire. Payloads only."""
    n = len(data)
    r = ((seq * 2654435761) % (len(KEY) // 8)) * 8
    return (int.from_bytes(data, 'little') ^ int.from_bytes(KEY2[r:r + n], 'little')).to_bytes(n, 'little')


def reply(mtype, seq, status=0, nblocks=0, bsize=0, flags=0, data=b''):
    h = struct.pack('<IIIIQIIQII', MAGIC, mtype | 0x80, seq, status, 0, 0, len(data), nblocks, bsize, flags)
    return h + b'\0' * (HDR - len(h)) + data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', nargs='?', help='disk image served as usbroot0 (omit: kernel log only)')
    ap.add_argument('--rw', action='store_true', help='allow writes to IMAGE (kernel also needs usbrootrw=1)')
    ap.add_argument('--bs', type=int, default=4096, help='block size announced to the kernel (default 4096)')
    ap.add_argument('--log', help='append the kernel console text to this file as well')
    ap.add_argument('--input-port', type=int, default=0, help='take key/touch events for the phone as UDP text lines on '
                    '127.0.0.1:PORT (tools/g6plus/g6input.py); the kernel then polls for them')
    ap.add_argument('--once', action='store_true', help='exit when the phone disconnects after a session')
    ap.add_argument('--secs', type=int, default=0, help='give up after N seconds in total (0 = run until interrupted)')
    ap.add_argument('--sum', type=int, metavar='MIB', help='print the byte sum of the first MIB MiB of IMAGE '
                    '(what the kernel prints with usbdisktest=MIB) and exit')
    a = ap.parse_args()
    if a.sum:
        data = open(a.image, 'rb').read(a.sum << 20)
        data = data[:len(data) // a.bs * a.bs]
        print(f'{len(data)} bytes, byte sum {sum(data)}')
        return

    fd = size = None
    if a.image:
        fd = os.open(a.image, os.O_RDWR if a.rw else os.O_RDONLY)
        size = os.fstat(fd).st_size
        if size % a.bs:
            print(f'warning: {a.image} is not a multiple of {a.bs} bytes; the tail is not served', file=sys.stderr)
    logf = open(a.log, 'ab') if a.log else None
    link = Link(load_libusb())
    stats = dict(reads=0, read_bytes=0, writes=0, logs=0, bad=0, aborts=0)
    deadline = time.time() + a.secs if a.secs else None
    if a.secs:      # the reads block without a timeout: --secs is enforced from a timer instead
        def give_up():
            print(f'[usbdisk] --secs {a.secs} reached: logs {stats["logs"]}, reads {stats["reads"]} '
                  f'({stats["read_bytes"] >> 10} KiB), writes {stats["writes"]}, bad {stats["bad"]}, aborted messages {stats["aborts"]}', file=sys.stderr, flush=True)
            os._exit(0)
        t = threading.Timer(a.secs, give_up)
        t.daemon = True
        t.start()
    had_session = False
    events = collections.deque(maxlen=4096)
    if a.input_port:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', a.input_port))

        def input_loop():
            while True:
                for line in sock.recv(4096).decode('ascii', 'replace').splitlines():
                    w = line.split()
                    try:
                        if w[0] == 'key':
                            events.append(struct.pack('<IIII', 0, KEYS[w[1]], int(w[2]) & 1, 0))
                        elif w[0] == 'touch':
                            x, y = (min(65535, max(0, int(float(v) * 65535))) for v in w[3:5])
                            events.append(struct.pack('<IIII', 1, int(w[1]), int(w[2]) & 1, x | y << 16))
                    except (IndexError, KeyError, ValueError):
                        print(f'[usbdisk] bad input line: {line!r}', file=sys.stderr, flush=True)
        threading.Thread(target=input_loop, daemon=True).start()
    trace = collections.deque(maxlen=24)        # last messages, dumped when the framing breaks

    def note(s):
        print(f'[usbdisk {time.strftime("%H:%M:%S")}] {s}', file=sys.stderr, flush=True)

    note('waiting for the phone (1209:0001, interface 2)')
    while deadline is None or time.time() < deadline:
        if not link.open():
            if link.err:
                note(link.err)
                link.err = ''
            if had_session and a.once:
                break
            time.sleep(0.3)
            continue
        had_session = True
        note('phone connected')
        try:
            while deadline is None or time.time() < deadline:
                h = link.read(HDR, 0)
                if not h:
                    continue
                if len(h) < HDR:
                    # the kernel's abort marker ("SWDBABRT", a short packet) or the tail of an abandoned message
                    stats['aborts'] += 1
                    continue
                magic, mtype, seq, _status, lba, nblk, length = struct.unpack_from('<IIIIQII', h, 0)
                if magic == MAGIC and mtype not in (LOG, INPUT):
                    trace.append(f'{time.strftime("%H:%M:%S")}.{int(time.time() * 1000) % 1000:03d} type {mtype} seq {seq} '
                                 f'lba {lba} nblk {nblk} len {length}')
                if magic != MAGIC:
                    stats['bad'] += 1
                    if stats['bad'] == 1:
                        note('framing lost; last requests before it:')
                        for t in trace:
                            note('   ' + t)
                    note(f'bad magic {magic:#x} (#{stats["bad"]}): reopening the interface to reset both pipes')
                    raise OSError('framing lost')
                if mtype == LOG:
                    text = (whiten(h[64:], seq) if _status & 1 else h[64:])[:min(length, HDR - 64)]
                    sys.stdout.buffer.write(text)
                    sys.stdout.buffer.flush()
                    if logf:
                        logf.write(text)
                        logf.flush()
                    stats['logs'] += 1
                elif mtype == HELLO:
                    if fd is None:
                        link.write(reply(HELLO, seq, status=1))
                        note('HELLO refused: no image given')
                    else:
                        link.write(reply(HELLO, seq, nblocks=size // a.bs, bsize=a.bs, flags=(1 if a.rw else 0) | 2 | (4 if a.input_port else 0)))
                        note(f'HELLO: {size // a.bs} blocks of {a.bs} ({size >> 20} MiB), {"rw" if a.rw else "ro"}')
                elif mtype == READ:
                    n = nblk * a.bs
                    data = os.pread(fd, n, lba * a.bs) if fd is not None else b''
                    ok = fd is not None and len(data) == n
                    link.write(reply(READ, seq, status=0 if ok else 1, data=whiten(data if ok else b'\0' * n, seq)))
                    stats['reads'] += 1
                    stats['read_bytes'] += n
                elif mtype == WRITE:
                    data = link.read(length, 0)
                    if len(data) != length:
                        # a short packet ended the read: the kernel gave this message up (abort marker) and resends it
                        stats['aborts'] += 1
                        continue
                    data = whiten(data, seq)
                    ok = fd is not None and a.rw and length == nblk * a.bs
                    if ok:
                        os.pwrite(fd, data, lba * a.bs)
                        stats['writes'] += 1
                    link.write(reply(WRITE, seq, status=0 if ok else 1))
                elif mtype == INPUT:
                    ev = [events.popleft() for _ in range(min(len(events), 24))]
                    h = struct.pack('<IIIII', MAGIC, INPUT | 0x80, seq, 0, len(ev))
                    link.write(h + b'\0' * (64 - len(h)) + b''.join(ev) + b'\0' * (HDR - 64 - 16 * len(ev)))
                elif mtype == FLUSH:
                    if fd is not None and a.rw:
                        os.fsync(fd)
                    link.write(reply(FLUSH, seq))
                else:
                    note(f'unknown message type {mtype}')
        except OSError as e:
            note(f'phone gone ({e}); logs {stats["logs"]}, reads {stats["reads"]} ({stats["read_bytes"] >> 10} KiB), '
                 f'writes {stats["writes"]}')
        link.close()
        if a.once:
            break
    note(f'done: logs {stats["logs"]}, reads {stats["reads"]} ({stats["read_bytes"] >> 10} KiB), writes {stats["writes"]}')


if __name__ == '__main__':
    main()
