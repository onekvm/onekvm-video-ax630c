#!/usr/bin/env python3
"""Exercise the real activation script with isolated sysfs and device trees."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "packaging/activate-vdec"
ALIASES = ("onekvm-vin", "onekvm-vproc", "onekvm-vdec", "onekvm-vdec-media")


class DeviceAliasTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="onekvm-device-alias-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dev = self.root / "dev"
        self.platform = self.root / "sys/bus/platform/devices"
        self.dev.mkdir()
        self.platform.mkdir(parents=True)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        # Character-device checks work without root. Never chmod the real node.
        for name in ("video0", "video1", "video2", "video7", "media0", "media3"):
            (self.dev / name).symlink_to("/dev/null")
        for name in ALIASES:
            (self.dev / name).symlink_to(self.dev / "video7")
        for name, body in (("modprobe", 'exit "${MOCK_MODPROBE_EXIT:-0}"'),
                           ("chmod", "exit 0")):
            wrapper = self.bin / name
            wrapper.write_text("#!/bin/sh\n" + body + "\n")
            wrapper.chmod(0o755)
        self.script = self.root / "activate-vdec"
        self.script.write_text(SCRIPT.read_text().replace("/sys/", str(self.root / "sys") + "/")
                               .replace("/dev/", str(self.dev) + "/"))

    def topology(self, vin="video2", tdp="video0", vdec="video1", media="media0"):
        nodes = (("vin", "video4linux", vin, "open_vin_capture"),
                 ("4405000.image-processor", "video4linux", tdp, "ax630c-tdp"),
                 ("4020000.vdec", "video4linux", vdec, "axera,ax630c-vdec-dec"),
                 ("4020000.vdec", "", media, None))
        for device, group, node, name in nodes:
            if node is None:
                continue
            path = self.platform / device / group / node
            path.mkdir(parents=True)
            if name:
                (path / "name").write_text(name + "\n")

    def run_script(self, **env):
        return subprocess.run(["/bin/sh", str(self.script)], capture_output=True, text=True,
                              env={**os.environ, "PATH": str(self.bin) + ":" + os.environ["PATH"], **env},
                              timeout=5)

    def assert_absent(self, name):
        self.assertFalse(os.path.lexists(self.dev / name), name)

    def test_refresh_after_reordered_enumeration(self):
        for nodes in (("video2", "video0", "video1", "media0"),
                      ("video0", "video1", "video2", "media3")):
            with self.subTest(nodes=nodes):
                self.topology(*nodes)
                result = self.run_script()
                self.assertEqual(result.returncode, 0, result.stderr)
                for alias, target in zip(ALIASES, nodes):
                    self.assertEqual(os.readlink(self.dev / alias), str(self.dev / target))
                for child in self.platform.iterdir():
                    shutil.rmtree(child)

    def test_missing_vin_rejects_stale_character_device(self):
        self.topology(vin=None)
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assert_absent("onekvm-vin")

    def test_missing_tdp_rejects_stale_character_device(self):
        self.topology(tdp=None)
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assert_absent("onekvm-vproc")

    def test_missing_decoder_node_rejects_stale_character_device(self):
        self.topology(vdec=None)
        (self.platform / "4020000.vdec/video4linux").mkdir()
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assert_absent("onekvm-vdec")

    def test_missing_media_node_rejects_stale_character_device(self):
        self.topology(media=None)
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assert_absent("onekvm-vdec-media")

    def test_absent_optional_decoder_clears_old_aliases(self):
        self.topology(vdec=None, media=None)
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_absent("onekvm-vdec")
        self.assert_absent("onekvm-vdec-media")

    def test_module_failure_cannot_leave_stale_aliases(self):
        self.topology()
        result = self.run_script(MOCK_MODPROBE_EXIT="1")
        self.assertNotEqual(result.returncode, 0)
        for alias in ALIASES:
            self.assert_absent(alias)

    def test_missing_character_node_clears_alias(self):
        self.topology()
        (self.dev / "video2").unlink()
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assert_absent("onekvm-vin")


if __name__ == "__main__":
    unittest.main()
