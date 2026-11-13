#!/usr/bin/env python3
"""Point backboardd's launchd job at the GL launcher (gpu/eagl/launch/bgl-launch.c) on the running phone, or back.

  bglrender.py install | revert | status   [--host root@192.168.7.2]

Only ProgramArguments[0] of com.apple.backboardd in /System/Library/xpc/launchd.plist changes
(/usr/libexec/backboardd <-> /usr/local/lib/bergamota-gl/bgl-launch); the first install keeps the original file as
/var/root/launchd.plist.orig. It takes effect at the next boot (launchd reads the cache at boot; the iOS 16 boot-args
have launchd_unsecure_cache=1). The launcher execs the real backboardd unchanged unless the kernel boot-args have
bglrender=es|soft or the marker /private/var/run/bglrender names this boot session, and it has a crash-loop guard.
The payload must be on the phone first: TARGET=ios16-arm64 gpu/eagl/make-payload.sh, then copy research/bergamota-gl/
to /usr/local/lib/bergamota-gl/. The root is the image the Mac serves: nothing is written to the phone itself.
"""
import plistlib, subprocess, sys

PLIST = '/System/Library/xpc/launchd.plist'
REAL = '/usr/libexec/backboardd'
LAUNCH = '/usr/local/lib/bergamota-gl/bgl-launch'


def ssh(host, cmd, data=None):
    return subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host, cmd],
                          input=data, capture_output=True, check=True).stdout


def main():
    a = sys.argv[1:]
    host = 'root@192.168.7.2'
    if '--host' in a:
        i = a.index('--host')
        host = a[i + 1]
        del a[i:i + 2]
    if len(a) != 1 or a[0] not in ('install', 'revert', 'status'):
        sys.exit(__doc__)
    raw = ssh(host, f'cat {PLIST}')
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    d = plistlib.loads(raw)
    jobs = [j for j in d['LaunchDaemons'].values() if j.get('Label') == 'com.apple.backboardd']
    if len(jobs) != 1 or not jobs[0].get('ProgramArguments'):
        sys.exit('com.apple.backboardd not found (or it has no ProgramArguments)')
    job = jobs[0]
    print('backboardd job:', job['ProgramArguments'])
    if a[0] == 'status':
        return
    want = LAUNCH if a[0] == 'install' else REAL
    if job['ProgramArguments'][0] == want:
        print('nothing to do')
        return
    if a[0] == 'install':
        for f in ('bgl-launch', 'OpenGLES.framework/OpenGLES', 'libOSMesa.8.dylib', 'libdrm.2.dylib'):
            ssh(host, f'test -r /usr/local/lib/bergamota-gl/{f}')       # raises if the payload is not there
        ssh(host, f'[ -e /var/root/launchd.plist.orig ] || cp {PLIST} /var/root/launchd.plist.orig')
    job['ProgramArguments'][0] = want
    ssh(host, f'cat > {PLIST}.new && chmod 644 {PLIST}.new && mv {PLIST}.new {PLIST} && sync', plistlib.dumps(d, fmt=fmt))
    back = plistlib.loads(ssh(host, f'cat {PLIST}'))
    print('now:', [j['ProgramArguments'] for j in back['LaunchDaemons'].values() if j.get('Label') == 'com.apple.backboardd'][0],
          '(effective at the next boot)')


if __name__ == '__main__':
    main()
