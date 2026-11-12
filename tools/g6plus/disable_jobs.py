#!/usr/bin/env python3
"""Disable launchd jobs that cannot work on the Moto G6 Plus and burn CPU, over SSH (docs/SSH_OVER_USB.md).

  disable_jobs.py [--host root@192.168.7.2] [--undo] [LABEL ...]

Edits /private/var/db/com.apple.xpc.launchd/disabled.plist on the running phone (effective at the next
boot); the first run keeps the original as /var/root/disabled.plist.orig. With no LABEL the list below is
used. --undo removes the labels again. The root is the image the Mac serves: nothing is written to the phone.

Default list, each observed on the G6 (2026-11-12, ps and the kernel's exec log):
  com.apple.voicemail.vmd   vmd at 94 % of a core, no modem
  com.apple.bluetoothd      exec'd 39 times in 12 minutes, no Bluetooth driver
  com.apple.fairplayd.H2    exec'd 39 times in 12 minutes
"""
import plistlib, subprocess, sys

DEFAULT = ['com.apple.voicemail.vmd', 'com.apple.bluetoothd', 'com.apple.fairplayd.H2']
PATH = '/private/var/db/com.apple.xpc.launchd/disabled.plist'


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
    undo = '--undo' in a
    labels = [x for x in a if x != '--undo'] or DEFAULT
    raw = ssh(host, f'cat {PATH}')
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    d = plistlib.loads(raw) if raw else {}
    ssh(host, f'[ -e /var/root/disabled.plist.orig ] || cp {PATH} /var/root/disabled.plist.orig')
    for l in labels:
        if undo:
            d.pop(l, None)
        else:
            d[l] = True
    ssh(host, f'cat > {PATH}.new && mv {PATH}.new {PATH} && chmod 644 {PATH} && sync', plistlib.dumps(d, fmt=fmt))
    back = plistlib.loads(ssh(host, f'cat {PATH}'))
    for k in sorted(back):
        print(f'{"disabled" if back[k] else "enabled "}  {k}')


if __name__ == '__main__':
    main()
