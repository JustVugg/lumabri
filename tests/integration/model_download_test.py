"""Model download against a hub imitated on loopback. Nothing here leaves this computer.

The local server answers the three requests the downloader makes to a model
hub: resolve a revision to a commit, list the files of that commit (paginated
with a Link header), and serve a file through a redirect with Range support.
The answers of the real service were NOT exercised; the fields relied upon
are listed in docs/MODEL_DOWNLOAD.md.
"""
import fcntl
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import ssl
import stat
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
FETCH = ROOT / "lumabri-fetch"
COMMIT_A, COMMIT_B = "a1" * 20, "b2" * 20
BIG = bytes((i * 7 + i // 251) % 256 for i in range(300_000))
CONFIG = b'{"model_type": "fixture"}\n'


def git_oid(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


class Repo:
    def __init__(self, files, commit=COMMIT_A, **flags):
        self.commit, self.files, self.flags = commit, dict(files), flags
        self.dropped = False

    def entries(self):
        rows = [{"type": "directory", "oid": "0" * 40, "size": 0, "path": "sub"}]
        for path, data in self.files.items():
            row = {"type": "file", "path": path, "size": len(data), "oid": git_oid(data),
                   "lastCommit": None, "securityFileStatus": {"status": "unscanned"}}
            if path.endswith(".bin"):  # large files are stored as LFS objects
                row["oid"] = "c" * 40  # the pointer's id, which is not the content's
                row["lfs"] = {"oid": hashlib.sha256(data).hexdigest(), "size": len(data), "pointerSize": 134}
            rows.append(row)
        if self.flags.get("wrong_sha"):
            rows[-1]["lfs"]["oid"] = "0" * 63 + "1"
        if self.flags.get("wrong_git"):
            next(r for r in rows if r["path"] == "config.json")["oid"] = "d" * 40
        if self.flags.get("wrong_size"):
            rows[-1]["size"] += 5
            rows[-1]["lfs"]["size"] += 5
        if self.flags.get("huge"):
            rows[-1]["size"] = 1 << 50
        if self.flags.get("evil"):
            rows.append({"type": "file", "path": "../outside.txt", "size": 4, "oid": git_oid(b"evil")})
        return rows


def standard_files():
    return {"config.json": CONFIG, "empty.txt": b"", "sub/dir/notes.txt": b"nested\n", "weights/big.bin": BIG}


class Hub(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.repos, self.heads, self.log = {}, {}, []
        self.release = threading.Event()
        self.release.set()
        self.port = self.server_address[1]
        self.base = f"http://127.0.0.1:{self.port}"

    def add(self, name, repo):
        self.repos.setdefault(name, {})[repo.commit] = repo
        self.heads[name] = repo.commit
        return repo


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", headers=()):
        self.send_response(status)
        for key, value in headers:
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        hub = self.server
        path, _, query = self.path.partition("?")
        hub.log.append((path, query, self.headers.get("Range"), self.headers.get("Authorization")))
        parts = path.strip("/").split("/")
        try:
            if parts[:2] == ["api", "models"] and parts[4] == "revision":
                name = "/".join(parts[2:4])
                return self.reply(200, json.dumps({"id": name, "sha": hub.heads[name], "private": False}).encode(),
                                  [("Content-Type", "application/json")])
            if parts[:2] == ["api", "models"] and parts[4] == "tree":
                name = "/".join(parts[2:4])
                rows = hub.repos[name][parts[5]].entries()
                assert "recursive=true" in query
                start = int(dict(p.split("=") for p in query.split("&")).get("cursor", 0))
                headers = [("Content-Type", "application/json")]
                if start + 2 < len(rows):  # two entries a page, the next one named as the real hub does
                    link = f"{hub.base}{path}?recursive=true&cursor={start + 2}"
                    headers.append(("Link", f'<{link}>; rel="next"'))
                return self.reply(200, json.dumps(rows[start:start + 2]).encode(), headers)
            if len(parts) > 4 and parts[2] == "resolve":
                name = "/".join(parts[:2])
                repo = hub.repos[name][parts[3]]
                if repo.flags.get("loop"):
                    return self.reply(302, b"redirecting", [("Location", self.path + "x")])
                if repo.flags.get("external"):
                    return self.reply(302, b"", [("Location", "http://192.0.2.1/elsewhere")])
                # Relative, as the hub's own redirect to its content servers.
                return self.reply(302, b"this body is not the file", [("Location", "/cdn/" + "/".join(parts))])
            if parts[0] == "cdn":
                name = "/".join(parts[1:3])
                repo = hub.repos[name][parts[4]]
                return self.serve(repo, repo.files["/".join(parts[5:])])
        except (KeyError, IndexError, ValueError):
            pass
        self.reply(404, b"{}")

    def serve(self, repo, data):
        offset = 0
        byte_range = self.headers.get("Range")
        if byte_range and not repo.flags.get("no_range"):
            offset = int(byte_range.removeprefix("bytes=").rstrip("-"))
            if offset >= len(data) and data:
                return self.reply(416)
        body = data[offset:]
        self.send_response(206 if byte_range and not repo.flags.get("no_range") else 200)
        if offset or (byte_range and not repo.flags.get("no_range")):
            self.send_header("Content-Range", f"bytes {offset}-{len(data) - 1}/{len(data)}")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        large = len(data) > 100_000
        if large and repo.flags.get("drop") and not repo.dropped:
            repo.dropped = True  # the connection breaks once, mid-file
            self.wfile.write(body[:120_000])
            self.wfile.flush()
            self.close_connection = True  # closed with most of the announced length missing
            return
        if large and repo.flags.get("stall") and not self.server.release.is_set():
            self.wfile.write(body[:100_000])
            self.wfile.flush()
            self.server.release.wait(60)
            body = body[100_000:]
        try:
            self.wfile.write(body)
        except OSError:
            pass


class Case:
    """One download with the helper alone: its own models and state folders."""
    count = 0

    def __init__(self, base, hub, repo_name, model="fixture"):
        Case.count += 1
        self.root = base / f"case-{Case.count}"
        self.models, self.state = self.root / "models", self.root / "state"
        self.models.mkdir(parents=True)
        self.hub, self.repo, self.model = hub, repo_name, model
        self.stage = self.models / f".lumabri-download-{model}"
        self.revision = self.models / f".lumabri-download-{model}.revision"

    def command(self, endpoint=None, revision="main", named=()):
        files = [part for name, size, digest in named for part in ("--file", f"{name}:{size}:{digest}")]
        return [str(FETCH), "get", "--models", str(self.models), "--state", str(self.state), "--id", self.model,
                "--endpoint", endpoint or self.hub.base, "--repo", self.repo, "--revision", revision,
                "--estimate", "1000", *files]

    def run(self, **options):
        return subprocess.run(self.command(**options), capture_output=True, text=True, timeout=60,
                              stdin=subprocess.DEVNULL)

    def start(self):
        return subprocess.Popen(self.command(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                stdin=subprocess.DEVNULL)

    def record(self):
        try:
            fields = (self.state / "state").read_text().split()
        except OSError:
            return {}
        return {"id": fields[1], "state": fields[2], "bytes": int(fields[3]), "total": int(fields[4]),
                "error": fields[7]} if len(fields) == 8 else {}

    def wait_bytes(self, minimum):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.record().get("state") == "downloading" and self.record().get("bytes", 0) >= minimum:
                return
            time.sleep(0.05)
        raise AssertionError(("download did not reach", minimum, self.record()))

    def failed(self, answer, reason):
        assert answer.returncode == 1, (answer.returncode, answer.stderr)
        assert self.record()["state"] == "failed" and self.record()["error"] == reason, (self.record(), answer.stderr)
        # Nothing with the model's name: the catalogue never sees a partial or wrong model.
        assert not (self.models / self.model).exists()


def tree(path):
    return {str(p.relative_to(path)): p.read_bytes() for p in sorted(path.rglob("*")) if p.is_file()}


def lock_free(state):
    with open(state / "lock", "a+") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except OSError:
            return False
        return True


def wait_exit(process, code):
    assert process.wait(timeout=20) == code, process.returncode


def helper_tests(base, hub):
    def policy(*args):
        answer = subprocess.run([str(FETCH), *args], capture_output=True, text=True, timeout=10)
        return answer.stdout.strip()

    # ---- address policy, at the unit level ----
    assert policy("check-url", "https://huggingface.co/x") == "allowed"
    assert policy("check-url", "http://127.0.0.1:8080/x") == "allowed"
    assert policy("check-url", "http://[::1]:8080/x") == "allowed"
    for refused in ("http://huggingface.co/x", "http://127.0.0.1.example.com/x", "http://localhost/x",
                    "http://127.0.0.1:80@example.com/x", "http://127.0.0.1x/x"):
        assert policy("check-url", refused) in ("plain_http_refused", "url_invalid"), refused
    assert policy("check-url", "https://user:secret@example.com/x") == "url_invalid"
    assert policy("check-url", "ftp://example.com/x") == "url_scheme_refused"
    assert policy("check-url", "file:///etc/passwd") == "url_scheme_refused"
    # A redirect may change the HTTPS host, as a hub does for its content servers, never the protection.
    assert policy("check-url", "https://huggingface.co/a", "https://cdn.example.net/b") == "allowed"
    assert policy("check-url", "https://huggingface.co/a", "http://127.0.0.1:1/b") == "redirect_to_plain_http_refused"
    assert policy("check-url", "https://huggingface.co/a", "http://example.com/b") == "plain_http_refused"
    for name in ("../x", "a/../../x", "/etc/passwd", "a//b", "a/./b", "a\\b", "c:/x", "x.lumabri-part", ".."):
        assert policy("check-path", name) == "listed_path_refused", name
    for name in ("config.json", "sub/dir/model-00001.safetensors", ".gitattributes", "a b/c..d"):
        assert policy("check-path", name) == "allowed", name

    # ---- a whole snapshot: pagination, redirect, nested folders, both hashes ----
    hub.add("owner/full", Repo(standard_files()))
    case = Case(base, hub, "owner/full")
    answer = case.run()
    assert answer.returncode == 0, answer.stderr
    assert COMMIT_A in answer.stdout
    assert tree(case.models / "fixture") == {k: v for k, v in standard_files().items()}
    assert not case.stage.exists()
    assert case.revision.read_text() == f"LMBREV1 owner/full {COMMIT_A}\n"
    assert case.record()["state"] == "done" and case.record()["bytes"] == case.record()["total"] == \
        sum(len(v) for v in standard_files().values())
    assert stat.S_IMODE((case.state).stat().st_mode) == 0o700
    paths = [entry[0] for entry in hub.log]
    assert "/api/models/owner/full/revision/main" in paths
    assert sum(1 for entry in hub.log if entry[0] == f"/api/models/owner/full/tree/{COMMIT_A}") == 3
    assert f"/owner/full/resolve/{COMMIT_A}/weights/big.bin" in paths
    assert f"/cdn/owner/full/resolve/{COMMIT_A}/weights/big.bin" in paths
    assert all(entry[3] is None for entry in hub.log), "no credentials are ever sent"
    # The same name again is refused rather than overwritten.
    again = case.run()
    assert again.returncode == 1 and case.record()["error"] == "model_already_present", again.stderr
    assert tree(case.models / "fixture") == standard_files()

    # ---- a pinned commit needs no revision lookup ----
    hub.log.clear()
    pinned = Case(base, hub, "owner/full")
    assert pinned.run(revision=COMMIT_A).returncode == 0
    assert not any("/revision/" in entry[0] for entry in hub.log)

    # ---- files named by the catalogue: only those, no listing, placed where GGUF files are found ----
    variants = {"model-q8_0.gguf": BIG, "model-q4_k_m.gguf": BIG[:50_000], "README.md": b"many variants\n"}
    hub.add("owner/gguf", Repo(variants))
    wanted = ("model-q8_0.gguf", len(BIG), hashlib.sha256(BIG).hexdigest())
    hub.log.clear()
    case = Case(base, hub, "owner/gguf")
    answer = case.run(revision=COMMIT_A, named=[wanted])
    assert answer.returncode == 0, answer.stderr
    assert tree(case.models) == {"model-q8_0.gguf": BIG, ".lumabri-download-fixture.revision":
                                 f"LMBREV1 owner/gguf {COMMIT_A}\n".encode()}
    assert not any(entry[0].startswith("/api/") or "q4_k_m" in entry[0] or "README" in entry[0] for entry in hub.log)
    again = case.run(revision=COMMIT_A, named=[wanted])
    assert again.returncode == 1 and case.record()["error"] == "model_already_present"
    case = Case(base, hub, "owner/gguf")
    case.failed(case.run(revision=COMMIT_A, named=[(wanted[0], wanted[1], "0" * 63 + "1")]), "checksum_mismatch")
    assert not (case.models / wanted[0]).exists()
    case = Case(base, hub, "owner/gguf")
    case.failed(case.run(revision=COMMIT_A, named=[(wanted[0], wanted[1] - 1, wanted[2])]), "size_mismatch")
    assert not (case.models / wanted[0]).exists()
    for name in ("../x.gguf", "sub/x.gguf", ".hidden.gguf", "x.bin"):
        refused = subprocess.run(case.command(named=[(name, 1, "0" * 64)]), capture_output=True, timeout=10)
        assert refused.returncode == 2, name

    # ---- wrong content: SHA-256 of a large file, git hash of a small one, size ----
    for flag, reason in (("wrong_sha", "checksum_mismatch"), ("wrong_git", "checksum_mismatch"),
                         ("wrong_size", "size_mismatch")):
        hub.add("owner/" + flag, Repo(standard_files(), **{flag: True}))
        case = Case(base, hub, "owner/" + flag)
        case.failed(case.run(), reason)
        # The wrong file is gone, also as an unfinished file.
        wrong = "config.json" if flag == "wrong_git" else "weights/big.bin"
        assert not (case.stage / wrong).exists() and not (case.stage / (wrong + ".lumabri-part")).exists()

    # ---- the connection breaks mid-file: resumed with Range in the same run ----
    hub.log.clear()
    hub.add("owner/drop", Repo(standard_files(), drop=True))
    case = Case(base, hub, "owner/drop")
    answer = case.run()
    assert answer.returncode == 0, answer.stderr
    assert tree(case.models / "fixture") == standard_files()
    ranges = [entry[2] for entry in hub.log if entry[0].startswith("/cdn/") and entry[2]]
    assert ranges == ["bytes=120000-"], ranges

    # ---- the helper is killed: a later run resumes the SAME commit, although the branch moved ----
    hub.log.clear()
    hub.release.clear()
    hub.add("owner/restart", Repo(standard_files(), stall=True))
    case = Case(base, hub, "owner/restart")
    process = case.start()
    try:
        case.wait_bytes(100_000)
    finally:
        process.kill()
        process.wait(timeout=10)
    hub.release.set()
    part = case.stage / "weights/big.bin.lumabri-part"
    kept = part.stat().st_size
    assert 100_000 <= kept < len(BIG) and not (case.models / "fixture").exists()
    assert lock_free(case.state), "a killed helper holds no lock"
    moved = dict(standard_files(), **{"config.json": b'{"model_type": "newer"}\n'})
    hub.add("owner/restart", Repo(moved, commit=COMMIT_B))  # the branch now points elsewhere
    hub.log.clear()
    answer = case.run()
    assert answer.returncode == 0, answer.stderr
    assert tree(case.models / "fixture") == standard_files(), "files of two commits were mixed"
    assert not any("/revision/" in entry[0] or COMMIT_B in entry[0] for entry in hub.log)
    assert [entry[2] for entry in hub.log if entry[0].startswith("/cdn/") and entry[2]] == [f"bytes={kept}-"]
    # Finished small files were not fetched again.
    assert not any(entry[0].endswith("/config.json") for entry in hub.log)

    # ---- a server without Range support: the file starts again, once ----
    hub.release.clear()
    hub.add("owner/norange", Repo(standard_files(), stall=True, no_range=True))
    case = Case(base, hub, "owner/norange")
    process = case.start()
    try:
        case.wait_bytes(100_000)
    finally:
        process.kill()
        process.wait(timeout=10)
    hub.release.set()
    assert case.run().returncode == 0 and tree(case.models / "fixture") == standard_files()

    # ---- cancel discards, pause and a signal keep; one download at a time ----
    hub.add("owner/stall", Repo(standard_files(), stall=True))
    for marker, code, keeps in (("cancel", 4, False), ("pause", 5, True), (None, 5, True)):
        hub.release.clear()
        case = Case(base, hub, "owner/stall")
        process = case.start()
        try:
            case.wait_bytes(100_000)
            second = Case(base, hub, "owner/full", model="second")
            second.state = case.state  # the same computer
            busy = second.run()
            assert busy.returncode == 3 and not second.stage.exists(), (busy.returncode, busy.stderr)
            assert case.record()["id"] == "fixture", "a refused second download must not touch the record"
            if marker:
                (case.state / marker).touch()
            else:
                process.send_signal(signal.SIGTERM)
            wait_exit(process, code)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            hub.release.set()
        assert not (case.models / "fixture").exists() and lock_free(case.state)
        assert case.stage.exists() == keeps and case.revision.exists() == keeps, marker
        assert case.record()["state"] == ("interrupted" if keeps else "cancelled")
        assert not (case.state / "cancel").exists() and not (case.state / "pause").exists()
        if keeps:
            assert case.run().returncode == 0 and tree(case.models / "fixture") == standard_files()

    # ---- redirects: bounded, and never to plain HTTP elsewhere ----
    hub.add("owner/loop", Repo(standard_files(), loop=True))
    case = Case(base, hub, "owner/loop")
    hub.log.clear()
    case.failed(case.run(), "too_many_redirects")
    assert sum(1 for entry in hub.log if "/resolve/" in entry[0] and "config.json" in entry[0]) == 6
    hub.add("owner/external", Repo(standard_files(), external=True))
    case = Case(base, hub, "owner/external")
    case.failed(case.run(), "plain_http_refused")
    # Plain HTTP to another host is refused before any connection is made.
    case = Case(base, hub, "owner/full")
    case.failed(case.run(endpoint="http://example.com"), "plain_http_refused")
    case.failed(case.run(endpoint="http://127.0.0.1.example.com"), "plain_http_refused")

    # ---- a listing that names a file outside the model folder ----
    hub.add("owner/evil", Repo(standard_files(), evil=True))
    case = Case(base, hub, "owner/evil")
    hub.log.clear()
    case.failed(case.run(), "listed_path_refused")
    assert not (case.models / "outside.txt").exists() and not (case.root / "outside.txt").exists()
    assert not any("/resolve/" in entry[0] for entry in hub.log), "refused before any file is fetched"
    # A link planted where the download folder goes is removed, not followed.
    case = Case(base, hub, "owner/full")
    outside = case.root / "outside"
    outside.mkdir()
    (outside / "keep.txt").write_text("untouched")
    case.stage.symlink_to(outside)
    assert case.run().returncode == 0
    assert tree(outside) == {"keep.txt": b"untouched"} and tree(case.models / "fixture") == standard_files()
    case = Case(base, hub, "owner/full")
    case.stage.mkdir()
    (case.stage / "sub").symlink_to(outside)
    case.revision.write_text(f"LMBREV1 owner/full {COMMIT_A}\n")  # so that the folder is resumed, not replaced
    answer = case.run()
    assert answer.returncode == 1 and tree(outside) == {"keep.txt": b"untouched"}, answer.stderr
    assert not (case.models / "fixture").exists()

    # ---- not enough disk space: decided from the listing, before any file ----
    hub.add("owner/huge", Repo(standard_files(), huge=True))
    case = Case(base, hub, "owner/huge")
    hub.log.clear()
    case.failed(case.run(), "insufficient_disk_space")
    assert not any("/resolve/" in entry[0] for entry in hub.log)

    # ---- a repository that does not exist ----
    case = Case(base, hub, "owner/missing")
    case.failed(case.run(), "repository_or_file_not_found")

    # ---- HTTPS: a certificate that cannot be verified is refused; there is no option to accept it ----
    usage = subprocess.run([str(FETCH)], capture_output=True, text=True).stderr
    assert "insecure" not in usage.lower() and "--no-verify" not in usage
    openssl = shutil.which("openssl")
    if not openssl:
        print("model download: openssl not found, self-signed certificate refusal NOT tested")
        return
    key, certificate = base / "tls.key", base / "tls.crt"
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out",
                    str(certificate), "-days", "1", "-subj", "/CN=127.0.0.1",
                    "-addext", "subjectAltName=IP:127.0.0.1"], check=True, capture_output=True, timeout=60)
    secure = Hub()
    secure.add("owner/full", Repo(standard_files()))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(certificate), str(key))
    secure.socket = context.wrap_socket(secure.socket, server_side=True)
    threading.Thread(target=secure.serve_forever, daemon=True).start()
    try:
        case = Case(base, secure, "owner/full")
        environment = {**os.environ}
        for name in ("CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR"):
            environment.pop(name, None)
        answer = subprocess.run(case.command(endpoint=f"https://127.0.0.1:{secure.port}"), capture_output=True,
                                text=True, timeout=60, env=environment)
        case.failed(answer, "tls_verification_failed")
        assert secure.log == [], "nothing is requested over an unverified connection"
    finally:
        secure.shutdown()
        secure.server_close()


def gateway_tests(base, hub):
    home = base / "home"
    home.mkdir()
    temporary = str(home)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
    env = {**os.environ, "HOME": temporary, "LUMABRI_PEER_KEY": temporary + "/peer.key",
           "LUMABRI_KNOWN_HOSTS": temporary + "/known", "LUMABRI_ADVERTISE": "127.0.0.1",
           "LUMABRI_HOME_PORT_BASE": str(12000 + (port % 200) * 160),
           "LUMABRI_NO_DISK_PROBE": "1", "LUMABRI_RAM_RESERVE_MB": "256"}
    for name in ("LUMABRI_ENCRYPT", "LUMABRI_TOKEN", "LUMABRI_HOME_FOREGROUND"):
        env.pop(name, None)
    state, models = home / ".lumabri/downloads", home / ".lumabri/models"

    def run(*args):
        return subprocess.run([str(ROOT / "lumabri"), *args], env=env, text=True, stdin=subprocess.DEVNULL,
                              capture_output=True, timeout=90)

    def call(method, token=None, body=None, path="/api/v1/downloads"):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
        headers = {"Authorization": "Bearer " + token} if token else {}
        if body is not None:
            headers["Content-Type"] = "application/json"
        conn.request(method, path, body=json.dumps(body) if body is not None else None, headers=headers)
        response = conn.getresponse(); data = response.read(); conn.close()
        return response.status, json.loads(data) if data.startswith(b"{") else data

    def model(listing, name):
        return next(m for m in listing["models"] if m["id"] == name)

    def wait_state(token, name, wanted):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status, listing = call("GET", token)
            assert status == 200, listing
            if model(listing, name)["state"] == wanted:
                return listing
            time.sleep(0.1)
        raise AssertionError((name, wanted, listing))

    hub.add("owner/tiny", Repo(standard_files(), stall=True))
    catalogue = {"schema": 1, "endpoint": hub.base, "models": [
        {"id": "starter-soon", "family": "olmoe", "name": "Not published", "resident_gb": 7,
         "unavailable_reason": "needs a published converted checkpoint"},
        {"id": "tiny", "family": "fixture", "name": "Tiny fixture", "repo": "owner/tiny", "disk_gb": 0.001,
         "ram_min_gb": 0.001, "ram_good_gb": 0.002, "summary": "test files", "licence": "Test licence"},
        {"id": "giant", "family": "fixture", "name": "Giant fixture", "repo": "owner/full", "disk_gb": 0.001,
         "resident_gb": 900000, "ram_min_gb": 1, "ram_good_gb": 2},
        {"id": "named", "family": "fixture", "name": "Named file", "repo": "owner/gguf", "revision": COMMIT_A,
         "files": [{"path": "model-q8_0.gguf", "size": len(BIG), "sha256": hashlib.sha256(BIG).hexdigest()}]}]}
    try:
        # A fixed small share: the built-in entries must not fit on any runner.
        first = run("start", "--no-browser", "--port", str(port), "--ram-gb", "4")
        assert first.returncode == 0, (first.stdout, first.stderr)
        token = next(line for line in first.stdout.splitlines() if "#token=" in line).split("#token=")[1].strip()
        owner_file = home / ".lumabri/download-catalogue.json"
        owner_file.write_text(json.dumps(catalogue))

        # ---- permissions: 401 without a credential, 403 without management ----
        assert call("GET")[0] == 401 and call("POST", body={"action": "start", "id": "tiny"})[0] == 401
        assert call("GET", "local." + "0" * 64)[0] == 401
        plain = run("api", "user-add", "reader")
        assert plain.returncode == 0, plain.stderr
        reader = plain.stdout.strip()
        assert call("GET", reader) == (403, {"error": "management_permission_required"})
        assert call("POST", reader, {"action": "start", "id": "tiny"}) == (403, {"error": "management_permission_required"})
        assert run("api", "operator-grant", "reader").returncode == 0  # visibility is not management
        assert call("GET", reader)[0] == 403
        assert not state.exists() or not (state / "state").exists()

        # ---- the list: what fits, what is too large, what is not available ----
        status, listing = call("GET", token)
        assert status == 200 and listing["catalogue"] == "owner_file" and listing["downloader_available"] is True
        assert [m["id"] for m in listing["models"]] == ["tiny", "giant", "named"] and listing["unavailable_hidden"] == 1
        assert model(listing, "named")["named_files"] == 1 and model(listing, "named")["size_bytes"] == len(BIG)
        assert listing["folder"] == str(models) and listing["local_entries"] == 0 and listing["active"] is None
        assert listing["offered_memory_bytes"] > 0 and listing["free_bytes"] > 0
        tiny, giant = model(listing, "tiny"), model(listing, "giant")
        assert tiny["state"] == "absent" and tiny["fits"] is True and tiny["licence"] == "Test licence"
        assert tiny["memory_needed_bytes"] == 1_150_000 and tiny["page_url"] is None
        assert giant["fits"] is False and giant["memory_needed_bytes"] > listing["offered_memory_bytes"]
        assert "starter-soon" not in json.dumps(listing)

        # ---- starting is explicit, and refused for what cannot be held or does not exist ----
        assert call("POST", token, {"action": "start", "id": "giant"}) == (409, {"error": "model_too_large_for_household"})
        assert call("POST", token, {"action": "start", "id": "starter-soon"}) == (404, {"error": "download_not_found"})
        assert call("POST", token, {"action": "start", "id": "nothing"})[0] == 404
        assert call("POST", token, {"action": "start", "id": "../x"})[0] == 400
        assert call("POST", token, {"action": "start", "id": "tiny", "url": "http://x"})[0] == 400
        assert call("POST", token, {"action": "erase", "id": "tiny"})[0] == 400
        assert call("PUT", token, {"action": "start", "id": "tiny"})[0] == 400
        assert not any(models.iterdir()), "a refused request starts nothing"
        refused = run("fetch", "get", "giant")
        assert refused.returncode == 1 and "enough memory" in refused.stderr, refused.stderr
        listed = run("fetch", "list")
        assert listed.returncode == 0 and "tiny" in listed.stdout and "too large for this household" in listed.stdout
        assert "starter-soon" not in listed.stdout and "1 more model is listed" in listed.stdout

        # ---- start, progress, one at a time, cancel ----
        hub.release.clear()
        status, listing = call("POST", token, {"action": "start", "id": "tiny"})
        assert status == 202, listing
        listing = wait_state(token, "tiny", "downloading")
        assert listing["active"] == "tiny"
        deadline = time.monotonic() + 20
        while model(call("GET", token)[1], "tiny")["bytes"] < 100_000 and time.monotonic() < deadline:
            time.sleep(0.1)
        progress = model(call("GET", token)[1], "tiny")
        assert 100_000 <= progress["bytes"] < progress["size_bytes"] == sum(len(v) for v in standard_files().values())
        assert len(progress["commit"]) == 40
        assert call("POST", token, {"action": "start", "id": "tiny"}) == (409, {"error": "download_already_running"})
        assert "fixture" not in str(call("GET", token, path="/api/v1/catalogue")[1])
        status, listing = call("POST", token, {"action": "cancel", "id": "tiny"})
        assert status == 202 and model(listing, "tiny")["state"] == "absent", listing
        assert not any(models.iterdir()) and lock_free(state)
        hub.release.set()

        # ---- stopping the services pauses a download; the next start resumes it ----
        hub.release.clear()
        assert call("POST", token, {"action": "start", "id": "tiny"})[0] == 202
        wait_state(token, "tiny", "downloading")
        deadline = time.monotonic() + 20
        while model(call("GET", token)[1], "tiny")["bytes"] < 100_000 and time.monotonic() < deadline:
            time.sleep(0.1)
        assert run("service", "stop").returncode == 0
        assert lock_free(state), "no transfer keeps running behind a stopped Lumabri"
        assert (models / ".lumabri-download-tiny/weights/big.bin.lumabri-part").stat().st_size >= 100_000
        hub.release.set()
        again = run("start", "--no-browser", "--port", str(port))
        assert again.returncode == 0, (again.stdout, again.stderr)
        status, listing = call("GET", token)
        assert model(listing, "tiny")["state"] == "interrupted" and listing["local_entries"] == 0
        assert call("POST", token, {"action": "start", "id": "tiny"})[0] == 202
        listing = wait_state(token, "tiny", "present")
        assert tree(models / "tiny") == standard_files() and listing["local_entries"] == 1
        assert call("POST", token, {"action": "start", "id": "named"})[0] == 202
        wait_state(token, "named", "present")
        assert (models / "model-q8_0.gguf").read_bytes() == BIG and not (models / ".lumabri-download-named").exists()
        assert call("POST", token, {"action": "start", "id": "tiny"}) == (409, {"error": "model_already_present"})
        # A finished model is not removed by a cancel.
        assert call("POST", token, {"action": "cancel", "id": "tiny"})[0] == 200
        assert tree(models / "tiny") == standard_files()
        present = run("fetch", "list")
        assert "present" in present.stdout, present.stdout

        # ---- a catalogue someone else could write is refused, not used ----
        owner_file.chmod(0o666)
        status, listing = call("GET", token)
        assert status == 200 and listing["catalogue_ok"] is False and listing["models"] == []
        assert call("POST", token, {"action": "start", "id": "giant"})[0] == 503
        owner_file.unlink()
        # The built-in list: real entries, none offered that this household cannot hold.
        status, listing = call("GET", token)
        assert status == 200 and listing["catalogue"] == "built_in" and listing["catalogue_ok"] is True
        assert [m["id"] for m in listing["models"]] == ["olmoe-1b-7b-instruct", "qwen36-35b", "qwen38-flash-next"]
        assert listing["unavailable_hidden"] == 0 and listing["models"][0]["size_bytes"] == 7359943136
        for entry in listing["models"]:
            assert entry["fits"] is False and entry["page_url"].startswith("https://huggingface.co/"), entry
            assert call("POST", token, {"action": "start", "id": entry["id"]})[0] == 409
        for asset in ("/download.js", "/download.css"):
            assert call("GET", path=asset)[0] == 200
    finally:
        hub.release.set()
        stopped = run("service", "stop")
    assert stopped.returncode == 0, stopped.stderr
    assert not state.exists() or lock_free(state)


def main():
    assert FETCH.exists(), "build lumabri-fetch first (make lumabri-fetch)"
    hub = Hub()
    threading.Thread(target=hub.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="lmb-download-") as temporary:
            helper_tests(Path(temporary), hub)
            gateway_tests(Path(temporary), hub)
    finally:
        hub.release.set()
        hub.shutdown()
        hub.server_close()
    print("model download: PASS")


if __name__ == "__main__":
    main()
