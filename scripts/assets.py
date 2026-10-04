#!/usr/bin/env python3
"""Verify, install, and package Signal's external media."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parents[1]
PROVENANCE = "media-provenance.json"
MAX_PACK_BYTES = 2 * 1024 * 1024 * 1024


def safe_path(value):
    path = PurePosixPath(value)
    if (not value or "\\" in value or path.is_absolute()
            or any(part in ("", ".", "..") for part in value.split("/"))
            or ":" in value):
        raise ValueError(f"Invalid media path: {value}")
    return path


def inventory(manifest):
    result = {}
    for line in manifest.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        optional = line.startswith("?")
        name = line[1:] if optional else line
        safe_path(name)
        if name in result:
            raise ValueError(f"Duplicate media entry: {name}")
        result[name] = optional
    return result


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def verify(root, manifest):
    expected = inventory(manifest)
    metadata = json.loads((root / PROVENANCE).read_text())
    if metadata.get("schema") != 1 or not isinstance(metadata.get("files"), dict):
        raise ValueError("Media provenance requires schema 1 and a files object")
    files = metadata["files"]
    installed = []
    for name, optional in expected.items():
        target = root / name
        if optional and not target.exists():
            continue
        if not target.is_file() or target.is_symlink():
            raise ValueError(f"Required media file is missing: {name}")
        if root.resolve() not in target.resolve().parents:
            raise ValueError(f"Media path leaves its root: {name}")
        record = files.get(name, {})
        if not isinstance(record, dict) or not all(isinstance(record.get(key), str) and record[key].strip()
                   for key in ("sha256", "source", "license")):
            raise ValueError(f"Hash, source, and license are required: {name}")
        if record["sha256"] != digest(target):
            raise ValueError(f"Media hash mismatch: {name}")
        installed.append(name)
    return installed


def sync(root, manifest, source, sha256):
    if not source or len(sha256) != 64 or any(c not in "0123456789abcdef" for c in sha256):
        raise ValueError("Set SIGNAL_ASSET_PACK_URL and SIGNAL_ASSET_PACK_SHA256, or pass --pack and --sha256")
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="signal-media-") as directory:
        temporary = Path(directory)
        archive = temporary / "pack.zip"
        if source.startswith("https://"):
            with urllib.request.urlopen(source, timeout=30) as response, archive.open("wb") as output:
                total = 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > MAX_PACK_BYTES:
                        raise ValueError("Media pack exceeds the 2 GiB limit")
                    output.write(block)
        else:
            if "://" in source:
                raise ValueError("Use an HTTPS URL or a local ZIP path")
            shutil.copyfile(source, archive)
        if digest(archive) != sha256:
            raise ValueError("Media pack SHA-256 mismatch")
        stage = temporary / "verified"
        stage.mkdir()
        allowed = set(inventory(manifest)) | {PROVENANCE}
        seen = set()
        with zipfile.ZipFile(archive) as pack:
            total = 0
            for member in pack.infolist():
                if member.is_dir():
                    safe_path(member.filename.rstrip("/"))
                    continue
                safe_path(member.filename)
                mode = member.external_attr >> 16
                if (member.filename not in allowed or member.filename in seen
                        or stat.S_ISLNK(mode)):
                    raise ValueError(f"Unexpected media pack entry: {member.filename}")
                seen.add(member.filename)
                total += member.file_size
                if total > MAX_PACK_BYTES:
                    raise ValueError("Expanded media exceeds the 2 GiB limit")
                dest = stage / member.filename
                dest.parent.mkdir(parents=True, exist_ok=True)
                with pack.open(member) as src, dest.open("wb") as out:
                    shutil.copyfileobj(src, out)
        names = verify(stage, manifest)
        omitted = [name for name, optional in inventory(manifest).items()
                   if optional and name not in names]
        # Validate the complete pack and all destinations before installation.
        for name in names + [PROVENANCE] + omitted:
            dest = root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            if root.resolve() not in dest.resolve().parents or dest.is_symlink():
                raise ValueError(f"Invalid install destination: {name}")
        for name in names + [PROVENANCE]:
            dest = root / name
            with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as tmp:
                staged = Path(tmp.name)
            try:
                shutil.copyfile(stage / name, staged)
                os.replace(staged, dest)
            finally:
                staged.unlink(missing_ok=True)
        for name in omitted:
            (root / name).unlink(missing_ok=True)
    return verify(root, manifest)


def package(root, manifest, build, destination, runtime):
    names = verify(root, manifest) if runtime != "server" else []
    if destination.exists():
        raise ValueError(f"Choose a fresh package directory: {destination}")
    destination.mkdir(parents=True)
    if runtime == "web":
        binaries = ["signal.js", "signal.wasm", "play.html", "signal-touch-controls.js"]
        media_root = destination
    else:
        executable = "signal_server" if runtime == "server" else "signal"
        if (build / f"{executable}.exe").is_file():
            executable += ".exe"
        binaries = [executable]
        media_root = destination / "assets"
    for name in binaries:
        shutil.copy2(build / name, destination / name)
    if runtime != "server":
        for name in names + [PROVENANCE]:
            target = media_root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / name, target)
        verify(media_root, manifest)
    records = {str(item.relative_to(destination)).replace(os.sep, "/"): digest(item)
               for item in sorted(destination.rglob("*")) if item.is_file()}
    (destination / "SHA256SUMS").write_text(
        "".join(f"{value}  {name}\n" for name, value in records.items()))
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("sync", "verify", "package"))
    parser.add_argument("--root", type=Path, default=REPO / "assets")
    parser.add_argument("--manifest", type=Path, default=REPO / "assets/manifest.txt")
    parser.add_argument("--pack", default=os.getenv("SIGNAL_ASSET_PACK_URL", ""))
    parser.add_argument("--sha256", default=os.getenv("SIGNAL_ASSET_PACK_SHA256", ""))
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--dest", type=Path, default=REPO / "dist")
    parser.add_argument("--runtime", choices=("native", "web", "server"), default="native")
    args = parser.parse_args()
    try:
        if args.command == "sync":
            result = sync(args.root, args.manifest, args.pack, args.sha256)
        elif args.command == "verify":
            result = verify(args.root, args.manifest)
        else:
            result = package(args.root, args.manifest, args.build, args.dest, args.runtime)
        print(f"Media {args.command}: {len(result)} verified files")
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Media {args.command}: {error}\n")


if __name__ == "__main__":
    main()
