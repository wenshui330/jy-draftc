# SPDX-License-Identifier: MIT
"""Single-file draft crypto CLI for Jianying 11.7.0.14652."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import sys

from bun_draft_crypto import BunDraftCryptoCodec, BunDraftCryptoError


def main(argv=None) -> int:
    parser = argument_parser()
    args = parser.parse_args(argv)
    source, output = validate_arguments(parser, args)
    try:
        codec = BunDraftCryptoCodec(args.jy_dir, timeout=args.timeout)
        if args.check is not None:
            report = check_codec(codec, source)
        else:
            report = convert_file(codec, source, output, args)
    except (BunDraftCryptoError, OSError) as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, ensure_ascii=True))
        return 1
    print(json.dumps(report, ensure_ascii=True))
    return 0


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Jianying 11.7.0.14652 single-file draft encryption/decryption.",
        epilog="Uses the selected installation's vebun.exe and videoeditor.dll. "
        "Directory processing, version conversion and MP4 export are not supported.",
    )
    commands = parser.add_mutually_exclusive_group(required=True)
    commands.add_argument("-dc", "--decrypt", metavar="FILE", help="decrypt one content file")
    commands.add_argument("-ec", "--encrypt", metavar="FILE", help="encrypt one JSON content file")
    commands.add_argument(
        "-c", "--check", nargs="?", const="", metavar="FILE",
        help="run a real synthetic round-trip; optionally also check an encrypted file",
    )
    parser.add_argument(
        "--jy-dir", default=os.environ.get("JY_INSTALL_DIR"),
        help="installation directory containing vebun.exe and videoeditor.dll "
        "(default: JY_INSTALL_DIR environment variable)",
    )
    parser.add_argument("--timeout", type=positive_timeout, default=45.0, metavar="SECONDS")
    parser.add_argument("-o", "--out", metavar="FILE", help="required output file for -dc/-ec")
    parser.add_argument("-f", "--force", action="store_true", help="overwrite an existing output file")
    return parser


def positive_timeout(value: str) -> float:
    try:
        timeout = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("timeout must be a positive finite number") from exc
    if not math.isfinite(timeout) or timeout <= 0:
        raise argparse.ArgumentTypeError("timeout must be a positive finite number")
    return timeout


def validate_arguments(parser, args):
    if not args.jy_dir:
        parser.error("--jy-dir or JY_INSTALL_DIR is required")
    if args.check is not None:
        if args.out is not None or args.force:
            parser.error("-c does not write files; -o and -f are not applicable")
        source_name = args.check
    else:
        if args.out is None:
            parser.error("-dc/-ec requires an explicit -o output file")
        source_name = args.decrypt or args.encrypt

    source = Path(source_name).expanduser().resolve() if source_name else None
    if source is not None and not source.is_file():
        parser.error("input must be one existing file; directory processing is not supported")
    if args.out is None:
        return source, None

    output = Path(args.out).expanduser().resolve()
    if output == source or (output.exists() and output.samefile(source)):
        parser.error("output must differ from the input, including when -f is used")
    if output.exists() and (not output.is_file() or not args.force):
        parser.error("output already exists; use -f only to overwrite a different file")
    if not output.parent.is_dir():
        parser.error("output parent directory does not exist")
    return source, output


def check_codec(codec, source):
    plain = json.dumps(
        {
            "title": "剪映 11.7 🧪",
            "counter": 9007199254740993,
            "unknown_field": {"keep": True},
        },
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    encrypted = codec.encrypt(plain)
    require_equal(codec.decrypt(encrypted), plain)
    report = {
        "status": "passed", "operation": "check",
        "synthetic_roundtrip_equal": True, "encrypted_bytes": len(encrypted),
    }
    if source is not None:
        original = source.read_bytes()
        decoded = codec.decrypt(original)
        require_equal(codec.decrypt(codec.encrypt(decoded)), decoded)
        report.update({
            "sample": str(source), "sample_bytes": len(original),
            "sample_roundtrip_equal": True,
        })
    return report


def require_equal(actual, expected):
    if actual != expected:
        raise BunDraftCryptoError("encrypt/decrypt round-trip changed draft bytes")


def convert_file(codec, source, output, args):
    raw = source.read_bytes()
    operation = "decrypt" if args.decrypt else "encrypt"
    converted = codec.decrypt(raw) if args.decrypt else codec.encrypt(raw)
    with output.open("wb" if args.force else "xb") as stream:
        stream.write(converted)
    return {
        "status": "passed", "operation": operation,
        "input": str(source), "output": str(output),
        "input_bytes": len(raw), "output_bytes": len(converted),
        **({"roundtrip_equal": True} if args.encrypt else {}),
    }


if __name__ == "__main__":
    sys.exit(main())
