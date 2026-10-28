"""MCP wire/authority tests plus the real authenticated Lumabri API."""
import http.server
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]


class Agent:
    def __init__(self, binary, token_file, port, log, manage=False):
        args = [str(binary), "--token-file", str(token_file), "--port", str(port)]
        if manage:
            args.append("--allow-management")
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=log, text=True)
        self.sequence = 0

    def call(self, method, params=None):
        self.sequence += 1
        request = {"jsonrpc": "2.0", "id": self.sequence, "method": method}
        if params is not None:
            request["params"] = params
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 20)[0], "MCP response timed out"
        response = json.loads(self.process.stdout.readline())
        assert response["id"] == self.sequence and response["jsonrpc"] == "2.0", response
        return response

    def initialize(self):
        response = self.call("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                            "clientInfo": {"name": "lumabri-native-test", "version": "1"}})
        assert response["result"]["protocolVersion"] == "2025-11-25"
        self.process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()

    def tool(self, name, arguments=None):
        return self.call("tools/call", {"name": name, "arguments": arguments or {}})

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise
        assert self.process.returncode == 0


def content(response):
    return json.loads(response["result"]["content"][0]["text"])


def verify_resident_agent(api, allocations):
    """Called after real two-model preparation; subsequent flow runs inference."""
    artifacts = Path(api.env["HOME"]).parent
    credential = artifacts / "agent-control-token"
    credential.write_text(api.token + "\n")
    credential.chmod(0o600)
    with (artifacts / "resident-agent.log").open("w") as log:
        agent = Agent(api.runtime / "lumabri-agent", credential, api.port, log, manage=True)
        try:
            agent.initialize()
            operation = agent.tool("lumabri_preparation", {"request": {"action": "status", "operation": api.operation}})
            assert not operation["result"]["isError"] and content(operation)["state"] == "done"
            first, second = [a["id"] for a in allocations]
            def observe(allocation):
                reply = agent.tool("lumabri_allocation_status", {"allocation": allocation})
                assert not reply["result"]["isError"], reply
                return content(reply)
            original, other = observe(first), observe(second)
            assert original["state"] == other["state"] == "accepting"
            request = {"action": "drain", "instance": original["instance"], "revision": original["revision"]}
            drain = agent.tool("lumabri_allocation_control", {"allocation": first, "request": request})
            assert not drain["result"]["isError"], drain
            deadline = time.monotonic() + 10
            while True:
                drained = observe(first)
                if drained["state"] == "drained":
                    break
                assert time.monotonic() < deadline, drained
                time.sleep(.05)
            assert observe(second) == other, "draining one model changed another"
            resume = agent.tool("lumabri_allocation_control", {"allocation": first, "request": {
                "action": "resume", "instance": drained["instance"], "revision": drained["revision"]}})
            assert not resume["result"]["isError"], resume
            resumed = observe(first)
            assert resumed["state"] == "accepting" and resumed["instance"] == original["instance"]
            assert not resumed["weights_unloaded"]
            stale = agent.tool("lumabri_allocation_control", {"allocation": first, "request": request})
            assert stale["result"]["isError"], "old fence was replayed by the agent"
            assert observe(first)["state"] == "accepting" and observe(second) == other
        finally:
            agent.close()
            credential.unlink()
    print("RESIDENT AGENT: PASS (two real models, durable status, drain/resume, unchanged neighbour, stale-fence refusal)", flush=True)


def main():
    binary = ROOT / "lumabri-agent"
    with tempfile.TemporaryDirectory(prefix="lmb-agent-test-") as directory:
        home = Path(directory)
        token_file = home / "token"
        token = "agent." + "a" * 64
        token_file.write_text(token + "\n")
        token_file.chmod(0o600)
        requests = []
        state = {"mode": "normal"}

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def do_GET(self):
                self.handle_request()

            def do_POST(self):
                self.handle_request()

            def handle_request(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                requests.append((self.command, self.path, body))
                assert self.headers["Origin"] == f"http://127.0.0.1:{self.server.server_port}"
                status = 200 if self.headers["Authorization"] == "Bearer " + token else 401
                data = json.dumps({"status": status, "method": self.command, "path": self.path}).encode()
                self.send_response(302 if state["mode"] == "redirect" else status)
                self.send_header("Content-Type", "application/json")
                if state["mode"] == "chunked":
                    self.send_header("Transfer-Encoding", "chunked")
                if state["mode"] == "duplicate":
                    self.send_header("Content-Length", str(len(data)))
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        with (home / "agent.log").open("w+") as log:
            agent = Agent(binary, token_file, server.server_port, log)
            try:
                assert agent.call("tools/list")["error"]["code"] == -32002
                agent.initialize()
                tools = agent.call("tools/list")["result"]["tools"]
                assert len(tools) == 8 and all(t["annotations"]["readOnlyHint"] for t in tools)
                documents = agent.call("resources/list")["result"]["resources"]
                assert len(documents) == 5
                doc = agent.call("resources/read", {"uri": "lumabri://docs/preparation"})
                assert doc["result"]["contents"][0]["text"] == (ROOT / "docs/MODEL_PREPARATION_API.md").read_text()
                assert "error" in agent.call("resources/read", {"uri": "file:///etc/passwd"})
                result = agent.tool("lumabri_workspace")
                assert not result["result"]["isError"] and content(result)["path"] == "/api/v1/workspace"
                count = len(requests)
                assert "error" in agent.tool("lumabri_preparation", {"request": {"action": "start"}})
                assert "error" in agent.tool("lumabri_workspace", {"url": "http://example.invalid"})
                assert "error" in agent.tool("lumabri_allocation_status", {"allocation": "../secret"})
                assert len(requests) == count
                token_file.chmod(0o644)
                assert agent.tool("lumabri_models")["result"]["isError"]
                assert len(requests) == count
                token_file.chmod(0o600)
                token_file.write_text("agent." + "b" * 64)
                rejected = agent.tool("lumabri_models")
                assert rejected["result"]["isError"] and content(rejected)["status"] == 401
                token_file.write_text(token + "\n")
                for mode in ("chunked", "duplicate", "redirect"):
                    state["mode"] = mode
                    count = len(requests)
                    assert agent.tool("lumabri_models")["result"]["isError"]
                    assert len(requests) == count + 1, "must not redirect or retry"
                state["mode"] = "normal"
            finally:
                agent.close()

            managed = Agent(binary, token_file, server.server_port, log, manage=True)
            try:
                managed.initialize()
                assert len(managed.call("tools/list")["result"]["tools"]) == 15
                request = {"action": "status", "operation": "1" * 64}
                result = managed.tool("lumabri_preparation", {"request": request})
                assert not result["result"]["isError"]
                assert requests[-1][:2] == ("POST", "/api/v1/preparation")
                assert json.loads(requests[-1][2]) == request
                fence = {"action": "drain", "instance": "2" * 64, "revision": "18446744073709551614"}
                result = managed.tool("lumabri_allocation_control", {"allocation": "3" * 64, "request": fence})
                assert not result["result"]["isError"]
                assert json.loads(requests[-1][2]) == fence, "revision must not round through float"
            finally:
                managed.close()
        server.shutdown()
        server.server_close()
        assert token not in (home / "agent.log").read_text()

        # Exercise real credential grants/revocation, not just HTTP doubles.
        env = {**os.environ, "HOME": str(home), "LUMABRI_PEER_KEY": str(home / "peer.key"),
               "LUMABRI_KNOWN_HOSTS": str(home / "known"), "LUMABRI_ENCRYPT": "1"}
        env.pop("LUMABRI_TOKEN", None)
        def cli(*args):
            return subprocess.run([str(ROOT / "lumabri"), *args], env=env, capture_output=True,
                                  text=True, check=True, timeout=20).stdout.strip()
        token_file.write_text(cli("api", "user-add", "agent-test") + "\n")
        cli("api", "operator-grant", "agent-test")
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
        with (home / "real-api.log").open("w+") as log:
            api = subprocess.Popen([str(ROOT / "lumabri"), "api", "serve", "--port", str(port), "--tracker", "127.0.0.1:1"],
                                   env=env, stdout=log, stderr=log)
            agent = None
            try:
                deadline = time.monotonic() + 10
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), .1):
                            break
                    except OSError:
                        assert api.poll() is None and time.monotonic() < deadline, (home / "real-api.log").read_text()
                        time.sleep(.05)
                agent = Agent(binary, token_file, port, log, manage=True)
                agent.initialize()
                assert not agent.tool("lumabri_models")["result"]["isError"]
                refused = agent.tool("lumabri_capacity_check", {"request": {"record": "missing", "ttft_ms": 10, "gap_ms": 10, "max_age_seconds": 60}})
                assert refused["result"]["isError"] and "management" in json.dumps(content(refused)).lower()
                cli("api", "manage-grant", "agent-test")
                allowed = agent.tool("lumabri_capacity_check", {"request": {"record": "missing", "ttft_ms": 10, "gap_ms": 10, "max_age_seconds": 60}})
                assert "management" not in json.dumps(content(allowed)).lower()
                cli("api", "manage-revoke", "agent-test")
                refused = agent.tool("lumabri_capacity_check", {"request": {"record": "missing", "ttft_ms": 10, "gap_ms": 10, "max_age_seconds": 60}})
                assert refused["result"]["isError"] and "management" in json.dumps(content(refused)).lower()
            finally:
                if agent:
                    agent.close()
                api.terminate()
                api.wait(timeout=20)
        print("AGENT CONTROL: PASS (MCP, bounded HTTP, fixed tools, private token, live API grants/revocation)")


if __name__ == "__main__":
    main()
