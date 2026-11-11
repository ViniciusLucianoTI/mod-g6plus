#!/usr/bin/env python3
"""QEMU checks for the kernel's GIC init order (xnu patch 1030), Moto G6 Plus profile.

  qemu_gic_test.py KERNEL RAMDISK OUTDIR [--secs 90] [--keep-ram]
  qemu_gic_test.py --log QEMU_LOG          T1 only, on an existing qemu.log

KERNEL    kernel.development.sweet of an xnu.yml run (artifact xnu-kernel, macos_version 26.5)
RAMDISK   the sweet-init image booted as md0 (research/sweet-init, see tools/mkinit.py)
OUTDIR    work directory: evert-qemu.adt, payload.bin, qemu.log, ram.dump, report.txt

It builds the QEMU device tree and payload with tools/mkadt.py and tools/mkpayload.py, boots them with
tools/qemu-sweet.py (cortex-a53, evert framebuffer) while QEMU traces every GIC register write, then runs:

T1  order: GICD_CTLR is written with ARE set before the first write to the redistributor's SGI/PPI priority
    or enable registers (GICR_IPRIORITYR0-7, GICR_ISENABLER0). While GICD_CTLR.ARE_NS is 0 those registers
    are RAZ/WI. The Moto G6 Plus (SDM630) bootloader hands the GIC over with GICD_CTLR = 0, so a kernel that
    programs the redistributor first loses the virtual timer and SGI enables and hangs at its first timed
    sleep. A kernel without patch 1030 FAILS here.
T2  regression: the kernel log in guest RAM shows the boot CPU's self-IPI arriving, md0 as root and init
    being loaded, and no panic.

QEMU cannot show the G6 hang itself: its GIC has ARE set from reset (GICD_CTLR reads 0x50), so both kernels
boot there. T1 checks the property the fix establishes; the phone is still the acceptance test.

Exit 0 = both pass, 1 = a check failed, 2 = inconclusive (no trace lines, QEMU did not run).
"""
import argparse, mmap, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
CMDLINE = 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=md0 sweetearlyfb=2 vm_compression_limit=81920'

DIST = {0x0: 'GICD_CTLR'}
REDIST = {0x14: 'GICR_WAKER', 0x10080: 'GICR_IGROUPR0', 0x10100: 'GICR_ISENABLER0'}
REDIST.update({0x10400 + 4 * i: f'GICR_IPRIORITYR{i}' for i in range(8)})
GATED = {0x10100} | {0x10400 + 4 * i for i in range(8)}        # RAZ/WI while ARE_NS is 0
TRACE = re.compile(r'gicv3_(dist|redist)_write\b.*?offset (0x[0-9a-f]+) data (0x[0-9a-f]+)')

# (literal to search for, regex anchored there, wanted?, label). The regexes need the formatted digits, so the
# format strings inside the kernel image itself never match.
MILESTONES = [
    (b'SWEET GIC: GICD_CTLR 0x', rb'SWEET GIC: GICD_CTLR 0x[0-9a-f]+ -> 0x[0-9a-f]+', True,
     'SWEET GIC: GICD_CTLR 0x.. -> 0x..   (printed by patch 1030 on the boot CPU)'),
    (b'SWEET CPU: cpu=0 mpidr=0x', rb'SWEET CPU: cpu=0 mpidr=0x[0-9a-f]+ came alive; SGI1 OK', True,
     'SWEET CPU: cpu=0 mpidr=0x.. came alive; SGI1 OK'),
    (b'SWEET CPU: 1/1 CPUs', rb'SWEET CPU: 1/1 CPUs initialized, SGI1 verified', True,
     'SWEET CPU: 1/1 CPUs initialized, SGI1 verified'),
    (b'BSD root: md0', rb'BSD root: md0, major [0-9]+', True, 'BSD root: md0'),
    (b'load_init_program: attempting', rb'load_init_program: attempting to load /sbin/launchd', True,
     'load_init_program: attempting to load /sbin/launchd'),
    (b'SWEET GIC: redistributor kept', rb'SWEET GIC: redistributor kept ISENABLER0 0x[0-9a-f]+[^\n]{0,60}', False,
     'SWEET GIC: redistributor kept ISENABLER0 (enable write dropped)'),
    (b'panic(cpu ', rb'panic\(cpu [0-9]+ caller 0x[0-9a-f]+\)[^\n]{0,120}', False, 'panic'),
]


