"""Update a generated build-options stamp only when the effective options change."""
import json
import os
from pathlib import Path
import sys
import tempfile


def main():
    path = Path(sys.argv[1])
    data = (json.dumps(sys.argv[2:], ensure_ascii=True) + "\n").encode()
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


if __name__ == "__main__":
    main()
