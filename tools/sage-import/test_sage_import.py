#!/usr/bin/env python3
"""Tests for sage_import stage 1. Uses throw-away dummy files; no real package content."""
import json, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
import sage_import as s

fails = 0
def expect(name, cond):
    global fails
    print(("ok   " if cond else "FAIL ") + name)
    fails += 0 if cond else 1

def mkvault(files=None, manifest=None):
    v = Path(tempfile.mkdtemp(prefix="vault-"))
    for n, d in (files or {"a.bin": b"AAA", "b.bin": b"BBB"}).items():
        (v / n).write_bytes(d)
    m = manifest if manifest is not None else {"package_id": "p1", "files": {
        "doc1": {"path": "a.bin", "expected_version": "1.1"}, "doc3": {"path": "b.bin"}}}
    (v / s.MANIFEST).write_text(json.dumps(m))
    return v

def run(*args): return subprocess.run([sys.executable, str(HERE / "sage_import.py"), *args], capture_output=True, text=True)

v = mkvault()
expect("ingest succeeds", run("ingest", str(v)).returncode == 0)
lock = json.loads((v / s.LOCK).read_text())
expect("lock has sha256 + size, no content", lock["files"]["doc1"]["size"] == 3 and "AAA" not in json.dumps(lock))
expect("verify OK on untouched vault", run("verify", str(v)).returncode == 0)
(v / "a.bin").write_bytes(b"AAX")
expect("verify detects a modified file", run("verify", str(v)).returncode == 1)
expect("re-ingest of changed package refused", run("ingest", str(v)).returncode == 1)
expect("--accept-new-package re-locks", run("ingest", str(v), "--accept-new-package").returncode == 0 and run("verify", str(v)).returncode == 0)
expect("missing manifest -> error", run("ingest", str(mkvault(manifest={}).joinpath("nope"))).returncode == 2)
expect("path traversal rejected", run("ingest", str(mkvault(manifest={"package_id": "x", "files": {"d": {"path": "../x"}}}))).returncode == 2)
expect("absolute path rejected", run("ingest", str(mkvault(manifest={"package_id": "x", "files": {"d": {"path": "/etc/passwd"}}}))).returncode == 2)
expect("missing listed file rejected", run("ingest", str(mkvault(manifest={"package_id": "x", "files": {"d": {"path": "zzz"}}}))).returncode == 2)
# a vault inside a git work tree and NOT ignored must be refused
g = Path(tempfile.mkdtemp(prefix="gitrepo-")); subprocess.run(["git", "init", "-q", str(g)], check=True)
inner = g / "vault"; inner.mkdir(); (inner / "a.bin").write_bytes(b"x")
(inner / s.MANIFEST).write_text(json.dumps({"package_id": "x", "files": {"d": {"path": "a.bin"}}}))
expect("unignored vault inside a git tree refused", run("ingest", str(inner)).returncode == 2)
(g / ".gitignore").write_text("vault/\n")
expect("ignored vault inside a git tree accepted", run("ingest", str(inner)).returncode == 0)
print(f"{fails} failure(s)"); sys.exit(1 if fails else 0)
