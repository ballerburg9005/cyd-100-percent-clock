#!/usr/bin/env python3
"""Enable the experimental composite test in an existing ESP-IDF sdkconfig.

Preserves Wi-Fi credentials, NVS, ignored face assets, touch calibration,
and all unrelated project settings. Backs up sdkconfig once.
This is NOT required by clean checkout: sdkconfig.defaults enables the test.
"""
from pathlib import Path
import shutil

cfg = Path(__file__).resolve().parents[1] / "sdkconfig"
if not cfg.exists():
    print("No existing sdkconfig; sdkconfig.defaults configures the experiment.")
    raise SystemExit(0)
backup = cfg.with_name("sdkconfig.before-composite")
if not backup.exists():
    shutil.copy2(cfg, backup)
updates = {
    "CONFIG_CLOCK_COMPOSITE_TF_TEST": "y",
    "CONFIG_VIDEO_ENABLE_LVGL_SUPPORT": "n",
    "CONFIG_COMPILER_OPTIMIZATION_SIZE": "n",
    "CONFIG_COMPILER_OPTIMIZATION_PERF": "y",
}
lines = cfg.read_text().splitlines()
for key, value in updates.items():
    replacement = f"{key}=y" if value == "y" else f"# {key} is not set"
    found = False
    for i, line in enumerate(lines):
        if line.startswith(key + "=") or line == f"# {key} is not set":
            lines[i] = replacement
            found = True
            break
    if not found:
        lines.append(replacement)
cfg.write_text("\n".join(lines) + "\n")
print(f"Composite TF preview and -O2 enabled; backup: {backup.name}")
