#!/usr/bin/env python3
"""Write the game settings profiles built into the runtime (source/games/) to
the game fixes repository, as profiles/<id>.json.

    tools/export-game-profiles.py [--check] [--fixes PATH]

A profile in the repository is what Autorun uses for a game it has no profile
built in for (game_settings_find: a built-in profile always wins). Exporting
keeps the repository's copy of each built-in profile the same as the built-in
one, which is what lets a game's profile leave source/games/: export, publish
the repository, and then take it out of game_settings_builtin.

A profile whose content changed gets the next version, which the repository's
tools/build-fixes.py asks for. --check writes nothing and says which
profiles differ, exiting 1 if any do.

The JSON comes from tests/game_profiles.c, which includes the runtime's own
game_settings.c, so what is exported is what the runtime has; its check mode
is the proof that a profile comes back from JSON unchanged.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import tempfile

horizon_wine = Path(__file__).resolve().parents[1]
root = horizon_wine.parent


def built_in_profiles():
    with tempfile.TemporaryDirectory() as build:
        binary = Path(build) / 'game_profiles'
        subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-I', str(horizon_wine / 'source'),
                        '-I/opt/homebrew/include', str(horizon_wine / 'tests/game_profiles.c'),
                        str(horizon_wine / 'source/game_fixes.c'), str(horizon_wine / 'source/horizon_dlls.c'),
                        '-L/opt/homebrew/lib', '-lzstd', '-lz', '-lcrypto' if sys.platform != 'darwin' else '-lm',
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary), 'check'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        exported = subprocess.run([str(binary), 'export'], check=True, stdout=subprocess.PIPE, text=True).stdout
    return json.loads(exported)['profiles']


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', action='store_true', help='write nothing; say which profiles differ')
    parser.add_argument('--fixes', type=Path, default=root / 'game-fixes', help='the game fixes repository')
    args = parser.parse_args()

    folder = args.fixes / 'profiles'
    differ = []
    for profile in built_in_profiles():
        name = profile.pop('id')
        path = folder / f'{name}.json'
        current = json.loads(path.read_text()) if path.is_file() else None
        content = {k: v for k, v in profile.items() if k != 'version'}
        if current and {k: v for k, v in current.items() if k != 'version'} == content:
            continue
        differ.append(name)
        if args.check:
            continue
        profile['version'] = current['version'] + 1 if current else 1
        folder.mkdir(exist_ok=True)
        path.write_text(json.dumps(profile, indent=1, ensure_ascii=False) + '\n')
        print(f'{path.relative_to(args.fixes)}: version {profile["version"]}')
    if args.check:
        print('built-in profiles the repository has a different copy of:', ', '.join(differ) or 'none')
        return 1 if differ else 0
    if not differ:
        print('the repository has every built-in profile as it is')
    return 0


if __name__ == '__main__':
    sys.exit(main())
