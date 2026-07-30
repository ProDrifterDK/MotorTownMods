#!/usr/bin/env python3
"""Fail-closed static compatibility verifier for exact Motor Town binaries."""

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import stat
import string
import sys


MAX_MANIFEST_SIZE = 1024 * 1024
HASH_CHUNK_SIZE = 1024 * 1024


class VerificationInputError(ValueError):
    pass


def parse_pattern(pattern):
    if not isinstance(pattern, str):
        raise VerificationInputError("signature pattern must be a string")
    tokens = pattern.split()
    if not tokens:
        raise VerificationInputError("signature pattern must not be empty")

    parsed = []
    for token in tokens:
        if token == "??":
            parsed.append(None)
        elif len(token) == 2 and all(char in string.hexdigits for char in token):
            parsed.append(int(token, 16))
        else:
            raise VerificationInputError(
                f"invalid pattern token {token!r}; expected two hex digits or ??"
            )
    if all(token is None for token in parsed):
        raise VerificationInputError("signature pattern must contain a literal byte")
    return parsed


def find_matches(data, pattern):
    """Find overlapping tokenized matches without constructing a regular expression."""
    longest_start = 0
    longest = b""
    run_start = 0
    run = bytearray()
    for index, token in enumerate(pattern + [None]):
        if token is not None:
            if not run:
                run_start = index
            run.append(token)
        else:
            if len(run) > len(longest):
                longest_start = run_start
                longest = bytes(run)
            run.clear()

    matches = []
    search_from = 0
    while True:
        anchor_at = data.find(longest, search_from)
        if anchor_at < 0:
            break
        candidate = anchor_at - longest_start
        if candidate >= 0 and candidate + len(pattern) <= len(data):
            if all(token is None or data[candidate + index] == token
                   for index, token in enumerate(pattern)):
                matches.append(candidate)
        search_from = anchor_at + 1
    return matches


def parse_offset(value):
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError as error:
            raise VerificationInputError(f"invalid offset {value!r}") from error
    raise VerificationInputError(f"invalid offset {value!r}")


def open_regular_file(path, label):
    try:
        path_stat = path.stat()
    except OSError as error:
        raise VerificationInputError(f"unable to stat {label}: {error}") from error
    if not stat.S_ISREG(path_stat.st_mode):
        raise VerificationInputError(f"{label} must be a regular file")

    flags = os.O_RDONLY
    flags |= getattr(os, "O_BINARY", 0)
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NONBLOCK", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise VerificationInputError(f"unable to open {label}: {error}") from error

    try:
        opened_stat = os.fstat(descriptor)
        if not stat.S_ISREG(opened_stat.st_mode):
            raise VerificationInputError(f"{label} must be a regular file")
        return os.fdopen(descriptor, "rb"), opened_stat
    except Exception:
        os.close(descriptor)
        raise


def load_manifest(path):
    manifest_file, manifest_stat = open_regular_file(path, "manifest")
    if manifest_stat.st_size > MAX_MANIFEST_SIZE:
        manifest_file.close()
        raise VerificationInputError(
            f"manifest exceeds {MAX_MANIFEST_SIZE}-byte limit"
        )

    try:
        with manifest_file:
            raw_manifest = manifest_file.read(MAX_MANIFEST_SIZE + 1)
        if len(raw_manifest) > MAX_MANIFEST_SIZE:
            raise VerificationInputError(
                f"manifest exceeds {MAX_MANIFEST_SIZE}-byte limit"
            )
        manifest = json.loads(raw_manifest.decode("utf-8"))
    except VerificationInputError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise VerificationInputError(f"unable to read manifest: {error}") from error

    try:
        binary = manifest["binary"]
        expected_size = binary["size"]
        expected_sha = binary["sha256"].lower()
        signatures = manifest["signatures"]
    except (KeyError, TypeError, AttributeError) as error:
        raise VerificationInputError(f"invalid manifest structure: {error}") from error

    if not isinstance(expected_size, int) or isinstance(expected_size, bool) or expected_size < 0:
        raise VerificationInputError("binary.size must be a non-negative integer")
    if (len(expected_sha) != 64 or
            any(char not in string.hexdigits for char in expected_sha)):
        raise VerificationInputError("binary.sha256 must be a 64-digit hexadecimal string")
    if not isinstance(signatures, list) or not signatures:
        raise VerificationInputError("signatures must be a non-empty list")

    return manifest, expected_size, expected_sha, signatures


