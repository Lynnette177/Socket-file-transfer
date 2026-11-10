"""Package only executables produced by the current GitHub Actions build."""
import argparse
import os
import platform
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    'windows-x86_64': ('Windows', {'AMD64', 'x86_64'}),
    'linux-x86_64': ('Linux', {'x86_64', 'AMD64'}),
    'macos-arm64': ('Darwin', {'arm64', 'aarch64'}),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', choices=TARGETS, required=True)
    parser.add_argument('--binary-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    expected_os, architectures = TARGETS[args.target]
    if platform.system() != expected_os or platform.machine() not in architectures:
        raise SystemExit(f'Unexpected runner for {args.target}: {platform.system()} {platform.machine()}')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sft-package-') as temp:
        stage = Path(temp)
        if args.target.startswith('windows-'):
            dest = stage/'binaries'/args.target
            dest.mkdir(parents=True)
            for name in ('sft.exe', 'sft_gui.exe'):
                shutil.copy2(args.binary_dir/name, dest/name)
            shutil.copytree(ROOT/'test', stage/'test', ignore=shutil.ignore_patterns('runtime', '__pycache__'))
            readme = 'Windows x64: run test/start.cmd for the graphical local demo, or test/server.cmd and test/client.cmd for command-line testing.\n'
        else:
            shutil.copy2(args.binary_dir/'sft', stage/'sft')
            os.chmod(stage/'sft', 0o755)
            readme = 'Run ./sft --help for usage. The CLI includes both server and client.\n'
        shutil.copy2(ROOT/'binaries'/args.target/'OpenSSL-LICENSE.txt', stage/'OpenSSL-LICENSE.txt')
        (stage/'README.txt').write_text(readme, encoding='utf-8')
        if args.target.startswith('windows-'):
            (stage/'source').mkdir()
            shutil.copy2(ROOT/'source/Windows图形客户端.md', stage/'source'/'Windows图形客户端.md')
            archive = args.output_dir/f'sft-{args.target}.zip'
            with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
                for p in sorted(stage.rglob('*')):
                    if p.is_file(): z.write(p, p.relative_to(stage))
        else:
            archive = args.output_dir/f'sft-{args.target}.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                for p in sorted(stage.iterdir()): tar.add(p, arcname=p.name)
        print(f'Created {archive}')


if __name__ == '__main__':
    main()
