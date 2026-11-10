#!/usr/bin/env python3
"""Measure coherent-tone residual energy against a direct Fourier reference."""

import argparse
import csv
import math
from pathlib import Path
import struct
import subprocess


N = 4096
RATES = (44100, 48000, 96000)
BINS = (19, 233, 773)
CASES = ("sine", "saw", "pulse", "triangle", "oversaturate", "oversaturate_hq")
THRESHOLD_DBC = -60


def samples(path, rate):
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("missing diagnostic WAVE container")
    offset = 12
    fmt = None
    payload = None
    while offset + 8 <= len(raw):
        kind, size = struct.unpack_from("<4sI", raw, offset)
        offset += 8
        if size > len(raw) - offset:
            raise ValueError("diagnostic WAVE chunk truncated")
        if kind == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", raw, offset)
        if kind == b"data":
            payload = raw[offset:offset + size]
        offset += size + size % 2
    if fmt != (3, 2, rate, rate * 8, 8, 32) or payload is None or len(payload) != 2 * N * 8:
        raise ValueError("unexpected diagnostic WAVE format")
    result = [left for left, _ in struct.iter_unpack("<ff", payload[N * 8:])]
    if len(result) != N or not all(math.isfinite(x) for x in result):
        raise ValueError("invalid diagnostic samples")
    return result


def power(signal, k):
    a = 2 * math.pi * k / N
    real = math.fsum(x * math.cos(a*i) for i, x in enumerate(signal))
    imaginary = math.fsum(x * math.sin(a*i) for i, x in enumerate(signal))
    return (real * real + imaginary * imaginary) * (1 if k in (0, N // 2) else 2) / N


def measure(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for rate in RATES:
        for k in BINS:
            hertz = rate * k / N
            for case in CASES:
                name = f"{case}-{rate}-{k}"
                patch = output / (name + ".rpatch")
                wave = output / (name + ".wav")
                nonlinear = case.startswith("oversaturate")
                oscillator = "sine" if nonlinear else case
                args = f"{hertz:.12f},.35" if oscillator == "pulse" else f"{hertz:.12f}"
                source = f"tone={oscillator}({args});"
                source += (f"hot=mul(tone,.8); clipped={case}(hot,4); y=mul(clipped,.5);"
                           if nonlinear else "y=mul(tone,.5);")
                patch.write_text(source + "out(y);\n")
                subprocess.run([executable, "render-patch", patch, wave, str(2*N), str(rate), "128", "float32"],
                               check=True, capture_output=True, timeout=30)
                signal = samples(wave, rate)
                dc = power(signal, 0)
                harmonic = math.fsum(power(signal, b) for b in range(k, N // 2 + 1, k))
                total = math.fsum(x*x for x in signal)
                residual = max(total - dc - harmonic, 1e-18)
                db = 10 * math.log10(residual / harmonic)
                rows.append({"case": case, "rate": rate, "bin": k, "frequency_hz": hertz,
                             "harmonic_energy": harmonic, "residual_energy": residual,
                             "residual_dbc": db, "threshold_dbc": THRESHOLD_DBC, "passed": db <= THRESHOLD_DBC})
    with (output / "spectral.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    for case in CASES:
        group = [r for r in rows if r["case"] == case]
        print(f"{case}: {sum(r['passed'] for r in group)}/{len(group)} pass; "
              f"worst {max(r['residual_dbc'] for r in group):.2f} dBc")
    return all(r["passed"] for r in rows if r["case"] != "oversaturate")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    raise SystemExit(0 if measure(args.executable.resolve(), args.output) else 1)
