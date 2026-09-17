"""Shared test setup for the BMW Solar Logger host tests.

`app/` is a directory of scripts rather than an installed package, which is how
`tools/send.sh` and `tools/upload.sh` already reach it, so the tests put it on
`sys.path` the same way `app/device_tool.py` does.
"""

from __future__ import annotations

import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
APP_DIR = PROJECT_ROOT / "app"
FIRMWARE = PROJECT_ROOT / "Arduino" / "solar-logger" / "solar-logger.ino"

if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))
