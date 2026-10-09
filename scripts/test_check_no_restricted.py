#!/usr/bin/env python3
"""Negative tests for check_no_restricted.py: a guard that never fires proves nothing."""
import hashlib, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import check_no_restricted as g


def repo(files: dict[str, bytes], fps: str = "") -> Path:
    d = Path(tempfile.mkdtemp(prefix="restricted-"))
    subprocess.run(["git", "init", "-q", str(d)], check=True)
    for rel, data in files.items():
        (d / rel).parent.mkdir(parents=True, exist_ok=True)
        (d / rel).write_bytes(data)
    if fps:
        (d / g.FINGERPRINTS).parent.mkdir(parents=True, exist_ok=True)
        (d / g.FINGERPRINTS).write_text(fps)
    subprocess.run(["git", "-C", str(d), "add", "-A", "-f"], check=True)
    return d


fails = 0
def expect(name, got, want):
    global fails
    ok = got == want
    print(("ok   " if ok else "FAIL ") + name)
    fails += 0 if ok else 1

def hits(d): return g.check(d, g.tracked(d))

expect("clean tree passes", hits(repo({"README.md": b"x"})), [])
expect("vault path flagged", len(hits(repo({"snow3g/vault/doc2.pdf": b"x"}))), 1)
expect("generated path flagged", len(hits(repo({"snow3g/generated/t.h": b"x"}))), 1)
expect(".sage suffix flagged", len(hits(repo({"a/b.sage": b"x"}))), 1)
secret = b"pretend-restricted-bytes"
fp = hashlib.sha256(secret).hexdigest() + "  # my package doc 2\n"
expect("fingerprint match flagged", len(hits(repo({"docs/innocent.txt": secret}, fp))), 1)
expect("non-matching file passes with fingerprints", hits(repo({"docs/ok.txt": b"other"}, fp)), [])
print(f"{fails} failure(s)")
sys.exit(1 if fails else 0)
