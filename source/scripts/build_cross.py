"""Build Linux x64 / Windows x64 on an Apple Silicon Mac without installing toolchains.

Downloads official, SHA-256 checked sources/tools into build-cross; all compiler
caches and OpenSSL builds live in a temporary directory. Native CI is still
required to validate the generated executables on the target operating systems.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DOWNLOADS = ROOT / "build-cross" / "downloads"
OUTPUT = ROOT.parent / "binaries"
OPENSSL_VERSION = "3.5.9"
OPENSSL_SHA256 = "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"
ZIG_VERSION = "0.15.2"
ZIG_SHA256 = "3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b"


def download(url, path, digest):
    if not path.exists():
        subprocess.run(["curl", "-fLsS", "--retry", "2", "--connect-timeout", "15",
                        "--max-time", "240", url, "-o", str(path)], check=True)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError("SHA-256 mismatch: " + path.name)


def execute(args, cwd, env=None):
    print("Running:", " ".join(map(str, args)), flush=True)
    logfile = cwd / "build.log"
    with logfile.open("ab") as log:
        result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, stdout=log, stderr=log)
    if result.returncode:
        print(logfile.read_text(errors="replace")[-10000:])
        raise RuntimeError("Build failed; log: " + str(logfile))


def main():
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        raise SystemExit("This helper targets an Apple Silicon build host; use native CMake elsewhere")
    if not shutil.which("x86_64-w64-mingw32-g++"):
        raise SystemExit("Existing mingw-w64 toolchain is required for Windows cross build")
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    source_archive = DOWNLOADS / f"openssl-{OPENSSL_VERSION}.tar.gz"
    zig_archive = DOWNLOADS / f"zig-aarch64-macos-{ZIG_VERSION}.tar.xz"
    download(f"https://github.com/openssl/openssl/releases/download/openssl-{OPENSSL_VERSION}/{source_archive.name}",
             source_archive, OPENSSL_SHA256)
    download(f"https://ziglang.org/download/{ZIG_VERSION}/{zig_archive.name}", zig_archive, ZIG_SHA256)
    work = Path(tempfile.mkdtemp(prefix="sft-cross-"))
    (ROOT / "build-cross" / "work-path.txt").write_text(str(work))
    print("Build workspace:", work, flush=True)
    for archive in [source_archive, zig_archive]:
        with tarfile.open(archive) as handle:
            handle.extractall(work, filter="data")
    source = work / f"openssl-{OPENSSL_VERSION}"
    zig = work / f"zig-aarch64-macos-{ZIG_VERSION}" / "zig"
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(work / "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(work / "zig-local-cache")
    wrappers = {}
    for name, language in [("cc", "cc"), ("cxx", "c++"), ("ar", "ar"), ("ranlib", "ranlib")]:
        wrapper = work / ("zig-" + name)
        command = [str(zig), language]
        if language in ("cc", "c++"):
            command += ["-target", "x86_64-linux-musl"]
        wrapper.write_text("#!/bin/sh\nexec " + shlex.join(command) + ' "$@"\n')
        wrapper.chmod(0o755)
        wrappers[name] = wrapper

    for target in ["windows-x86_64", "linux-x86_64"]:
        build = work / target
        prefix = build / "openssl"
        build.mkdir()
        if target.startswith("windows"):
            configuration = ["mingw64", "--cross-compile-prefix=x86_64-w64-mingw32-"]
            compiler = shutil.which("x86_64-w64-mingw32-g++")
            system = "Windows"
            target_env = env
        else:
            configuration = ["linux-x86_64"]
            compiler = str(wrappers["cxx"])
            system = "Linux"
            target_env = env.copy()
            for variable, tool in [("CC", "cc"), ("AR", "ar"), ("RANLIB", "ranlib")]:
                target_env[variable] = str(wrappers[tool])
        execute(["perl", source / "Configure", *configuration, "no-shared", "no-tests",
                 "no-module", "no-docs", f"--prefix={prefix}", "--libdir=lib"], build, target_env)
        execute(["make", "-j4", "build_libs"], build, target_env)
        execute(["make", "install_dev"], build, target_env)
        toolchain = build / "toolchain.cmake"
        toolchain.write_text(f'''set(CMAKE_SYSTEM_NAME {system})
set(CMAKE_CXX_COMPILER "{compiler}")
set(CMAKE_FIND_ROOT_PATH "{prefix}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS "-static")
''')
        app = build / "app"
        execute(["cmake", "-S", ROOT, "-B", app, "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                 f"-DOPENSSL_ROOT_DIR={prefix}", "-DSFT_STATIC_OPENSSL=ON", "-DBUILD_TESTING=OFF",
                 f"-DSFT_WINDOWS_OUTPUT_DIR={app}", "-DCMAKE_BUILD_TYPE=Release"], build, env)
        execute(["cmake", "--build", app, "--parallel", "4"], build, env)
        artifact_dir = OUTPUT / target
        artifact_dir.mkdir(parents=True, exist_ok=True)
        name = "sft.exe" if system == "Windows" else "sft"
        shutil.copy2(app / name, artifact_dir / name)
        if system == "Windows":
            shutil.copy2(app / "sft_gui.exe", artifact_dir / "sft_gui.exe")
        shutil.copy2(source / "LICENSE.txt", artifact_dir / "OpenSSL-LICENSE.txt")
        if (source / "NOTICE").exists():
            shutil.copy2(source / "NOTICE", artifact_dir / "OpenSSL-NOTICE.txt")
        print("Built:", artifact_dir / name, flush=True)
    manifest = {"openssl": OPENSSL_VERSION, "zig": ZIG_VERSION,
                "targets": ["linux-x86_64", "windows-x86_64"], "native_runtime_tested": False}
    (OUTPUT / "cross-build.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
