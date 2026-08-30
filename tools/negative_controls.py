#!/usr/bin/env python3
"""Run actual verifier subprocesses on independently altered bundles."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def rehash(root):
    manifest = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and path != root / "MANIFEST.json":
            data = path.read_bytes()
            manifest[path.relative_to(root).as_posix()] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    (root / "MANIFEST.json").write_text(json.dumps(manifest, sort_keys=True) + "\n")


def unsafe(root):
    path = root / "probe/program.rbsc"
    data = bytearray(path.read_bytes())
    struct.pack_into("<H", data, len(data) - 12, 65535)
    path.write_bytes(data)


def count(root):
    path = root / "probe/render.txt"
    path.write_text(re.sub(r"frames=\d+", "frames=12345", path.read_text(), count=1))


def timing(root):
    path = root / "probe/events.csv"
    header, first, rest = path.read_text().split("\n", 2)
    time, event = first.split(",", 1)
    path.write_text(f"{header}\n{int(time) + 1},{event}\n{rest}")


def audio(root):
    path = root / "probe/audio.wav"
    data = bytearray(path.read_bytes())
    location = data.index(b"data", 12) + 8 + 8000
    data[location] ^= 1
    path.write_bytes(data)


def source(root):
    path = root / "SOURCE_INPUTS.json"
    doc = json.loads(path.read_text())
    doc["commit"] = "0" * 40
    path.write_text(json.dumps(doc))


def missing(root):
    (root / "probe/audio.wav").unlink()


def capture_count(root):
    path = root / "workbench/capture.wav"
    data = path.read_bytes()
    count = re.search(rb"Resonant capture frames=(\d+)", data)
    if not count:
        raise ValueError("capture metadata unavailable")
    data = data[:count.start(1)] + b"9" * len(count[1]) + data[count.end(1):]
    path.write_bytes(data)


def export_program(root):
    path = root / "workbench/export.wav"
    data = path.read_bytes()
    tag = re.search(rb" program_id=([0-9a-f]+)\0", data)
    if not tag:
        raise ValueError("export metadata unavailable")
    data = data[:tag.start(1)] + b"0" * len(tag[1]) + data[tag.end(1):]
    path.write_bytes(data)


def exercise(bundle, executable, output):
    verifier = [sys.executable, "tools/verify_evidence.py"]
    cases = [
        ("unsafe", unsafe, True, "bytecode operand outside defined registers"),
        ("frames", count, True, "render receipt sample count differs"),
        ("midi", timing, True, "event tape timing or payload differs"),
        ("pcm", audio, True, "bounded replay PCM differs"),
        ("corruption", audio, False, "artifact digest mismatch"),
        ("missing", missing, False, "artifact inventory mismatch"),
        ("source", source, True, "source commit differs"),
        ("replay", None, False, "replay subprocess failed"),
    ]
    if (bundle / "workbench/capture.wav").exists():
        cases.extend([
            ("capture_count", capture_count, True, "capture metadata frame count differs"),
            ("export_program", export_program, True, "workbench export program identity differs"),
        ])
    report = []
    with tempfile.TemporaryDirectory(prefix="resonant-negative-") as temporary:
        root = Path(temporary)
        for name, mutate, rehashed, reason in cases:
            selected = bundle
            if mutate is not None:
                selected = root / name
                shutil.copytree(bundle, selected)
                mutate(selected)
                if rehashed:
                    rehash(selected)
            engine = "/usr/bin/false" if name == "replay" else str(executable)
            argv = [*verifier, str(selected), "--engine", engine]
            result = subprocess.run(argv, text=True, capture_output=True, timeout=90)
            report.append({"case": name, "rehash": rehashed, "argv": argv, "exit": result.returncode,
                           "stdout": result.stdout.strip(), "stderr": result.stderr.strip()})
            if mutate is not None:
                shutil.rmtree(selected)
            if result.returncode != 1 or reason not in result.stderr:
                output.write_text(json.dumps(report, indent=2) + "\n")
                raise ValueError(f"{name}: expected rejection containing {reason!r}; got {result.stderr!r}")
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"verifier rejected all {len(cases)} altered bundles for the expected reasons")


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("bundle", type=Path)
    cli.add_argument("engine", type=Path)
    cli.add_argument("output", type=Path)
    args = cli.parse_args()
    exercise(args.bundle.resolve(), args.engine.resolve(), args.output)
