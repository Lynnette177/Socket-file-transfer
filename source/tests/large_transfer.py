"""Optional real 270 MiB roundtrip: bounded memory and automatic TLS KeyUpdate."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
try:
    import resource
except ImportError:
    resource = None

import integration as test


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            result.update(block)
    return result.digest()


def main():
    size = int(os.environ.get("SFT_LARGE_SIZE_BYTES", 270 * 1024 * 1024))
    with tempfile.TemporaryDirectory(prefix="sft-large-") as temp:
        base = Path(temp)
        local = base / "local"
        local.mkdir()
        for name in ["server", "client"]:
            test.run("keygen", "--key", base / (name + ".key"), "--public", base / (name + ".pub"))
        with (local / "large.bin").open("wb") as stream:
            # Sparse input avoids allocating the file in Python memory.
            stream.truncate(size)
        with test.server(base) as (port, _):
            result = test.client(base, port, "upload large.bin\nget large.bin copy.bin\nexit")
            test.check(result.stdout.count("Fresh handshake complete") == 1, "Unexpected reconnect")
        test.check((local / "copy.bin").stat().st_size == size, "Wrong large file size")
        test.check(digest(local / "large.bin") == digest(local / "copy.bin"), "Large file digest mismatch")
        memory = "child RSS unavailable on this platform"
        if resource:
            peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            peak_bytes = peak if sys.platform == "darwin" else peak * 1024
            test.check(peak_bytes < 128 * 1024 * 1024, "Release child memory exceeded 128 MiB")
            memory = f"peak child RSS {peak_bytes / 1024 / 1024:.1f} MiB"
        print(f"PASS: {size} byte roundtrip in one session; automatic key updates; {memory}")


if __name__ == "__main__":
    main()
