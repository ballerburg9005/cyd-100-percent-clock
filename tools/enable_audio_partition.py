#!/usr/bin/env python3
"""One-time migration of an existing local sdkconfig to 3 MiB app layout.

Preserves all unrelated device-local Kconfig options. The NVS, phy_init and
factory app offsets are unchanged, so Wi-Fi credentials are not erased.
For fresh builds sdkconfig.defaults already selects the same partition CSV.
"""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
cfg = root / "sdkconfig"
if not cfg.exists():
    print("No existing sdkconfig; sdkconfig.defaults already selects 3 MiB")
    raise SystemExit(0)

original = cfg.read_text(encoding="utf-8")
lines = original.splitlines()
keys = (
    "CONFIG_PARTITION_TABLE_SINGLE_APP",
    "CONFIG_PARTITION_TABLE_TWO_OTA",
    "CONFIG_PARTITION_TABLE_CUSTOM",
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME",
    "CONFIG_PARTITION_TABLE_FILENAME",
)
updated = []
for line in lines:
    if line.startswith("CONFIG_"):
        if any(line.startswith(key + "=") for key in keys):
            continue
    if line.startswith("# CONFIG_"):
        if any(line.startswith("# " + key + " is not set") for key in keys):
            continue
    updated.append(line)
updated += [
    "# CONFIG_PARTITION_TABLE_SINGLE_APP is not set",
    "# CONFIG_PARTITION_TABLE_TWO_OTA is not set",
    "CONFIG_PARTITION_TABLE_CUSTOM=y",
    'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"',
    'CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"',
]
result = "\n".join(updated) + "\n"
if result == original:
    print("Already configured for 3 MiB app")
else:
    backup = root / "sdkconfig.before-audio"
    if not backup.exists():
        backup.write_text(original, encoding="utf-8")
    cfg.write_text(result, encoding="utf-8")
    print("Updated local sdkconfig to 3 MiB factory app; backup: sdkconfig.before-audio")
    print("NVS/app offsets unchanged. Run: idf.py reconfigure build flash")
