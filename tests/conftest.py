"""Shared test setup for the BMW Solar Logger host tests.

`app/` is a directory of scripts rather than an installed package, which is how
`tools/send.sh` and `tools/upload.sh` already reach it, so the tests put it on
`sys.path` the same way `app/device_tool.py` does. `tools/` goes on it too, for
`tools/intellisense.py`.
"""

from __future__ import annotations

import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
APP_DIR = PROJECT_ROOT / "app"
TOOLS_DIR = PROJECT_ROOT / "tools"

# Firmware source is read through tests/firmware_source.py, which reads every
# file in the sketch directory. Do not add a path to solar-logger.ino alone
# here: a test pinned to one file breaks when modularization moves code out.

for directory in (APP_DIR, TOOLS_DIR):
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))
