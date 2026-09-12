import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "prepare_source", Path(__file__).with_name("prepare-source.py"))
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class SourcePreparationTests(unittest.TestCase):
    def test_paths_stay_inside_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(setup.checked_path(root, "platform/vr/vr.h"),
                             root / "platform/vr/vr.h")
            for path in ("../outside", "/outside", "C:/outside", ".git/config", "a\\b"):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    setup.checked_path(root, path)

    def test_existing_destination_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "keep.txt").write_text("existing work")
            with self.assertRaises(FileExistsError):
                setup.prepare(root)
            self.assertEqual((root / "keep.txt").read_text(), "existing work")

    def test_archive_and_member_hashes_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "sdk.tar.gz"
            payload = b"checked header\n"
            with tarfile.open(archive, "w:gz") as tar:
                entry = tarfile.TarInfo("sdk/include/header.h")
                entry.size = len(payload)
                tar.addfile(entry, io.BytesIO(payload))
            bundle = {
                "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "files": [{"member": "sdk/include/header.h", "path": "include/header.h",
                           "sha256": hashlib.sha256(payload).hexdigest()}],
            }
            destination = root / "checkout"
            destination.mkdir()
            bad = json.loads(json.dumps(bundle))
            bad["sha256"] = "0" * 64
            with self.assertRaises(ValueError):
                setup.install_archive(archive, bad, destination)
            self.assertFalse((destination / "include/header.h").exists())
            bad = json.loads(json.dumps(bundle))
            bad["files"][0]["sha256"] = "0" * 64
            with self.assertRaises(ValueError):
                setup.install_archive(archive, bad, destination)
            self.assertFalse((destination / "include/header.h").exists())
            setup.install_archive(archive, bundle, destination)
            self.assertEqual((destination / "include/header.h").read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
