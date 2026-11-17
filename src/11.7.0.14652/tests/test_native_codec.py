# SPDX-License-Identifier: MIT
"""Optional integration tests against the selected, locally installed engine."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE))

from bun_draft_crypto import BunDraftCryptoCodec, BunDraftCryptoError


@unittest.skipUnless(
    os.name == "nt" and os.environ.get("JY_INSTALL_DIR"),
    "Requires Windows x64 and JY_INSTALL_DIR pointing to Jianying 11.7.0.14652",
)
class NativeCodecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.codec = BunDraftCryptoCodec(os.environ["JY_INSTALL_DIR"])
        cls.plain = (
            '{\r\n  "title": "剪映 🧪", "counter": 9007199254740993,\r\n'
            '  "tracks": [], "unknown_field": {"keep": true}\r\n}\r\n'
        ).encode("utf-8")

    def run_cli(self, *arguments):
        return subprocess.run(
            [sys.executable, str(SOURCE / "jy_draftc.py"), *map(str, arguments)],
            capture_output=True,
            timeout=60,
        )

    def test_roundtrip_preserves_json_bytes(self):
        encrypted = self.codec.encrypt(self.plain)
        self.assertNotEqual(encrypted, self.plain)
        self.assertEqual(self.codec.decrypt(encrypted), self.plain)

    def test_corrupt_ciphertext_is_rejected(self):
        with self.assertRaises(BunDraftCryptoError):
            self.codec.decrypt(b"this is not an encrypted draft")

    def test_cli_file_conversion_keeps_input_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "source.json"
            encrypted = work / "encrypted.json"
            decoded = work / "decrypted.json"
            source.write_bytes(self.plain)
            source_mtime = source.stat().st_mtime_ns
            encode = self.run_cli("-ec", source, "-o", encrypted)
            self.assertEqual(encode.returncode, 0, encode.stdout + encode.stderr)
            self.assertTrue(json.loads(encode.stdout)["roundtrip_equal"])
            encrypted_before = encrypted.read_bytes()
            encrypted_mtime = encrypted.stat().st_mtime_ns
            decode = self.run_cli("-dc", encrypted, "-o", decoded)
            self.assertEqual(decode.returncode, 0, decode.stdout + decode.stderr)
            self.assertEqual(decoded.read_bytes(), self.plain)
            self.assertEqual(source.read_bytes(), self.plain)
            self.assertEqual(source.stat().st_mtime_ns, source_mtime)
            self.assertEqual(encrypted.read_bytes(), encrypted_before)
            self.assertEqual(encrypted.stat().st_mtime_ns, encrypted_mtime)

    def test_cli_check_is_read_only(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sample.json"
            original = self.codec.encrypt(self.plain)
            source.write_bytes(original)
            source_mtime = source.stat().st_mtime_ns
            checked = self.run_cli("-c", source)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            report = json.loads(checked.stdout)
            self.assertTrue(report["synthetic_roundtrip_equal"])
            self.assertTrue(report["sample_roundtrip_equal"])
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(source.stat().st_mtime_ns, source_mtime)
            self.assertEqual(list(source.parent.iterdir()), [source])


if __name__ == "__main__":
    unittest.main()
