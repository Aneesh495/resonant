#!/usr/bin/env python3

from pathlib import Path
import re
import shutil
import subprocess
from xml.etree import ElementTree


def main():
    prose = [Path("README.md"), *Path("docs").rglob("*.md")]
    for path in prose:
        text = path.read_text()
        if "\u2014" in text:
            raise ValueError(f"em dash in {path}")
        for destination in re.findall(r"\]\(([^\s)]+)\)", text):
            if "://" in destination or destination.startswith("#"):
                continue
            name = destination.split("#", 1)[0]
            if not (path.parent / name).exists():
                raise ValueError(f"broken local link in {path}: {destination}")
    diagrams = sorted(Path("docs/diagrams").glob("*.dot"))
    if len(diagrams) != 12:
        raise ValueError("missing diagram source")
    for source in diagrams:
        svg = ElementTree.parse(source.with_suffix(".svg")).getroot()
        namespace = "{http://www.w3.org/2000/svg}"
        if svg.tag != namespace + "svg" or len(list(svg.iter(namespace + "path"))) < 3:
            raise ValueError(f"empty or invalid graph render: {source}")
        if shutil.which("dot"):
            rendered = subprocess.check_output(["dot", "-Tsvg", source], timeout=20)
            generated = ElementTree.fromstring(rendered)
            if len(list(svg.iter(namespace + "path"))) != len(list(generated.iter(namespace + "path"))):
                raise ValueError(f"checked-in render has different path count: {source}")
    print(f"{len(prose)} prose files and {len(diagrams)} rendered diagrams checked")


if __name__ == "__main__":
    main()