def hash_stream(stream):
    digest = hashlib.sha256()
    while chunk := stream.read(HASH_CHUNK_SIZE):
        digest.update(chunk)
    return digest.hexdigest()


def validate_signatures(signatures):
    validated = []
    for entry in signatures:
        try:
            name = entry["name"]
            expected_count = entry["expected_count"]
            raw_offsets = entry["expected_offsets"]
            raw_pattern = entry["pattern"]
        except (KeyError, TypeError) as error:
            raise VerificationInputError(f"invalid signature entry: {error}") from error

        if not isinstance(name, str) or not name:
            raise VerificationInputError("signature name must be a non-empty string")
        if (not isinstance(expected_count, int) or
                isinstance(expected_count, bool) or expected_count <= 0):
            raise VerificationInputError(
                f"{name}: expected_count must be strictly positive"
            )
        try:
            expected_offsets = [parse_offset(value) for value in raw_offsets]
        except TypeError as error:
            raise VerificationInputError(
                f"{name}: expected_offsets must be an array"
            ) from error
        if len(expected_offsets) != expected_count:
            raise VerificationInputError(
                f"{name}: expected_offsets length must equal expected_count"
            )

        validated.append({
            "name": name,
            "expected_count": expected_count,
            "expected_offsets": expected_offsets,
            "pattern": parse_pattern(raw_pattern),
        })
    return validated


def verify(manifest_path, binary_path):
    _, expected_size, expected_sha, signatures = load_manifest(manifest_path)
    signatures = validate_signatures(signatures)
    binary_file, binary_stat = open_regular_file(binary_path, "binary")
    actual_size = binary_stat.st_size
    size_matches = actual_size == expected_size
    report = {
        "verified": False,
        "binary": {
            "expected_size": expected_size,
            "actual_size": actual_size,
            "size_matches": size_matches,
            "expected_sha256": expected_sha,
            "actual_sha256": None,
            "sha256_matches": False,
        },
        "signatures": [],
    }

    # Reject size mismatches before hashing or mapping an unrelated binary.
    if not size_matches:
        binary_file.close()
        return report

    try:
        actual_sha = hash_stream(binary_file)
        report["binary"]["actual_sha256"] = actual_sha
        report["binary"]["sha256_matches"] = actual_sha == expected_sha
        if actual_sha != expected_sha:
            return report

        mapped_binary = (
            mmap.mmap(binary_file.fileno(), 0, access=mmap.ACCESS_READ)
            if actual_size else None
        )
        data = mapped_binary if mapped_binary is not None else b""
        try:
            all_verified = True
            for entry in signatures:
                name = entry["name"]
                expected_count = entry["expected_count"]
                expected_offsets = entry["expected_offsets"]
                offsets = find_matches(data, entry["pattern"])
                if len(offsets) < expected_count:
                    count_status = "missing_candidates"
                elif len(offsets) > expected_count:
                    count_status = "extra_candidates"
                else:
                    count_status = "expected_candidates"
                offsets_match = offsets == expected_offsets
                signature_verified = (
                    count_status == "expected_candidates" and offsets_match
                )
                all_verified = all_verified and signature_verified
                report["signatures"].append({
                    "name": name,
                    "verified": signature_verified,
                    "expected_count": expected_count,
                    "actual_count": len(offsets),
                    "match_count_status": count_status,
                    "expected_offsets": expected_offsets,
                    "match_offsets": offsets,
                    "offsets_match": offsets_match,
                })

            report["verified"] = all_verified
        finally:
            if mapped_binary is not None:
                mapped_binary.close()
    except OSError as error:
        raise VerificationInputError(f"unable to read binary: {error}") from error
    finally:
        binary_file.close()

    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args(argv)

    try:
        report = verify(args.manifest, args.binary)
        exit_code = 0 if report["verified"] else 1
    except VerificationInputError as error:
        report = {"verified": False, "error": str(error)}
        exit_code = 2

    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
