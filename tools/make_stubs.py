"""Writes the stub pack in stubs/ from the feature list in src/features.cpp.

Usage: python -I tools/make_stubs.py [--check]

  (no option)  write every stub file (existing files are overwritten with the same content)
  --check      write nothing; exit 1 if a stub is missing or differs from what would be written

A mod copies the stubs it needs into its own folder (README "Stub pack"). Stubs of features
that are no longer in features.cpp are kept: guards stay usable forever (README "Versions and
compatibility").
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
STUBS = ROOT / 'stubs'


def features():
    src = (ROOT / 'src' / 'features.cpp').read_text(encoding='utf-8')
    names = re.findall(r'^    \{"([a-z_]+)", Tier::', src, re.M)
    if not names:
        sys.exit('no features found in src/features.cpp')
    return names


def stub_files(names):
    files = {
        'common/scripted_triggers/hoi4_loader_detect.txt':
            '# hoi4_loader stub. When hoi4_loader (version.dll) is active it serves\n'
            '# "hoi4_loader_present = { hidden_trigger = { always = yes } }" instead.\n'
            'hoi4_loader_present = { always = no }\n',
        'common/scripted_effects/hoi4_loader_detect.txt':
            '# hoi4_loader stub, used only when hoi4_loader (version.dll) is NOT active. The loader\n'
            '# serves its own version that sets hoi4_loader_present, global.hoi4_loader_version and\n'
            '# one hoi4_loader_plugin_<name> flag per active feature. Clearing here removes values\n'
            '# an earlier session with the loader may have saved.\n'
            'hoi4_loader_init = {\n'
            '\tclr_global_flag = hoi4_loader_present\n'
            '\tclear_variable = global.hoi4_loader_version\n'
            + ''.join(f'\tclr_global_flag = hoi4_loader_plugin_{n}\n' for n in names) +
            '}\n',
    }
    for n in names:
        files[f'common/scripted_triggers/hoi4_loader_feature_{n}.txt'] = (
            f'# hoi4_loader stub, used when hoi4_loader is absent or its {n} feature is not available.\n'
            f'# The loader serves "hidden_trigger = {{ always = yes }}" when the feature is active and all\n'
            f'# its commands registered.\n'
            f'hoi4_loader_feature_{n} = {{ always = no }}\n')
    return files


def main():
    check = '--check' in sys.argv[1:]
    bad = 0
    for rel, text in stub_files(features()).items():
        path = STUBS / rel
        data = text.encode('utf-8')
        if check:
            if not path.exists() or path.read_bytes().replace(b'\r\n', b'\n') != data:
                print(f'  FAIL stub {rel} missing or out of date (run tools/make_stubs.py)')
                bad += 1
            else:
                print(f'  ok   stub {rel}')
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f'wrote {rel}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
