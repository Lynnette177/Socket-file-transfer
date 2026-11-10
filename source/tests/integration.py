"""Loopback integration and hostile-peer tests. Uses only Python stdlib + OpenSSL CLI."""
import contextlib
import hashlib
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

BINARY = str(Path(sys.argv[1]).resolve())
OPENSSL = sys.argv[2] if len(sys.argv) > 2 else shutil.which("openssl")
if not OPENSSL:
    raise SystemExit("OpenSSL CLI is required for adversarial TLS tests")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(*args, expected=0):
    result = subprocess.run([BINARY, *map(str, args)], capture_output=True,
                            text=True, encoding="utf-8", errors="replace", timeout=25)
    check(result.returncode == expected,
          f"Unexpected exit {result.returncode}, expected {expected}\n{result.stdout}\n{result.stderr}")
    return result


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_until(predicate, message, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError(message)


@contextlib.contextmanager
def server(base, *extra, trust=None, root=None, port_override=None):
    port = port_override or free_port()
    root = root or base / "shared"
    log = tempfile.TemporaryFile()
    args = [BINARY, "server", "--key", str(base / "server.key"),
            "--root", str(root), "--port", str(port), "--timeout", "3"]
    args += ["--trust-dir", str(trust)] if trust else ["--peer", str(base / "client.pub")]
    process = subprocess.Popen([*args, *map(str, extra)], stdout=log, stderr=log)

    def started():
        log.seek(0)
        data = log.read()
        if process.poll() is not None:
            raise AssertionError("Server exited: " + data.decode(errors="replace"))
        return b"Server Started" in data
    try:
        wait_until(started, "Server did not start")
        yield port, process
    finally:
        if process.poll() is None:
            if os.name == "nt":
                # Windows CI cannot reliably deliver Ctrl+C to a child console.
                process.terminate()
            else:
                process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise AssertionError("Server shutdown hung")
        log.close()


def client(base, port, commands, expected=0, key="client", peer="server", local=None):
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=base,
                                     prefix="commands-", suffix=".txt", delete=False) as stream:
        stream.write(commands)
        script = Path(stream.name)
    return run("client", "--key", base / (key + ".key"),
               "--peer", base / (peer + ".pub"), "--port", port,
               "--commands", script, "--local", local or base / "local",
               "--retries", "1", "--timeout", "3", "--no-progress", expected=expected)


def frame(kind, payload):
    return struct.pack("!4sHHI", b"SFT1", 1, kind, len(payload)) + payload


def command(*fields):
    data = struct.pack("!H", len(fields))
    for field in fields:
        value = field.encode()
        data += struct.pack("!I", len(value)) + value
    return frame(1, data)


def read_all(sock, size):
    data = b""
    while len(data) < size:
        block = sock.recv(size - len(data))
        if not block:
            raise EOFError("Peer closed")
        data += block
    return data


def receive(sock):
    magic, version, kind, size = struct.unpack("!4sHHI", read_all(sock, 12))
    check(magic == b"SFT1" and version == 1 and size <= 256 * 1024, "Bad frame")
    return kind, read_all(sock, size)


def response(sock):
    kind, data = receive(sock)
    check(kind == 2, "Expected response")
    count, = struct.unpack_from("!H", data)
    offset = 2
    fields = []
    for _ in range(count):
        size, = struct.unpack_from("!I", data, offset)
        offset += 4
        fields.append(data[offset:offset + size].decode())
        offset += size
    return fields


def tls_context(base):
    # Hostile-peer harness only. Product clients always enforce pinned identities.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.set_ecdh_curve("X25519")
    ctx.set_alpn_protocols(["sft/1"])
    ctx.load_cert_chain(str(base / "client-cert.pem"), str(base / "client.key"))
    return ctx


def tls_connect(ctx, port):
    raw = socket.create_connection(("127.0.0.1", port), timeout=4)
    sock = ctx.wrap_socket(raw, server_hostname="localhost")
    sock.settimeout(4)
    check(not sock.session_reused, "Session resumption was unexpectedly enabled")
    check(sock.version() == "TLSv1.3", "Wrong TLS version")
    check(sock.cipher()[0] == "TLS_AES_256_GCM_SHA384", "Wrong cipher")
    check(response(sock) == ["OK", "/"], "Missing authenticated greeting")
    check(not sock.session.has_ticket, "Resumption ticket unexpectedly issued")
    return sock


def rejected(sock):
    try:
        receive(sock)
    except (EOFError, ssl.SSLError, ConnectionError):
        return
    raise AssertionError("Invalid protocol/TLS data was accepted")