def order_check(log_path, say):
    """T1. Returns 0 pass, 1 fail, 2 inconclusive."""
    writes = []
    for line in open(log_path, errors='replace'):
        m = TRACE.search(line)
        if m:
            writes.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    if not writes:
        say(f'T1 INCONCLUSIVE: no gicv3_dist_write / gicv3_redist_write lines in {log_path}')
        say('   run qemu-sweet.py with --trace gicv3_dist_write --trace gicv3_redist_write and check that')
        say('   `qemu-system-aarch64 -trace help` lists those events')
        return 2
    are_at = gated_at = None
    for i, (kind, off, data) in enumerate(writes):
        if kind == 'dist' and off == 0 and data & 0x10 and are_at is None:
            are_at = i
        if kind == 'redist' and off in GATED and gated_at is None:
            gated_at = i
    for i, (kind, off, data) in enumerate(writes[:16]):
        name = (DIST if kind == 'dist' else REDIST).get(off, hex(off))
        mark = '  <- distributor: ARE + group enable' if i == are_at else \
               '  <- first redistributor priority/enable write' if i == gated_at else ''
        say(f'   {i + 1:2d}  {kind:6s} {name:17s} <- {data:#x}{mark}')
    if len(writes) > 16:
        say(f'   ... {len(writes) - 16} more')
    if gated_at is None:
        say('T1 INCONCLUSIVE: no redistributor priority/enable write in the trace')
        return 2
    if are_at is not None and are_at < gated_at:
        say('T1 PASS: GICD_CTLR (ARE) is written before the redistributor priority/enable registers')
        return 0
    say('T1 FAIL: the redistributor priority/enable registers are written before GICD_CTLR gets ARE')
    say('   on the Moto G6 Plus those writes are dropped (GICD_CTLR = 0 at hand-off): xnu patch 1030 is missing')
    return 1


def ram_check(dump_path, say):
    """T2. Returns 0 pass, 1 fail."""
    bad = 0
    with open(dump_path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        for literal, regex, wanted, label in MILESTONES:
            rx, hits, pos = re.compile(regex), [], 0
            while True:
                i = mm.find(literal, pos)
                if i < 0:
                    break
                m = rx.match(mm, i)
                if m:
                    hits.append(m.group(0).decode(errors='replace'))
                pos = i + 1
            if wanted:
                ok = bool(hits)
                say(f'   {"ok     " if ok else "MISSING"} {hits[0] if hits else label}')
            else:
                ok = not hits
                say(f'   {"ok     " if ok else "PRESENT"} ' + (f'none: {label}' if ok else hits[0]))
            bad += not ok
    say('T2 PASS: self-IPI delivered, md0 root, init loaded, no panic' if not bad
        else f'T2 FAIL: {bad} line(s) above are wrong')
    return 1 if bad else 0


def run(cmd, **kw):
    print('$', ' '.join(cmd), flush=True)
    return subprocess.run(cmd, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('kernel', nargs='?')
    ap.add_argument('ramdisk', nargs='?')
    ap.add_argument('outdir', nargs='?')
    ap.add_argument('--secs', type=int, default=90, help='QEMU run time (default 90; md0/init show up after ~30-60 s)')
    ap.add_argument('--keep-ram', action='store_true', help='keep OUTDIR/ram.dump (about 1.2 GB)')
    ap.add_argument('--log', help='only run T1 on this qemu.log')
    a = ap.parse_args()
    if a.log:
        return order_check(a.log, print)
    if not (a.kernel and a.ramdisk and a.outdir):
        ap.error('KERNEL RAMDISK OUTDIR are required (or --log QEMU_LOG)')
    out = os.path.abspath(a.outdir)
    os.makedirs(out, exist_ok=True)
    adt, payload, dump = (os.path.join(out, n) for n in ('evert-qemu.adt', 'payload.bin', 'ram.dump'))
    for stale in ('qemu.log', 'ram.dump'):
        if os.path.exists(os.path.join(out, stale)):
            os.unlink(os.path.join(out, stale))
    py = sys.executable
    run([py, os.path.join(TOOLS, 'mkadt.py'), adt, '--qemu', '--board', 'evert'], check=True)
    # no --compress: qemu-sweet.py reads payload version 1 only
    run([py, os.path.join(TOOLS, 'mkpayload.py'), os.path.abspath(a.kernel), adt, payload,
         '--ramdisk', os.path.abspath(a.ramdisk), '--cmdline', CMDLINE], check=True)
    phys_base, mem_size = struct.unpack_from('<2Q', open(payload, 'rb').read(0x48), 0x38)
    # pmemsave: relative file name in quotes (the monitor parses '/' as an operator), hence cwd=out
    q = run([py, os.path.join(TOOLS, 'qemu-sweet.py'), payload, out, '--cpu', 'cortex-a53',
             '--fb', '0x9d400000:1080x2160', '--secs', str(a.secs),
             '--trace', 'gicv3_dist_write', '--trace', 'gicv3_redist_write',
             '--hmp', f'pmemsave {phys_base:#x} {mem_size:#x} "ram.dump"'], cwd=out)
    lines = []

    def say(s):
        print(s, flush=True)
        lines.append(s)

    say(f'kernel: {os.path.abspath(a.kernel)}')
    if q.returncode != 0 or not os.path.exists(dump):
        say(f'INCONCLUSIVE: qemu-sweet.py returned {q.returncode}' + ('' if os.path.exists(dump) else ', no ram.dump'))
        return 2
    say('T1 order of the GIC register writes (QEMU trace)')
    t1 = order_check(os.path.join(out, 'qemu.log'), say)
    say('T2 kernel log in guest RAM')
    t2 = ram_check(dump, say)
    if not a.keep_ram:
        os.unlink(dump)
    result = 2 if t1 == 2 else 1 if (t1 or t2) else 0
    say('RESULT: ' + {0: 'PASS', 1: 'FAIL', 2: 'INCONCLUSIVE'}[result])
    open(os.path.join(out, 'report.txt'), 'w').write('\n'.join(lines) + '\n')
    return result


if __name__ == '__main__':
    sys.exit(main())
