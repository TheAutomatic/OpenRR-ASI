# SPDX-License-Identifier: MIT
"""Archive/prune/restore only the explicit delivery experiment-tool inventory."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT/'docs/test-evidence/mesa-aco-delivery-2026-11-10'
INVENTORY = EVIDENCE/'experiment-tool-list.json'
ARCHIVE = EVIDENCE/'experiment-tools.zip'
MANIFEST = EVIDENCE/'experiment-tools.json'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def checked_path(name):
    path = (ROOT/name).resolve()
    if not path.is_relative_to(ROOT/'asi/tests') or path.suffix not in ('.py', '.ps1'):
        raise ValueError('Outside explicit experiment-tool directory: '+name)
    return path

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=('create','verify','prune','restore'))
    args = ap.parse_args()
    names = json.loads(INVENTORY.read_text())['files']
    if len(set(names)) != len(names):
        raise ValueError('Duplicate inventory entry')
    for name in names:
        checked_path(name)
    if args.action == 'create':
        if ARCHIVE.exists() or MANIFEST.exists():
            raise FileExistsError('Refusing to replace experiment archive')
        rows = []
        with zipfile.ZipFile(ARCHIVE, 'w', zipfile.ZIP_DEFLATED) as z:
            for name in names:
                data = checked_path(name).read_bytes()
                z.writestr(name, data)
                rows.append(dict(path=name, bytes=len(data), sha256=sha(data)))
        MANIFEST.write_text(json.dumps(dict(sha256=sha(ARCHIVE.read_bytes()), members=rows), indent=2)+'\n')
    manifest = json.loads(MANIFEST.read_text())
    if sha(ARCHIVE.read_bytes()) != manifest['sha256']:
        raise ValueError('Archive checksum mismatch')
    with zipfile.ZipFile(ARCHIVE) as z:
        if set(z.namelist()) != set(names) or {r['path'] for r in manifest['members']} != set(names):
            raise ValueError('Inventory/archive mismatch')
        # Check every member before any removal or restoration.
        for row in manifest['members']:
            data = z.read(row['path'])
            if len(data) != row['bytes'] or sha(data) != row['sha256']:
                raise ValueError('Member checksum mismatch: '+row['path'])
            path = checked_path(row['path'])
            if path.exists() and sha(path.read_bytes()) != row['sha256']:
                raise ValueError('Local tool changed; preserve it: '+row['path'])
        if args.action == 'prune':
            for name in names:
                checked_path(name).unlink(missing_ok=True)
        elif args.action == 'restore':
            for name in names:
                path = checked_path(name)
                if not path.exists():
                    path.write_bytes(z.read(name))
    print(f'{args.action}: verified {len(names)} explicitly listed tools')

if __name__ == '__main__':
    main()
