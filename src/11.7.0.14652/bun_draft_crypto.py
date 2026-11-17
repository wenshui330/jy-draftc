# SPDX-License-Identifier: MIT
"""One-shot Windows x64 draft crypto using Jianying's installed Bun runtime."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import subprocess
import tempfile


class BunDraftCryptoError(RuntimeError):
    pass


class BunDraftCryptoCodec:
    def __init__(self, jy_install_dir, timeout=45):
        if os.name != "nt":
            raise BunDraftCryptoError("Bundled Bun draft crypto requires Windows x64")
        if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
            raise BunDraftCryptoError("Codec timeout must be a positive finite number")
        self.install = Path(jy_install_dir).expanduser().resolve()
        self.runtime = self.install / "vebun.exe"
        self.bridge = Path(__file__).with_suffix(".js")
        self.timeout = timeout
        for required in (self.runtime, self.install / "videoeditor.dll", self.bridge):
            if not required.is_file():
                raise BunDraftCryptoError(f"Required bundled codec component not found: {required}")

    def decrypt(self, raw: bytes) -> bytes:
        """Return UTF-8 JSON bytes without reserializing the draft."""
        if not isinstance(raw, bytes) or not raw:
            raise BunDraftCryptoError("Encrypted draft input must be nonempty bytes")
        output = self._run("decode", raw)
        _check_json_object(output, "Decrypted draft")
        return output

    def encrypt(self, raw: bytes) -> bytes:
        """Encrypt UTF-8 JSON and verify byte-for-byte decryption before returning."""
        _check_json_object(raw, "Draft input")
        return self._run("encode", raw)

    def _run(self, operation: str, data: bytes) -> bytes:
        with tempfile.TemporaryDirectory(prefix="jy-draftc-bun-") as folder:
            work = Path(folder).resolve()
            source, output, report = (
                work / name for name in ("input.bin", "output.bin", "report.json")
            )
            spec = work / "spec.json"
            source.write_bytes(data)
            spec.write_text(
                json.dumps({
                    "operation": operation,
                    "install_dir": str(self.install),
                    "input": str(source),
                    "output": str(output),
                    "report": str(report),
                }),
                encoding="utf-8",
            )
            env = os.environ.copy()
            env.pop("PYTHONHOME", None)
            env.pop("PYTHONPATH", None)
            try:
                process = subprocess.run(
                    [str(self.runtime), str(self.bridge), str(spec)],
                    cwd=self.install,
                    capture_output=True,
                    timeout=self.timeout,
                    env=env,
                )
            except subprocess.TimeoutExpired as exc:
                raise BunDraftCryptoError(
                    f"Bundled Bun codec timed out after {self.timeout} seconds"
                ) from exc
            except OSError as exc:
                raise BunDraftCryptoError(f"Cannot start bundled Bun codec: {exc}") from exc
            try:
                result = json.loads(report.read_text(encoding="utf-8"))
                if not isinstance(result, dict):
                    raise ValueError("Codec report must be an object")
            except (OSError, ValueError) as exc:
                raise BunDraftCryptoError(
                    f"Bundled Bun codec produced no valid report; exit={process.returncode}; "
                    f'stderr={process.stderr.decode("utf-8", "replace")[-2000:]}'
                ) from exc
            if process.returncode != 0 or result.get("status") != "passed":
                raise BunDraftCryptoError(
                    f"Bundled Bun codec failed; exit={process.returncode}; "
                    f'error={result.get("error", "unspecified native failure")}'
                )
            if operation == "encode" and result.get("roundtrip_equal") is not True:
                raise BunDraftCryptoError(
                    "Bundled Bun codec did not verify an exact encryption round-trip"
                )
            try:
                raw = output.read_bytes()
            except OSError as exc:
                raise BunDraftCryptoError("Bundled Bun codec did not produce output") from exc
            if not raw or result.get("output_bytes") != len(raw):
                raise BunDraftCryptoError("Bundled Bun codec output length differs from its report")
            return raw


def _check_json_object(raw: bytes, label: str) -> None:
    if not isinstance(raw, bytes) or not raw:
        raise BunDraftCryptoError(f"{label} must be nonempty UTF-8 JSON bytes")
    try:
        document = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise BunDraftCryptoError(f"{label} is not valid UTF-8 JSON") from exc
    if not isinstance(document, dict):
        raise BunDraftCryptoError(f"{label} must be a JSON object")
