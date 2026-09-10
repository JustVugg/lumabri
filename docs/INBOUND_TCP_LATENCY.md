# Low-latency replies as well as requests

The client connector already enabled `TCP_NODELAY`. Accepted server sockets
did not. Lumabri writes a frame's header, payload and (when encrypted) tag
separately; leaving Nagle enabled on replies can interact with delayed ACKs
and add tens of milliseconds to a small request/reply exchange even on LAN.

The accepted-socket entry point now enables `TCP_NODELAY` before any handshake,
including when transport encryption is disabled. This is applied to the actual
accepted IPv4/IPv6 socket, not inferred from listener inheritance: launchers
may supply a pre-bound descriptor. Unix-domain sockets are unchanged. A socket
option error rejects the connection rather than claiming the policy was set.
Framing, encryption, authentication, approval and model arithmetic are unchanged.

`test_tcp_latency` checks the outbound and inbound socket options, Unix sockets,
invalid descriptors and byte equality through real AEAD record send/receive.
Fixed test keys cover record transport only, not identity authentication; the
household integration test covers the real authenticated path.

The test also prints an informational A/B with the legacy server option forced
off only inside the test. One Linux loopback run of 24 short encrypted exchanges
averaged 42.306 ms with the legacy option and 0.356 ms with NODELAY. Timing is
not a CI threshold, a physical-LAN measurement or model tok/s. Actual gains
depend on payload, TCP behavior, compute and topology; per-range chat observations
are the way to check the complete path.
