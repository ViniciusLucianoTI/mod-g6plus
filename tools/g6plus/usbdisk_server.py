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
   64.. inline log text.   Types: 1 HELLO, 2 READ, 3 WRITE, 4 LOG, 5 FLUSH; replies carry type | 0x80.
"""
import argparse, ctypes, ctypes.util, os, struct, sys, time

VID, PID, IFACE, EP_IN, EP_OUT = 0x1209, 0x0001, 2, 0x83, 0x03
MAGIC, HDR = 0x42445753, 512
HELLO, READ, WRITE, LOG, FLUSH = 1, 2, 3, 4, 5
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
        return True

    def close(self):
        if self.h:
            self.lib.libusb_close(self.h)
            self.h = None

    def read(self, n, timeout_ms):
        """Returns bytes, b'' on timeout; raises OSError when the device is gone."""
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

    def write(self, data, timeout_ms=5000):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        sent = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(self.h, EP_OUT, buf, len(data), ctypes.byref(sent), timeout_ms)
        if rc != 0 or sent.value != len(data):
            raise OSError(f'bulk OUT: libusb error {rc}, {sent.value}/{len(data)} bytes')


def reply(mtype, seq, status=0, nblocks=0, bsize=0, flags=0, data=b''):
    h = struct.pack('<IIIIQIIQII', MAGIC, mtype | 0x80, seq, status, 0, 0, len(data), nblocks, bsize, flags)
    return h + b'\0' * (HDR - len(h)) + data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', nargs='?', help='disk image served as usbroot0 (omit: kernel log only)')
    ap.add_argument('--rw', action='store_true', help='allow writes to IMAGE (kernel also needs usbrootrw=1)')
    ap.add_argument('--bs', type=int, default=4096, help='block size announced to the kernel (default 4096)')
    ap.add_argument('--log', help='append the kernel console text to this file as well')
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
    stats = dict(reads=0, read_bytes=0, writes=0, logs=0)
    deadline = time.time() + a.secs if a.secs else None
    had_session = False

    def note(s):
        print(f'[usbdisk {time.strftime("%H:%M:%S")}] {s}', file=sys.stderr, flush=True)

    note('waiting for the phone (1209:0001, interface 2)')
    while deadline is None or time.time() < deadline:
        if not link.open():
            if had_session and a.once:
                break
            time.sleep(0.3)
            continue
        had_session = True
        note('phone connected')
        try:
            while deadline is None or time.time() < deadline:
                h = link.read(HDR, 500)
                if not h:
                    continue
                if len(h) < 64:
                    note(f'short header ({len(h)} bytes), ignored')
                    continue
                magic, mtype, seq, _status, lba, nblk, length = struct.unpack_from('<IIIIQII', h, 0)
                if magic != MAGIC:
                    note(f'bad magic {magic:#x}, ignored')
                    continue
                if mtype == LOG:
                    text = h[64:64 + min(length, HDR - 64)]
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
                        link.write(reply(HELLO, seq, nblocks=size // a.bs, bsize=a.bs, flags=1 if a.rw else 0))
                        note(f'HELLO: {size // a.bs} blocks of {a.bs} ({size >> 20} MiB), {"rw" if a.rw else "ro"}')
                elif mtype == READ:
                    n = nblk * a.bs
                    data = os.pread(fd, n, lba * a.bs) if fd is not None else b''
                    ok = fd is not None and len(data) == n
                    link.write(reply(READ, seq, status=0 if ok else 1, data=data if ok else b'\0' * n))
                    stats['reads'] += 1
                    stats['read_bytes'] += n
                elif mtype == WRITE:
                    data = link.read_exact(length, 5000)
                    ok = fd is not None and a.rw and length == nblk * a.bs
                    if ok:
                        os.pwrite(fd, data, lba * a.bs)
                        stats['writes'] += 1
                    link.write(reply(WRITE, seq, status=0 if ok else 1))
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