def no_parts(root):
    return not list(root.rglob(".sft-part-*"))


class RecordProxy:
    """Forwards TLS records, optionally corrupts/captures/replays an application record."""
    def __init__(self, upstream):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(4)
        self.port = self.listener.getsockname()[1]
        self.upstream = upstream
        self.mode = None
        self.record = None
        self.error = None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.work)
        self.thread.start()

    def work(self):
        try:
            with self.listener.accept()[0] as downstream, socket.create_connection(
                    ("127.0.0.1", self.upstream), timeout=4) as upstream:
                sockets = [downstream, upstream]
                buffers = {downstream: b"", upstream: b""}
                while not self.stopping.is_set():
                    readable, _, _ = select.select(sockets, [], [], .1)
                    for source in readable:
                        data = source.recv(65536)
                        if not data:
                            return
                        buffers[source] += data
                        while len(buffers[source]) >= 5:
                            size = int.from_bytes(buffers[source][3:5], "big") + 5
                            check(size <= 18437, "Oversized TLS record")
                            if len(buffers[source]) < size:
                                break
                            record = buffers[source][:size]
                            buffers[source] = buffers[source][size:]
                            if source is downstream and self.mode:
                                mode, self.mode = self.mode, None
                                if mode == "capture":
                                    self.record = record
                                elif mode == "corrupt":
                                    record = record[:-1] + bytes([record[-1] ^ 1])
                                elif mode == "replay":
                                    record = self.record
                            (upstream if source is downstream else downstream).sendall(record)
        except (OSError, EOFError):
            pass
        except Exception as error:
            self.error = error

    def close(self):
        self.stopping.set()
        self.thread.join(timeout=5)
        self.listener.close()
        check(not self.thread.is_alive(), "Proxy thread hung")
        if self.error:
            raise self.error


