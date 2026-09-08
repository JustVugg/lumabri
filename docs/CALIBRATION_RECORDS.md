# Calibration records — integration in progress

`lumabri_calibration_store.h` supplies a private, bounded record format for
exact-key measurements. It is not yet a producer of catalogue speeds. The
remaining integration must obtain the actual participating runtime identities,
bind the checkpoint content independently of the request routing name, and
connect completed generation measurements to lookup/invalidation in the TUI.
Until that is done, the catalogue continues to say **not calibrated**.

The format stores the conditions and numerical measurements, not prompts,
replies, tokens or credentials. Records contain at most 32 nodes and 32 KiB.
Integers and binary64 values have an explicit little-endian representation;
C structure padding, locale and host endianness are not part of the format.

Each private directory holds one latest record per checkpoint. A temporary
file is written and synced before atomic replacement. A failed load clears
the output rather than leaving an earlier record available by accident.
Truncated, malformed, non-finite and checksum-invalid records are rejected,
as are unsafe names, symlinks, FIFOs, hardlinked files and non-private paths.
The checksum detects corruption; it does not attest a remote host's work.

Keys require all identity and execution fields. Empty keys, unterminated
strings, unknown thread counts and empty ranges cannot establish a match.
Changing any matched condition makes a previous speed stale. The producer
must still verify where each field came from: passing structural validation
does not make invented build IDs or a partial key authoritative.

Run `make test_calibration && ./test_calibration`. Native macOS workspace CI
runs the same record and key tests; Linux also runs them in the ordinary suite.
