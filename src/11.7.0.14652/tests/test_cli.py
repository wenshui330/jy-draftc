# SPDX-License-Identifier: MIT
"""Run the real CLI's help and argument boundaries without loading a native DLL."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CLI = Path(__file__).resolve().parents[1] / "jy_draftc.py"


class CliArgumentsTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.work = Path(self.folder.name)
        self.source = self.work / "draft_content.json"
        self.source.write_bytes(b'{"counter":9007199254740993}')
        self.env = os.environ.copy()
        self.env.pop("JY_INSTALL_DIR", None)

    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, str(CLI), *map(str, args)],
            capture_output=True, text=True, env=self.env, timeout=10,
        )

    def assert_rejected(self, result, message):
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(message, result.stderr)
        self.assertEqual(self.source.read_bytes(), b'{"counter":9007199254740993}')

    def test_help_needs_no_windows_installation(self):
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("-dc", result.stdout)
        self.assertIn("MP4 export are not supported", result.stdout)

    def test_requires_explicit_installation(self):
        result = self.run_cli("-dc", self.source, "-o", self.work / "plain.json")
        self.assert_rejected(result, "--jy-dir or JY_INSTALL_DIR is required")

    def test_requires_explicit_output(self):
        result = self.run_cli("-ec", self.source, "--jy-dir", self.work)
        self.assert_rejected(result, "requires an explicit -o")

    def test_refuses_source_overwrite_even_with_force(self):
        result = self.run_cli(
            "-ec", self.source, "-o", self.source, "-f", "--jy-dir", self.work,
        )
        self.assert_rejected(result, "output must differ from the input")

    def test_refuses_source_hardlink_overwrite(self):
        alias = self.work / "same-draft.json"
        try:
            os.link(self.source, alias)
        except OSError as exc:
            self.skipTest(f"hard links unavailable: {exc}")
        result = self.run_cli(
            "-ec", self.source, "-o", alias, "-f", "--jy-dir", self.work,
        )
        self.assert_rejected(result, "output must differ from the input")

    def test_refuses_existing_output_without_force(self):
        output = self.work / "plain.json"
        output.write_bytes(b"keep this output")
        result = self.run_cli("-dc", self.source, "-o", output, "--jy-dir", self.work)
        self.assert_rejected(result, "output already exists")
        self.assertEqual(output.read_bytes(), b"keep this output")

    def test_refuses_directory_input(self):
        result = self.run_cli("-dc", self.work, "-o", self.work / "plain.json", "--jy-dir", self.work)
        self.assert_rejected(result, "directory processing is not supported")

    def test_check_does_not_accept_output(self):
        result = self.run_cli("-c", "-o", self.work / "plain.json", "--jy-dir", self.work)
        self.assert_rejected(result, "does not write files")
        self.assertFalse((self.work / "plain.json").exists())

    def test_rejects_nonfinite_or_nonpositive_timeout(self):
        for timeout in ("nan", "inf", "0", "-1"):
            with self.subTest(timeout=timeout):
                result = self.run_cli("-c", "--jy-dir", self.work, "--timeout", timeout)
                self.assert_rejected(result, "timeout must be a positive finite number")


if __name__ == "__main__":
    unittest.main()
