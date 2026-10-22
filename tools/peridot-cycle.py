#!/usr/bin/env python3
"""Temporary diagnostic boot -> automatic recovery return -> console capture.

The image must contain sweetdiagreset=N or sweetpanicreset=1 (patch 0092).
Start with exactly one phone in root recovery. Sets only the 32-byte BCB
boot-recovery command; never flashes a partition or replaces a boot image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def run(args, timeout=15, check=True):
    return subprocess.run(args, timeout=timeout, check=check, capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('image', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--timeout', type=int, default=420)
    a = ap.parse_args()
    image = a.image.resolve(strict=True)
    a.out.mkdir(parents=True, exist_ok=True)
    rows = [l.split() for l in run(['adb', 'devices']).stdout.decode().splitlines()[1:] if l.strip()]
    if len(rows) != 1 or rows[0][1] != 'recovery':
        sys.exit('Need exactly one phone already in root recovery')
    serial = rows[0][0]
    adb, fastboot = ['adb', '-s', serial], ['fastboot', '-s', serial]
    if run(adb + ['shell', 'id -u']).stdout.strip() != b'0':
        sys.exit('Recovery shell must be root')
    original = run(adb + ['exec-out', 'dd if=/dev/block/by-name/misc bs=32 count=1 2>/dev/null']).stdout
    if len(original) != 32:
        sys.exit('Could not back up the BCB command')
    (a.out / 'bcb-command.before').write_bytes(original)
    command = a.out / 'bcb-boot-recovery.bin'
    command.write_bytes(b'boot-recovery'.ljust(32, b'\0'))
    run(adb + ['push', str(command), '/tmp/peridot-cycle-bcb.bin'])
    run(adb + ['shell', 'dd if=/tmp/peridot-cycle-bcb.bin of=/dev/block/by-name/misc '
               'bs=32 count=1 conv=notrunc 2>/dev/null && sync'])
    run(adb + ['reboot', 'bootloader'])
    print('Waiting for fastboot...', flush=True)
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if serial in run(['fastboot', 'devices'], check=False).stdout.decode():
            break
        time.sleep(1)
    else:
        sys.exit('Fastboot did not appear; BCB backup is in the output directory')
    boot = run(fastboot + ['boot', str(image)], timeout=45)
    (a.out / 'fastboot-boot.txt').write_bytes(boot.stdout + boot.stderr)
    metadata = {'image': str(image), 'sha256': hashlib.file_digest(image.open('rb'), 'sha256').hexdigest()}
    (a.out / 'boot.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print('Temporary boot accepted; waiting for diagnostic reset...', flush=True)
    start = time.monotonic()
    while time.monotonic() - start < a.timeout:
        state = run(adb + ['get-state'], check=False).stdout.strip()
        if state == b'recovery':
            # Recovery reconfigures its USB gadget during early init. Wait
            # until it has run for 20 s before starting a large log transfer.
            probe = run(adb + ['shell', 'cat /proc/uptime'], check=False)
            try:
                ready = probe.returncode == 0 and float(probe.stdout.split()[0]) >= 20
            except (ValueError, IndexError):
                ready = False
            if ready:
                pull = Path(__file__).resolve().with_name('sweet-log-pull.sh')
                for _ in range(3):
                    r = run([str(pull), '-s', serial, str(a.out / 'console.txt')], timeout=60, check=False)
                    (a.out / 'pull-result.txt').write_bytes(r.stdout + r.stderr)
                    if r.returncode == 0:
                        print(f'Captured console: {a.out / "console.txt"}', flush=True)
                        return 0
                    time.sleep(3)
                sys.exit('Recovery returned but log pull failed; see pull-result.txt')
        time.sleep(2)
    sys.exit('No automatic recovery return; manual fastboot/recovery reboot is needed')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        sys.exit(str(e))
