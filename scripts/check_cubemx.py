#!/usr/bin/env python3
"""Fail if hand edits escape CubeMX USER CODE sections (line endings ignored)."""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
BLOCK = re.compile(r"(/\* USER CODE BEGIN ([^*]+?) \*/).*?(/\* USER CODE END \2 \*/)", re.S)


def skeleton(text):
    return BLOCK.sub(lambda match: match[1] + match[3], text)


def main():
    manifest = json.loads((ROOT / "tests/cubemx_generated.sha256.json").read_text())
    failures = []
    for name, expected in manifest.items():
        path = ROOT / name
        if not path.exists():
            failures.append(name + ": missing generated file")
            continue
        text = path.read_text()
        blocks = list(BLOCK.finditer(text))
        if len(blocks) != text.count("/* USER CODE BEGIN") or len(blocks) != text.count("/* USER CODE END"):
            failures.append(name + ": unmatched USER CODE markers")
        actual = hashlib.sha256(skeleton(text).encode()).hexdigest()
        if actual != expected:
            failures.append(name + ": changes outside USER CODE sections")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"CubeMX boundaries: {len(manifest)} files unchanged outside USER CODE sections")
    return 0


if __name__ == "__main__":
    sys.exit(main())
