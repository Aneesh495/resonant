#!/usr/bin/env python3
"""Render the checked-in instrument library and identify exact inputs and outputs."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import wave


def digest(path):
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def invoke(executable, *args):
    result = subprocess.run([str(executable), *map(str, args)], check=True, text=True,
                            stdout=subprocess.PIPE, timeout=90)
    return result.stdout.strip()


def inspect_wave(path):
    with wave.open(str(path), "rb") as audio:
        width = audio.getsampwidth()
        count = audio.getnframes()
        if width != 3 or audio.getnchannels() != 2 or audio.getframerate() != 48000:
            raise ValueError(f"unexpected gallery WAVE format in {path}")
        peak = 0
        observed = 0
        while data := audio.readframes(4096):
            peak = max(peak, *(abs(int.from_bytes(data[i:i + 3], "little", signed=True))
                               for i in range(0, len(data), 3)))
            observed += len(data) // 6
        if count == 0 or count != observed or peak < 1000:
            raise ValueError(f"empty, truncated or silent gallery audio in {path}")
        return count, peak


def build(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for folder, extension in [("patches", "*.rpatch"), ("compositions", "*.rsn")]:
        target = output / folder
        target.mkdir(exist_ok=True)
        for source in sorted(Path(folder).glob(extension)):
            output_wave = target / (source.stem + ".wav")
            row = {"source": str(source), "source_sha256": digest(source)}
            if folder == "patches":
                bytecode = target / (source.stem + ".rbsc")
                invoke(executable, "compile", source, bytecode)
                invoke(executable, "verify", bytecode)
                row["bytecode_sha256"] = digest(bytecode)
                row["render"] = invoke(executable, "render-patch", source, output_wave, 72000)
            else:
                row["render"] = invoke(executable, "render", source, output_wave)
            row["frames"], row["peak_pcm24"] = inspect_wave(output_wave)
            row["wave"] = str(output_wave.relative_to(output))
            row["wave_sha256"] = digest(output_wave)
            manifest.append(row)
    if (sum(x["source"].startswith("patches/") for x in manifest) < 20 or
            sum(x["source"].startswith("compositions/") for x in manifest) != 6):
        raise ValueError("gallery is missing required patch or score sources")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{len(manifest)} rendered and independently checked WAVE files in {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    options = parser.parse_args()
    build(options.executable.resolve(), options.output)
