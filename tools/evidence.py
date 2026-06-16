#!/usr/bin/env python3
"""Collect a source-bound diagnostic bundle from a clean checkout."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time


def write(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def digest(path):
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def git(*args):
    return subprocess.check_output(["git", *args])


def inventory(output):
    return {path.relative_to(output).as_posix(): {"sha256": digest(path), "bytes": path.stat().st_size}
            for path in sorted(output.rglob("*")) if path.is_file() and path.name != "MANIFEST.json"}


class Recorder:
    def __init__(self, output):
        self.output = output
        self.commands = []
        (output / "logs").mkdir()

    def run(self, name, args, required=True, stdout=None, timeout=180):
        start = time.monotonic()
        argv = list(map(str, args))
        try:
            process = subprocess.run(argv, text=True, encoding="utf-8", errors="replace",
                                     capture_output=True, timeout=timeout)
            code, out, err = process.returncode, process.stdout, process.stderr
        except subprocess.TimeoutExpired as error:
            code = 124
            out = (error.stdout or b"").decode(errors="replace")
            err = (error.stderr or b"").decode(errors="replace") + "\nTIMEOUT\n"
        except OSError as error:
            code, out, err = 125, "", str(error)
        destination = stdout if stdout is not None else self.output / "logs" / (name + ".txt")
        destination.write_text(out)
        (self.output / "logs" / (name + ".stderr")).write_text(err)
        row = {"name": name, "argv": argv, "exit": code, "required": required,
               "seconds": time.monotonic() - start, "stdout": destination.relative_to(self.output).as_posix()}
        self.commands.append(row)
        with (self.output / "COMMANDS.jsonl").open("a") as log:
            log.write(json.dumps(row) + "\n")
        return code


def collect(build, output, device, profile, gallery):
    if git("status", "--porcelain", "--untracked-files=normal").strip():
        raise ValueError("campaign requires a clean checkout")
    commit = git("rev-parse", "HEAD").decode().strip()
    paths = [Path(x.decode()) for x in git("ls-files", "-z").split(b"\0") if x]
    output.mkdir(parents=True, exist_ok=False)
    write(output / "SOURCE_INPUTS.json", {"commit": commit, "files": {str(path): digest(path) for path in paths}})
    write(output / "DATASETS.json", {"external": [], "fixture": "tests/fixtures/tempo-probe.rsn",
                                    "reference_patch": "patches/reference-64.rpatch",
                                    "spectral_protocol": "docs/EXPERIMENT_PLAN.md"})
    engine = (build / "resonant").resolve()
    write(output / "ENVIRONMENT.json", {
        "platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
        "engine_sha256": digest(engine) if engine.is_file() else None,
        "build_config": "caller-supplied clean build",
    })
    if profile == "full":
        write(output / "ACCEPTANCE.json", {"profile": profile, "source_commit": commit, "status": "blocked",
              "reasons": ["eight-hour offline fault campaign is not implemented",
                          "callback-wide instrumentation for the device campaign is incomplete",
                          "full requirement and release gates are incomplete"]})
        write(output / "MANIFEST.json", inventory(output))
        return 1

    recorder = Recorder(output)
    probe = output / "probe"
    probe.mkdir()
    fixture = Path("tests/fixtures/tempo-probe.rsn")
    session = probe / "score.rsn"
    shutil.copyfile(fixture, session)
    patch = probe / "patch.rpatch"
    patch.write_bytes(session.read_bytes().split(b"\nsource\n", 1)[1])
    program = probe / "program.rbsc"
    rate = 48000
    recorder.run("compiler", [engine, "compile", patch, program])
    recorder.run("verifier", [engine, "verify", program])
    recorder.run("event_tape", [engine, "schedule", session, rate], stdout=probe / "events.csv")
    recorder.run("render", [engine, "render", session, probe / "audio.wav", rate, 128], stdout=probe / "render.txt")
    recorder.run("tests", ["ctest", "--test-dir", build, "-V"])
    recorder.run("swap", [build / "resonant_campaigns", "swap"])
    recorder.run("allocation", [build / "resonant_campaigns", "audit"])
    recorder.run("compiler_version", ["c++", "--version"])
    recorder.run("cmake_version", ["cmake", "--version"])
    recorder.run("doctor", [engine, "doctor"], required=False)
    recorder.run("spectral", [sys.executable, "tools/measure_dsp.py", engine, output / "spectral"])
    parity = output / "parity"
    parity.mkdir()
    for sample_rate in (44100, 48000, 96000):
        for block in (32, 64, 128, 256, 512):
            name = f"{sample_rate}-{block}"
            recorder.run("block-" + name, [engine, "render", session, parity / (name + ".wav"), sample_rate, block])
        recorder.run(f"scalar-{sample_rate}",
                     [engine, "render", session, parity / f"{sample_rate}-scalar.wav", sample_rate, 128, "pcm24", "scalar"])
    if gallery:
        recorder.run("gallery", [sys.executable, "tools/gallery.py", engine, output / "gallery"])
    if device:
        recorder.run("device", [engine, "play", "patches/reference-64.rpatch", 10, 0, -1, 48000, 128,
                                output / "device.csv"])
        app = build / "Resonant.app" / "Contents" / "MacOS" / "Resonant"
        if app.is_file():
            recorder.run("workbench", [app, "--smoke", output / "workbench"])
    failures = [row["name"] for row in recorder.commands if row["required"] and row["exit"] != 0]
    write(output / "ACCEPTANCE.json", {"profile": profile, "source_commit": commit, "sample_rate": rate,
          "probe_file": fixture.as_posix(), "status": "failed" if failures else "diagnostic",
          "command_failures": failures, "matrix": {"rates": [44100, 48000, 96000],
          "blocks": [32, 64, 128, 256, 512]}, "device_requested": device, "gallery_requested": gallery})
    coverage = {
        "G01": ["tests/wave.cpp", "logs/tests.txt"],
        "G02": ["tests/differential.cpp", "logs/tests.txt"],
        "G03": ["tests/bytecode.cpp", "logs/tests.txt"],
        "G04": ["tests/campaigns.cpp", "logs/allocation.txt"],
        "G05": ["tests/spectral.cpp", "logs/tests.txt"],
        "G06": ["tools/measure_dsp.py", "spectral/spectral.csv"],
        "G07": ["src/simd.cpp", "parity/"],
        "G08": ["src/session.cpp", "probe/events.csv"],
        "G09": ["src/engine.cpp", "logs/swap.txt"],
        "G10": ["src/device.cpp", "device.csv"],
        "G12": ["src/apple/workbench.mm", "workbench/workbench.txt"],
        "G13": ["tools/gallery.py", "gallery/manifest.json"],
        "G14": ["tools/verify_evidence.py", "probe/"],
    }
    write(output / "COVERAGE.json", {"scope": "diagnostic observations", "entries": coverage})
    write(output / "MANIFEST.json", inventory(output))
    print(f"{profile} evidence: {output} ({len(failures)} required command failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("build", type=Path)
    cli.add_argument("output", type=Path)
    cli.add_argument("--profile", choices=("diagnostic", "full"), default="diagnostic")
    cli.add_argument("--device", action="store_true")
    cli.add_argument("--gallery", action="store_true")
    args = cli.parse_args()
    raise SystemExit(collect(args.build.resolve(), args.output.resolve(), args.device, args.profile, args.gallery))
