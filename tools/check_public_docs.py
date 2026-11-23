#!/usr/bin/env python3
"""Guard public project copy against personal attribution or paths."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
TRACKED_DOCS = [
    ROOT / "README.md",
    ROOT / "AGENTS.md",
    ROOT / "CONTRIBUTING.md",
    *sorted((ROOT / "docs").rglob("*.md")),
    *sorted((ROOT / ".agents").rglob("*.md")),
]
PATTERNS = (
    re.compile(r"/home/[a-z0-9_.-]+/", re.IGNORECASE),
    re.compile(r"C:\\\\Users\\\\[^\\\\]+\\\\", re.IGNORECASE),
    re.compile(r"\b(the requester|the owner) wants\b", re.IGNORECASE),
)
for path in TRACKED_DOCS:
    text = path.read_text(encoding="utf-8")
    for pattern in PATTERNS:
        if pattern.search(text):
            raise SystemExit(f"Attribution/path leak in {path.relative_to(ROOT)}: {pattern.pattern}")
print(f"Checked {len(TRACKED_DOCS)} public documentation files.")
