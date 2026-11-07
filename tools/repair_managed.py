"""Verify managed_components/ against the component manager's cache and restore damaged components.

On Windows the IDF component manager sometimes leaves an incomplete copy in managed_components/ (hundreds of files missing,
seen for espressif__tinyusb after switching between envs that use different lock files; the configure step then fails with
"Path ... does not exist or is a broken symbolic link"). The manager's cache keeps an intact copy. This checks every managed
component that has a CHECKSUMS.json against it, and replaces a damaged one with the cached copy of the same name and version.

    python tools/repair_managed.py            check, and repair what is damaged
    python tools/repair_managed.py --check    only report

Run it from the project root, with no shell or editor sitting inside managed_components/ (a folder in use cannot be replaced).
"""
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
MANAGED = PROJECT / "managed_components"


def cache_roots():
    local = os.environ.get("LOCALAPPDATA")
    roots = []
    if local:
        roots.append(Path(local) / "Espressif" / "ComponentManager" / "Cache")
    roots.append(Path.home() / ".cache" / "Espressif" / "ComponentManager" / "Cache")
    return [r for r in roots if r.is_dir()]


def damage(folder: Path):
    """Return the list of files the folder's CHECKSUMS.json lists but that are missing or different."""
    sums = folder / "CHECKSUMS.json"
    if not sums.is_file():
        return None
    bad = []
    for entry in json.loads(sums.read_text(encoding="utf-8"))["files"]:
        path = folder / entry["path"]
        if not path.is_file():
            bad.append(entry["path"])
        elif hashlib.sha256(path.read_bytes()).hexdigest() != entry["hash"]:
            bad.append(entry["path"])
    return bad


def version_of(folder: Path):
    for line in (folder / "idf_component.yml").read_text(encoding="utf-8").splitlines():
        if line.startswith("version:"):
            return line.split(":", 1)[1].strip().strip("'\"")
    return None


def find_cached(name: str, version: str):
    for root in cache_roots():
        for service in root.iterdir():
            if not service.is_dir():
                continue
            for candidate in service.glob(f"{name}_{version}_*"):
                if candidate.is_dir() and damage(candidate) == []:
                    return candidate
    return None


def main():
    check_only = "--check" in sys.argv
    if not MANAGED.is_dir():
        print("no managed_components/ here")
        return 0
    status = 0
    for folder in sorted(p for p in MANAGED.iterdir() if p.is_dir()):
        bad = damage(folder)
        if bad is None:
            print(f"  {folder.name}: no CHECKSUMS.json, skipped")
        elif not bad:
            print(f"  {folder.name}: ok")
        else:
            print(f"  {folder.name}: DAMAGED ({len(bad)} files missing or changed, e.g. {bad[0]})")
            status = 1
            if check_only:
                continue
            version = version_of(folder)
            cached = find_cached(folder.name, version) if version else None
            if not cached:
                print(f"    no intact cached copy of {folder.name} {version}: delete the folder and let the manager fetch it")
                continue
            shutil.rmtree(folder)
            shutil.copytree(cached, folder)
            print(f"    restored from {cached}: " + ("verified" if damage(folder) == [] else "STILL DAMAGED"))
            status = 0 if damage(folder) == [] else 1
    return status


if __name__ == "__main__":
    sys.exit(main())
