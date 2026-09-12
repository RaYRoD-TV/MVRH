"""Assemble a complete DKR VR checkout from the pinned base and readable files."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

HERE = Path(__file__).resolve().parent


def checked_path(root, relative):
    parts = PurePosixPath(relative).parts
    if (not parts or PurePosixPath(relative).is_absolute() or
            "\\" in relative or ":" in relative or
            any(part in ("..", ".git") for part in parts)):
        raise ValueError(f"Unsafe source path: {relative}")
    root = root.resolve()
    path = root.joinpath(*parts)
    if not path.resolve().is_relative_to(root):
        raise ValueError(f"Source path leaves the checkout: {relative}")
    return path


def check_hash(data, expected, label):
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError(f"Invalid checksum for {label}")
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"Checksum mismatch for {label}")


def install_archive(archive, bundle, destination):
    check_hash(archive.read_bytes(), bundle["sha256"], archive.name)
    with tarfile.open(archive, "r:gz") as source:
        for entry in bundle["files"]:
            target = checked_path(destination, entry["path"])
            member = source.getmember(entry["member"])
            if not member.isfile():
                raise ValueError(f"Dependency is not a regular file: {entry['member']}")
            with source.extractfile(member) as stream:
                data = stream.read()
            check_hash(data, entry["sha256"], entry["member"])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)


def prepare(destination):
    destination = Path(destination).expanduser().resolve()
    if destination.exists():
        raise FileExistsError(f"Destination already exists: {destination}. Choose a new folder.")
    specification = json.loads((HERE / "source.json").read_text(encoding="utf-8"))
    commit = specification["commit"]
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("The upstream commit must be a full commit hash.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".dkr-source-", dir=destination.parent))
    complete = False
    try:
        print(f"Getting Golden Balloon {specification['tag']}...", flush=True)
        subprocess.run(["git", "clone", "--quiet", "--no-checkout",
                        specification["upstream"], str(stage)], check=True)
        subprocess.run(["git", "-C", str(stage), "-c", "advice.detachedHead=false",
                        "checkout", "--quiet", "--detach", commit], check=True)
        observed = subprocess.check_output(
            ["git", "-C", str(stage), "rev-parse", "HEAD"], text=True).strip()
        if observed != commit:
            raise ValueError("The checkout does not match the required upstream commit.")

        count = 0
        files = HERE / "files"
        for source in sorted(files.rglob("*")):
            if source.is_symlink():
                raise ValueError(f"Source links are not supported: {source}")
            if not source.is_file():
                continue
            target = checked_path(stage, source.relative_to(files).as_posix())
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            count += 1

        cache = stage / ".git" / "mvrh-dependencies"
        cache.mkdir()
        for number, bundle in enumerate(specification["dependencies"]):
            if not bundle["url"].startswith("https://"):
                raise ValueError("Dependency downloads require HTTPS.")
            print(f"Getting {bundle['name']}...", flush=True)
            archive = cache / f"{number}.tar.gz"
            with urllib.request.urlopen(bundle["url"], timeout=120) as response:
                with archive.open("wb") as output:
                    shutil.copyfileobj(response, output)
            install_archive(archive, bundle, stage)

        if destination.exists():
            raise FileExistsError(f"Destination was created during setup: {destination}")
        if (stage.resolve().parent != destination.parent.resolve() or
                not stage.name.startswith(".dkr-source-")):
            raise ValueError("The staging folder is outside the selected location.")
        stage.rename(destination)
        complete = True
        print(f"Ready: {destination}")
        print(f"Applied {count} readable source files. HEAD remains at upstream {commit}.")
        print("Use git diff in that folder to review the VR changes.")
    finally:
        if not complete:
            print(f"Setup stopped. Partial files were kept at {stage}.", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", help="a new folder for the complete source checkout")
    arguments = parser.parse_args()
    try:
        prepare(arguments.destination)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, tarfile.TarError) as error:
        print(f"Source preparation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
