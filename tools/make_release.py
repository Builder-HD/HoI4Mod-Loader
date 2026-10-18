"""Builds the release zip from the current Release build.

Usage: python -I tools/make_release.py

Writes release/hoi4_loader-v<version>.zip (one top folder of the same name) with:
  game/version.dll, game/loader_config.ini   -> the game folder
  mod/hoi4_loader_test.mod, mod/hoi4_loader_test/ -> Documents\\...\\Hearts of Iron IV\\mod\\
  stubs/                                      -> for mod authors (README "Stub pack")
  PLAYERS.md, CHANGELOG.md, SHA256SUMS.txt
and release/SHA256SUMS.txt (the zip contents) plus release/<zip>.sha256 (the zip itself).

Checks first: the DLL carries LOADER_RELEASE from src/loader.h and is newer than every source
file, the stub pack is current, and CHANGELOG.md has an entry for the release. Text files are
stored with CRLF line ends and a fixed timestamp, so the same tree gives the same zip.
"""
import hashlib
import pathlib
import re
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEXT = {'.txt', '.md', '.ini', '.yml', '.mod', '.gui', '.gfx'}


def fail(msg):
    sys.exit(f'make_release: {msg}')


def release_version():
    m = re.search(r'#define LOADER_RELEASE "([^"]+)"', (ROOT / 'src' / 'loader.h').read_text(encoding='utf-8'))
    if not m:
        fail('LOADER_RELEASE not found in src/loader.h')
    return m.group(1)


def zip_name(version):
    m = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:-[a-z0-9.]+)?', version)
    if not m:
        fail(f'unexpected release version {version!r}')
    major, minor, patch = m.groups()
    return f'hoi4_loader-v{major}.{minor}' + (f'.{patch}' if patch != '0' else '')


def release_date(version):
    m = re.search(r'^## ' + re.escape(version) + r' \((\d{4})-(\d{2})-(\d{2})\)', (ROOT / 'CHANGELOG.md').read_text(encoding='utf-8'), re.M)
    if not m:
        fail(f'CHANGELOG.md has no "## {version} (YYYY-MM-DD)" entry')
    return tuple(int(x) for x in m.groups()) + (0, 0, 0)


def check_dll(dll, version):
    if not dll.exists():
        fail(f'{dll} missing; build Release first')
    if f'{version}'.encode() not in dll.read_bytes():
        fail(f'{dll} does not contain release {version}; rebuild')
    newest = max(p.stat().st_mtime for p in (ROOT / 'src').iterdir() if p.is_file())
    if dll.stat().st_mtime < newest:
        fail(f'{dll} is older than src/; rebuild')


def files():
    """(path in the zip, source path) in a fixed order."""
    out = [('game/version.dll', ROOT / 'build' / 'Release' / 'version.dll'),
           ('game/loader_config.ini', ROOT / 'loader_config.ini'),
           ('PLAYERS.md', ROOT / 'PLAYERS.md'),
           ('CHANGELOG.md', ROOT / 'CHANGELOG.md'),
           ('mod/hoi4_loader_test.mod', ROOT / 'mod' / 'hoi4_loader_test.mod')]
    for base, prefix in ((ROOT / 'mod' / 'hoi4_loader_test', 'mod/hoi4_loader_test/'), (ROOT / 'stubs', 'stubs/')):
        for p in sorted(base.rglob('*')):
            if p.is_file():
                out.append((prefix + p.relative_to(base).as_posix(), p))
    return out


def content(src):
    data = src.read_bytes()
    if src.suffix.lower() in TEXT:
        data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
    return data


def main():
    version = release_version()
    name = zip_name(version)
    when = release_date(version)
    check_dll(ROOT / 'build' / 'Release' / 'version.dll', version)
    if subprocess.run([sys.executable, '-I', str(ROOT / 'tools' / 'make_stubs.py'), '--check'],
                      capture_output=True).returncode != 0:
        fail('stub pack out of date; run python -I tools/make_stubs.py')

    entries = [(arc, content(src)) for arc, src in files()]
    sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {arc}\n' for arc, data in entries)
    entries.append(('SHA256SUMS.txt', sums.replace('\n', '\r\n').encode()))

    out_dir = ROOT / 'release'
    out_dir.mkdir(exist_ok=True)
    zip_path = out_dir / f'{name}.zip'
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, data in entries:
            info = zipfile.ZipInfo(f'{name}/{arc}', date_time=when)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    (out_dir / 'SHA256SUMS.txt').write_bytes(sums.replace('\n', '\r\n').encode())
    zip_hash = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (out_dir / f'{name}.zip.sha256').write_text(f'{zip_hash}  {name}.zip\n', encoding='ascii')
    print(f'{zip_path.relative_to(ROOT)}: {len(entries)} files, {zip_path.stat().st_size} bytes, sha256 {zip_hash}')
    for arc, data in entries[:4]:
        print(f'  {hashlib.sha256(data).hexdigest()[:16]}  {arc}')


if __name__ == '__main__':
    main()
