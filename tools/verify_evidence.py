#!/usr/bin/env python3
"""Independently verify a Resonant diagnostic bundle against a trusted checkout."""

import argparse
import cmath
import csv
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import wave


RATES = (44100, 48000, 96000)
BLOCKS = (32, 64, 128, 256, 512)
BINS = (19, 233, 773)
SPECTRAL = ("sine", "saw", "pulse", "triangle", "oversaturate", "oversaturate_hq")
FIXTURE = "tests/fixtures/tempo-probe.rsn"


class Rejection(Exception):
    pass


def require(condition, reason):
    if not condition:
        raise Rejection(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def document(path):
    require(path.stat().st_size <= 4 * 1024 * 1024, "oversized JSON artifact")
    return json.loads(path.read_text(), object_pairs_hook=pairs,
                      parse_constant=lambda x: require(False, f"nonfinite JSON constant {x}"))


def command(args, cwd=None):
    try:
        result = subprocess.run(list(map(str, args)), cwd=cwd, check=True, capture_output=True, timeout=60)
    except subprocess.CalledProcessError as error:
        raise Rejection(f"replay subprocess failed: {Path(str(args[0])).name} returned {error.returncode}") from error
    return result.stdout


def verify_inventory(root):
    manifest = document(root / "MANIFEST.json")
    observed = {}
    size = 0
    for path in root.rglob("*"):
        require(not path.is_symlink(), "artifact contains a symbolic link")
        if path.is_dir() or path == root / "MANIFEST.json":
            continue
        require(path.is_file(), "artifact is not a regular file")
        count = path.stat().st_size
        size += count
        require(size <= 256 * 1024 * 1024 and len(observed) < 2048, "diagnostic bundle exceeds verification limits")
        observed[path.relative_to(root).as_posix()] = (path, count)
    require(set(manifest) == set(observed), "artifact inventory mismatch")
    for name, (path, size) in observed.items():
        row = manifest[name]
        require(row["bytes"] == size and row["sha256"] == file_sha(path), f"artifact digest mismatch: {name}")
    return set(observed)


def verify_source(root, repo, expected):
    actual = command(["git", "rev-parse", "--verify", f"{expected}^{{commit}}"], repo).decode().strip()
    claim = document(root / "SOURCE_INPUTS.json")
    require(claim["commit"] == actual, "source commit differs from the trusted checkout")
    listing = command(["git", "ls-tree", "-r", "-z", "--name-only", actual], repo)
    paths = {entry.decode() for entry in listing.split(b"\0") if entry}
    require(set(claim["files"]) == paths, "source input list differs from the trusted tree")
    trusted = {}
    for path, declared in claim["files"].items():
        blob = command(["git", "show", f"{actual}:{path}"], repo)
        require(sha(blob) == declared, f"source digest differs for {path}")
        trusted[path] = declared
    return actual, trusted


def fnv(name):
    state = 0xCBF29CE484222325
    for byte in name.encode("ascii"):
        state = ((state ^ byte) * 0x100000001B3) & ((1 << 64) - 1)
    return state


def reference_timeline(session, rate):
    text = session.decode()
    header, source = text.split("\nsource\n", 1)
    rows = [line.split() for line in header.splitlines()]
    require(rows[0] == ["resonant-session", "2"], "unexpected probe session version")
    end = int(rows[1][1])
    tempo = [(int(row[1]), int(row[2])) for row in rows[2:] if row[0] == "tempo"]
    require(tempo[0][0] == 0, "probe tempo map has no origin")

    def frame(tick):
        elapsed = Fraction()
        for i, (start, usec) in enumerate(tempo):
            stop = tempo[i+1][0] if i+1 < len(tempo) else end
            elapsed += Fraction(max(0, min(stop, tick) - start) * usec * rate, 960_000_000)
        return math.floor(elapsed + Fraction(1, 2))

    events = []
    for row in rows[2:]:
        kind = row[0]
        if kind == "tempo":
            continue
        tick = int(row[1])
        when = frame(tick)
        if kind == "note":
            duration, channel, note = map(int, row[2:5])
            events.extend([(when, 1, channel, note, 0, float(row[5]), 0),
                           (frame(tick + duration), 2, channel, note, 0, 0., 0)])
        elif kind == "pedal":
            events.append((when, 3, int(row[2]), 0, 0, float(row[3]), 0))
        elif kind == "control":
            events.append((when, 0, 0, 0, fnv(row[2]), float(row[3]), int(row[4])))
        else:
            raise Rejection("unexpected directive in trusted probe")
    ordering = {2: 0, 3: 1, 0: 2, 1: 3}
    events.sort(key=lambda item: (item[0], ordering[item[1]]))
    return frame(end), events, source.encode()


def check_events(path, frames, events):
    rows = list(csv.reader(path.read_text().splitlines()))
    require(rows and rows[0] == ["frames", str(frames)], "event tape sample count differs from tempo map")
    require(len(rows) == len(events) + 1, "event tape count differs")
    for observed, wanted in zip(rows[1:], events):
        require(len(observed) == 7, "event tape column count differs")
        for i, (a, b) in enumerate(zip(observed, wanted)):
            match = math.isclose(float(a), b, rel_tol=2e-7, abs_tol=1e-8) if i == 5 else int(a) == b
            require(match, "event tape timing or payload differs from independent timeline")


def structural_probe(blob):
    require(len(blob) >= 20, "bytecode header is missing")
    magic, version, registers, states, params, count, zero = struct.unpack_from("<4sHHHHII", blob)
    require(magic == b"RBSC" and version == 1 and zero == 0, "bytecode header is invalid")
    require(1 <= registers <= 256 and states == 1 and params == 1 and 1 <= count <= 32,
            "bytecode probe layout out of bounds")
    require(len(blob) == 20 + (states + params + count) * 16, "bytecode record extent mismatch")
    state_key, owner, words = struct.unpack_from("<QII", blob, 20)
    key, initial, reserved = struct.unpack_from("<QfI", blob, 36)
    require((state_key, owner, words) == (fnv("v"), 20, 512), "bytecode voice layout differs")
    require(key == fnv("level") and initial == .25 and reserved == 0, "bytecode parameter layout differs")
    # This independent verifier accepts the opcodes used by the frozen probe.
    arity = {0: 0, 1: 0, 4: 2, 18: 2, 19: 2, 20: 2, 29: 2}
    used = {1: 0, 20: 0, 29: 0}
    defined = {}
    for i in range(count):
        op, rate, dst, x, y, z, slot, immediate = struct.unpack_from("<BBHHHHHI", blob, 52 + 16*i)
        require(op in arity and (op == 29) == (i == count-1), "unknown or misplaced bytecode opcode")
        operands = (x, y, z)
        inputs = operands[:arity[op]]
        require(all(r in defined for r in inputs), "bytecode operand outside defined registers")
        require(slot == 0 and not any(operands[arity[op]:]), "nonzero unused bytecode fields")
        need = int(op in {18, 19, 20, 29} or any(defined[r] for r in inputs))
        require(rate == need, "bytecode rate mismatch")
        if op in {18, 19, 29}:
            require(defined[x] == 1 and (op != 29 or defined[y] == 1), "bytecode audio port receives control data")
        if op == 29:
            require(dst == 0, "nonzero output destination")
        else:
            require(dst < registers and dst not in inputs, "unsafe bytecode destination alias or bounds")
            defined[dst] = rate
        if op == 0:
            value, = struct.unpack("<f", struct.pack("<I", immediate))
            require(math.isfinite(value) and abs(value) <= 1048576, "bytecode literal outside signal bounds")
        else:
            require(immediate == 0, "noncanonical bytecode immediate")
        if op in used:
            used[op] += 1
    require(all(value == 1 for value in used.values()), "bytecode state or output ownership mismatch")


def pcm24(path, rate, frames=None):
    with wave.open(str(path), "rb") as audio:
        require((audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) == (2, 3, rate),
                "PCM format does not match stereo PCM24")
        total = audio.getnframes()
        require(0 < total <= 10_000_000 and (frames is None or total == frames), "PCM sample count differs")
        data = audio.readframes(total)
        require(len(data) == total * 6 and audio.readframes(1) == b"", "PCM extent mismatch")
    return data


def fft(data):
    n = len(data)
    buf = list(map(complex, data))
    pos = 0
    for i in range(1, n):
        bit = n >> 1
        while bit and pos & bit:
            pos ^= bit
            bit >>= 1
        pos ^= bit
        if i < pos:
            buf[i], buf[pos] = buf[pos], buf[i]
    width = 2
    while width <= n:
        twiddles = [cmath.exp(-2j * math.pi * j / width) for j in range(width // 2)]
        for begin in range(0, n, width):
            for j, root in enumerate(twiddles):
                a = begin + j
                b = a + width // 2
                right = root * buf[b]
                buf[a], buf[b] = buf[a] + right, buf[a] - right
        width *= 2
    return buf


def read_float_audio(path, rate):
    data = path.read_bytes()
    magic, declared, wave_tag = struct.unpack_from("<4sI4s", data)
    require((magic, declared + 8, wave_tag) == (b"RIFF", len(data), b"WAVE"), "invalid float diagnostic WAVE")
    offset = 12
    fmt = None
    signal = None
    while offset < len(data):
        tag, size = struct.unpack_from("<4sI", data, offset)
        offset += 8
        require(offset + size + (size & 1) <= len(data), "truncated diagnostic WAVE")
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, offset)
        elif tag == b"data":
            signal = data[offset:offset+size]
        offset += size + (size & 1)
    require(fmt == (3, 2, rate, 8*rate, 8, 32) and signal is not None and len(signal) == 8192*8,
            "float diagnostic format differs")
    tail = [x for x, _ in struct.iter_unpack("<ff", signal[4096*8:])]
    require(all(math.isfinite(x) for x in tail), "nonfinite diagnostic audio")
    return tail


def check_spectral(root):
    with (root / "spectral/spectral.csv").open() as file:
        table = list(csv.DictReader(file))
    expected = {(name, rate, k) for name in SPECTRAL for rate in RATES for k in BINS}
    require(len(table) == len(expected), "incomplete spectral matrix")
    worst = {}
    for row in table:
        case, rate, k = row["case"], int(row["rate"]), int(row["bin"])
        require((case, rate, k) in expected, "duplicate or unexpected spectral case")
        expected.remove((case, rate, k))
        n = 4096
        spectrum = fft(read_float_audio(root / f"spectral/{case}-{rate}-{k}.wav", rate))
        energy = [(abs(spectrum[i]) ** 2) * (1 if i == n//2 else 2) / n for i in range(1, n//2+1)]
        harmonic = math.fsum(energy[i-1] for i in range(1, n//2+1) if i % k == 0)
        residual = math.fsum(energy[i-1] for i in range(1, n//2+1) if i % k != 0)
        require(harmonic > 0 and math.isfinite(harmonic) and math.isfinite(residual), "invalid spectral energy")
        level = 10 * math.log10(max(residual, 1e-18) / harmonic)
        claimed = float(row["residual_dbc"])
        require(math.isfinite(claimed) and (abs(level-claimed) <= .1 or (level < -100 and claimed < -100)),
                "spectral result differs from independent FFT")
        require((level <= -60) == (row["passed"] == "True"), "spectral threshold decision differs")
        require(case == "oversaturate" or level <= -60, "required spectral case misses -60 dBc")
        worst[case] = max(worst.get(case, -math.inf), level)
    return {key: round(value, 2) for key, value in worst.items()}


def check_device(root):
    durations = []
    frames = 0
    flags = 0
    voices = 0
    with (root / "device.csv").open() as file:
        for row in csv.DictReader(file):
            n = int(row["frames"])
            duration = int(row["ns"])
            require(n == 128 and int(row["frame"]) == frames and 0 < duration < 2_666_667,
                    "device frame accounting or deadline mismatch")
            require(int(row["generation"]) == 1 and float(row["peak"]) == 0, "device diagnostic was not muted")
            frames += n
            durations.append(duration)
            flags |= int(row["flags"])
            voices = max(voices, int(row["voices"]))
    require(len(durations) >= 3000 and voices == 64, "device diagnostic work missing")
    p999 = sorted(durations)[math.ceil(.999 * len(durations))-1]
    require(p999 < 1_866_667, "device diagnostic misses engine p99.9 budget")
    return {"callbacks": len(durations), "frames": frames, "p999_ns": p999, "flag_mask": flags, "voices": voices}


def check_gallery(root, trusted):
    rows = document(root / "gallery/manifest.json")
    patches = set()
    scores = set()
    for row in rows:
        path = row["source"]
        group = patches if re.fullmatch(r"patches/[a-z0-9-]+\.rpatch", path) else scores
        require(group is patches or re.fullmatch(r"compositions/[a-z0-9-]+\.rsn", path), "invalid gallery source path")
        require(path in trusted and trusted[path] == row["source_sha256"] and path not in group, "gallery source mismatch")
        group.add(path)
        folder, name = path.split("/")
        wanted = f"{folder}/{name.rsplit('.', 1)[0]}.wav"
        require(row["wave"] == wanted, "gallery output path mismatch")
        wave_file = root / "gallery" / wanted
        data = pcm24(wave_file, 48000, row["frames"])
        require(file_sha(wave_file) == row["wave_sha256"] and any(data), "gallery PCM identity mismatch")
    require(len(patches) >= 20 and len(scores) == 6, "gallery coverage incomplete")
    return {"patches": len(patches), "scores": len(scores)}


def verify(root, repo, expected, engine):
    verify_inventory(root)
    commit, trusted = verify_source(root, repo, expected)
    report = document(root / "ACCEPTANCE.json")
    require(report["source_commit"] == commit and report["profile"] == "diagnostic",
            "unsupported or source-mismatched evidence profile")
    require(report["probe_file"] == FIXTURE and report["sample_rate"] == 48000 and
            report["matrix"] == {"rates": list(RATES), "blocks": list(BLOCKS)}, "diagnostic profile mismatch")
    probe = root / "probe"
    session = (probe / "score.rsn").read_bytes()
    require(sha(session) == trusted[FIXTURE], "probe score differs from trusted input")
    frames, events, patch = reference_timeline(session, 48000)
    require((probe / "patch.rpatch").read_bytes() == patch, "probe patch differs from score source")
    program = (probe / "program.rbsc").read_bytes()
    structural_probe(program)
    receipt = dict(word.split("=", 1) for word in (probe / "render.txt").read_text().split())
    require(int(receipt["frames"]) == frames, "render receipt sample count differs from tempo map")
    require(int(receipt["instructions"]) == frames * struct.unpack_from("<I", program, 12)[0] and
            int(receipt["events"]) == len(events), "render receipt operation or event count differs")
    check_events(probe / "events.csv", frames, events)
    raw = pcm24(probe / "audio.wav", 48000, frames)
    require(any(raw), "probe PCM is silent")
    with tempfile.TemporaryDirectory(prefix="resonant-replay-") as directory:
        out = Path(directory)
        command([engine, "compile", probe / "patch.rpatch", out / "program.rbsc"])
        require((out / "program.rbsc").read_bytes() == program, "recompiled bytecode differs from evidence")
        command([engine, "render", probe / "score.rsn", out / "audio.wav", 48000, 128])
        require(pcm24(out / "audio.wav", 48000, frames) == raw, "bounded replay PCM differs from evidence")
    for rate in RATES:
        size, _, _ = reference_timeline(session, rate)
        original = pcm24(root / f"parity/{rate}-scalar.wav", rate, size)
        for block in BLOCKS:
            require(pcm24(root / f"parity/{rate}-{block}.wav", rate, size) == original, "SIMD/block parity mismatch")
    result = {"source_commit": commit, "probe_frames": frames, "probe_events": len(events),
              "spectral_worst_dbc": check_spectral(root)}
    if report["device_requested"]:
        result["device"] = check_device(root)
    if report["gallery_requested"]:
        result["gallery"] = check_gallery(root, trusted)
    recorded = [json.loads(line) for line in (root / "COMMANDS.jsonl").read_text().splitlines()]
    require({x["name"] for x in recorded} >= {"tests", "swap", "allocation", "spectral", "event_tape", "render"},
            "required command observations missing")
    require(all(x["exit"] == 0 for x in recorded if x["required"]), "required diagnostic command exited nonzero")
    require(report["status"] == "diagnostic", "diagnostic status does not match command results")
    return result


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("evidence", type=Path)
    cli.add_argument("--repo", type=Path, default=Path("."))
    cli.add_argument("--commit", default="HEAD")
    cli.add_argument("--engine", type=Path, required=True)
    args = cli.parse_args()
    try:
        result = verify(args.evidence.resolve(), args.repo.resolve(), args.commit, args.engine.resolve())
        print(json.dumps(result, sort_keys=True))
    except (Rejection, ValueError, KeyError, TypeError, IndexError, OSError, struct.error, wave.Error) as error:
        print(f"evidence rejected: {error}", file=sys.stderr)
        raise SystemExit(1)
