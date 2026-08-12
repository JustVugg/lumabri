#!/usr/bin/env python3
# A user-space TCP tap: listen on --listen, forward every connection to
# --to, and append every byte seen in both directions to --log. It needs no
# root and no tcpdump, so a test can prove what actually crosses a link on any
# machine — put the tap in front of a peer and grep its log for a plaintext
# marker. Transparent at the byte level: the encrypted handshake and frames
# pass through untouched, so what lands in the log is exactly what a passive
# observer on the wire would see.
import argparse, socket, threading

def pump(src, dst, log, lock):
    try:
        while True:
            b = src.recv(65536)
            if not b:
                break
            with lock:
                log.write(b); log.flush()
            dst.sendall(b)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass

def serve(listen_port, to_host, to_port, log_path):
    lock = threading.Lock()
    log = open(log_path, "ab")
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", listen_port))
    srv.listen(64)
    print("ready", flush=True)   # the test waits for this line before driving traffic
    while True:
        try:
            c, _ = srv.accept()
        except OSError:
            break
        try:
            u = socket.create_connection((to_host, to_port))
        except OSError:
            c.close(); continue
        threading.Thread(target=pump, args=(c, u, log, lock), daemon=True).start()
        threading.Thread(target=pump, args=(u, c, log, lock), daemon=True).start()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--to", required=True, help="host:port to forward to")
    ap.add_argument("--log", required=True)
    a = ap.parse_args()
    host, port = a.to.rsplit(":", 1)
    try:
        serve(a.listen, host, int(port), a.log)
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