def main():
    with tempfile.TemporaryDirectory(prefix="sft-integration-") as temp:
        base = Path(temp)
        local = base / "local"
        local.mkdir()
        shared = base / "shared"
        for name in ("server", "client", "stranger"):
            run("keygen", "--key", base / (name + ".key"), "--public", base / (name + ".pub"))
        original = (base / "client.key").read_bytes()
        run("keygen", "--key", base / "client.key", "--public", base / "another.pub", expected=1)
        check((base / "client.key").read_bytes() == original, "Existing identity changed")
        if os.name != "nt":
            check((base / "client.key").stat().st_mode & 0o077 == 0, "Private key permissions")
        result = subprocess.run([OPENSSL, "req", "-new", "-x509", "-key", str(base / "client.key"),
                                 "-out", str(base / "client-cert.pem"), "-days", "1", "-subj", "/CN=test-peer"],
                                capture_output=True, timeout=10)
        check(result.returncode == 0, "OpenSSL test certificate failed: " + result.stderr.decode(errors="replace"))
        sizes = [0, 1, 2, 1023, 1024, 1025, 65535, 65536, 65537, 3 * 1024 * 1024 + 3]
        names = [f"file-{size}.bin" for size in sizes]
        names += ["中文 文件名.bin", "long-" + "a" * 100 + ".bin", "marker.bin"]
        for name, size in zip(names, sizes + [777, 999, 8]):
            (local / name).write_bytes(os.urandom(size))
        (local / "marker.bin").write_bytes(b"\x01\xfe\x02\xff\x04\xfe\x05\xff")
        with server(base) as (port, process):
            commands = [f'upload "{name}"' for name in names]
            commands += ["mkdir sub", "cd sub", "pwd", "cd ..", "rekey"]
            commands += [f'get "{name}" "copy-{index}.bin"' for index, name in enumerate(names)]
            commands += ['get -o file-2.bin', "reconnect", "pwd", "ls", "rm -r sub", "exit"]
            result = client(base, port, "\n".join(commands))
            check(result.stdout.count("Fresh handshake complete") == 2, "Incorrect reconnect count")
            for index, name in enumerate(names):
                source = hashlib.sha256((local / name).read_bytes()).digest()
                check(source == hashlib.sha256((shared / name).read_bytes()).digest(), "Upload mismatch")
                check(source == hashlib.sha256((local / f"copy-{index}.bin").read_bytes()).digest(), "Download mismatch")
            check((local / "file-2.bin.L").read_bytes() == (local / "file-2.bin").read_bytes(), "Legacy .L behavior")
            result = client(base, port, "upload file-2.bin\nls\nexit", expected=1)
            check("FAIL_EXISTED_NO_OVERWRITE" in result.stdout, "Overwrite was not rejected")
            (local / "file-2.bin").write_bytes(b"new content")
            client(base, port, "upload -o file-2.bin\nexit")
            check((shared / "file-2.bin").read_bytes() == b"new content", "Overwrite failed")
            client(base, port, "ls\nexit", peer="stranger", expected=1)
            client(base, port, "mkdir unauthorized\nexit", key="stranger", expected=1)
            check(not (shared / "unauthorized").exists(), "Untrusted client performed an operation")
            ctx = tls_context(base)
            with tls_connect(ctx, port) as sock:
                # The server's 3-second I/O deadline must not expire an idle session.
                time.sleep(4)
                sock.sendall(command("pwd"))
                check(response(sock) == ["OK", "/"], "Idle session did not survive timeout")
                for byte in command("pwd"):
                    sock.sendall(bytes([byte]))
                check(response(sock) == ["OK", "/"], "Fragmented frame failed")
                sock.sendall(command("pwd") + command("ls"))
                check(response(sock)[0] == "OK" and response(sock)[0] == "OK", "Coalesced frames failed")
                for path in ("../outside", "/../outside", "C:\\outside", "CON", ".sft-part-forbidden"):
                    sock.sendall(command("mkdir", path))
                    check(response(sock)[0] == "ERROR", "Unsafe path accepted")
            outside = base / "outside"
            outside.mkdir()
            try:
                (shared / "escape").symlink_to(outside, target_is_directory=True)
            except OSError:
                print("SKIP: symlink test requires symlink permission on this host")
            else:
                with tls_connect(ctx, port) as sock:
                    sock.sendall(command("upload", "0", "escape/stolen", "1"))
                    check(response(sock)[0] == "ERROR", "Symlink escape accepted")
                check(not (outside / "stolen").exists(), "Escaped root")

            # Mid-transfer disconnect must remove temporary data and preserve old contents.
            with tls_connect(ctx, port) as sock:
                sock.sendall(command("upload", "1", "file-2.bin", "1000000"))
                check(response(sock)[0] == "OK", "Upload not accepted")
                sock.sendall(frame(4, b"partial"))
            wait_until(lambda: no_parts(shared), "Partial upload was not cleaned")
            check((shared / "file-2.bin").read_bytes() == b"new content", "Partial upload destroyed target")
            client(base, port, 'upload file-1.bin retry.bin\nexit')
            check((shared / "retry.bin").read_bytes() == (local / "file-1.bin").read_bytes(), "Retry failed")
            with tls_connect(ctx, port) as sock:
                sock.sendall(command("upload", "0", "bad-digest", "2"))
                check(response(sock)[0] == "OK", "Upload not accepted")
                sock.sendall(frame(4, b"AB") + frame(5, b"\0" * 32))
                rejected(sock)
            wait_until(lambda: no_parts(shared), "Bad-digest temporary file remains")
            check(not (shared / "bad-digest").exists(), "Corrupt upload committed")
            with tls_connect(ctx, port) as sock:
                sock.sendall(struct.pack("!4sHHI", b"SFT1", 1, 1, 256 * 1024 + 1))
                rejected(sock)
            with tls_connect(ctx, port) as sock:
                sock.sendall(command("upload", "0", "extra-data", "1"))
                response(sock)
                sock.sendall(frame(4, b"AB"))
                rejected(sock)
            check(not (shared / "extra-data").exists(), "Oversized block committed")
            # Exercise >32-bit metadata without allocating/sending a huge file.
            with tls_connect(ctx, port) as sock:
                sock.sendall(command("upload", "0", "large-metadata", "4294967296"))
                check(response(sock)[0] == "OK", "64-bit file size was truncated/rejected")
            wait_until(lambda: no_parts(shared), "Large metadata partial file remains")
            check(not (shared / "large-metadata").exists(), "Incomplete large file committed")

            legacy = tls_context(base)
            legacy.minimum_version = legacy.maximum_version = ssl.TLSVersion.TLSv1_2
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=4) as raw:
                    legacy.wrap_socket(raw, server_hostname="localhost")
            except ssl.SSLError:
                pass
            else:
                raise AssertionError("TLS downgrade was accepted")

            proxy = RecordProxy(port)
            try:
                with tls_connect(ctx, proxy.port) as sock:
                    proxy.mode = "corrupt"
                    sock.sendall(command("mkdir", "tampered"))
                    rejected(sock)
            finally:
                proxy.close()
            check(not (shared / "tampered").exists(), "Ciphertext modification accepted")
            proxy = RecordProxy(port)
            try:
                with tls_connect(ctx, proxy.port) as sock:
                    proxy.mode = "capture"
                    sock.sendall(command("pwd"))
                    check(response(sock) == ["OK", "/"], "Capture command failed")
                    proxy.mode = "replay"
                    sock.sendall(command("mkdir", "same-session-replay"))
                    rejected(sock)
                captured = proxy.record
                check(captured is not None and b"pwd" not in captured, "Command was not encrypted")
            finally:
                proxy.close()
            proxy = RecordProxy(port)
            try:
                with tls_connect(ctx, proxy.port) as sock:
                    proxy.record = captured
                    proxy.mode = "replay"
                    sock.sendall(command("mkdir", "replayed"))
                    rejected(sock)
            finally:
                proxy.close()
            check(not (shared / "replayed").exists(), "Old-session ciphertext replay accepted")
            check(not (shared / "same-session-replay").exists(), "Same-session replay accepted")

            # Multiple authenticated sessions in parallel.
            errors = []
            def concurrent(index):
                try:
                    client(base, port, f"upload file-1025.bin parallel-{index}.bin\nexit")
                except Exception as error:
                    errors.append(error)
            threads = [threading.Thread(target=concurrent, args=(i,)) for i in range(4)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            check(not errors, f"Concurrent clients failed: {errors}")
            for i in range(4):
                check((shared / f"parallel-{i}.bin").read_bytes() == (local / "file-1025.bin").read_bytes(), "Concurrent mismatch")
            # A stalled peer must be timed out without taking down the listener.
            with tls_connect(ctx, port) as sock:
                sock.sendall(b"S")
                rejected(sock)
            client(base, port, "pwd\nexit")
            if os.name != "nt":
                stalled = socket.create_connection(("127.0.0.1", port), timeout=2)
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=3)
                stalled.close()
                check(process.returncode == 0, "Unclean shutdown")

        with server(base, "--read-only") as (port, _):
            result = client(base, port, "mkdir denied\nls\nexit", expected=1)
            check("FAIL_READ_ONLY" in result.stdout, "Read-only policy failed")
        with server(base, "--max-file-size", "1") as (port, _):
            client(base, port, "upload file-1025.bin too-large.bin\nexit", expected=1)
            check(not (shared / "too-large.bin").exists(), "Size limit failed")
        trusted = base / "trusted"
        trusted.mkdir()
        shutil.copyfile(base / "client.pub", trusted / "client.pub")
        with server(base, trust=trusted) as (port, _):
            client(base, port, "pwd\nexit")

        # The existing CLI must recover from a real server restart without replaying a mutation.
        recovery_root = base / "recovery-root"
        with server(base, root=recovery_root) as (port, process):
            output = tempfile.TemporaryFile()
            recovering = subprocess.Popen([BINARY, "client", "--key", str(base / "client.key"),
                "--peer", str(base / "server.pub"), "--local", str(local), "--port", str(port),
                "--retries", "1", "--timeout", "3", "--no-progress"],
                stdin=subprocess.PIPE, stdout=output, stderr=output)
            def logs():
                output.seek(0)
                return output.read().decode("utf-8", errors="replace").replace("\r\n", "\n")
            try:
                recovering.stdin.write(b"mkdir recovery\ncd recovery\n")
                recovering.stdin.flush()
                wait_until(lambda: "/recovery>" in logs(), "Client did not enter remote directory")
                process.terminate()
                process.wait(timeout=5)
                recovering.stdin.write(b"mkdir ambiguous\n")
                recovering.stdin.flush()
                wait_until(lambda: "Operation was NOT replayed" in logs(), "Disconnect was not detected")
                with server(base, root=recovery_root, port_override=port):
                    recovering.stdin.write(b"pwd\nexit\n")
                    recovering.stdin.flush()
                    recovering.stdin.close()
                    recovering.wait(timeout=10)
                check(logs().count("Fresh handshake complete") == 2, "CLI did not perform fresh reconnect")
                check("[+]Server:: /\n" in logs(), "Working directory did not reset")
                check(not list(recovery_root.rglob("ambiguous")), "Failed mutation was replayed")
            finally:
                if recovering.poll() is None:
                    recovering.kill()
                    recovering.wait()
                output.close()
        run("server", "--key", base / "server.key", "--peer", base / "client.pub", "--root", base, expected=1)
        check(no_parts(shared), "Temporary files remain")
        print("PASS: binary/Unicode/long filenames, multi-file session, fresh reconnect, rekey, overwrite,")
        print("      mutual pins, framing, traversal/link rejection, atomic cleanup, hash/size limits,")
        print("      ciphertext tampering/replay rejection, concurrent clients, timeout and shutdown.")


if __name__ == "__main__":
    main()
