import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
VERIFIER = ROOT / "tools" / "verify_compatibility.py"


class CompatibilityVerifierTests(unittest.TestCase):
    def run_verifier_paths(self, manifest_path, binary_path, *, timeout=10):
        completed = subprocess.run(
            [
                sys.executable,
                str(VERIFIER),
                "--manifest",
                str(manifest_path),
                "--binary",
                str(binary_path),
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return completed, json.loads(completed.stdout)

    def run_verifier(self, binary_data, signatures, *, manifest_sha=None):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            binary = temp / "server.exe"
            binary.write_bytes(binary_data)
            manifest = {
                "binary": {
                    "size": len(binary_data),
                    "sha256": manifest_sha or hashlib.sha256(binary_data).hexdigest(),
                },
                "signatures": signatures,
            }
            manifest_path = temp / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            return self.run_verifier_paths(manifest_path, binary)

    def test_success_verifies_identity_count_and_offsets(self):
        binary = bytes.fromhex("00 AA BB 10 DD 00 AA BB 20 DD 00")
        signatures = [{
            "name": "FText_Constructor",
            "pattern": "AA BB ?? DD",
            "expected_count": 2,
            "expected_offsets": [1, 6],
        }]

        completed, report = self.run_verifier(binary, signatures)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(report["verified"])
        self.assertEqual(report["signatures"][0]["match_count_status"], "expected_candidates")
        self.assertEqual(report["signatures"][0]["match_offsets"], [1, 6])

    def test_hash_mismatch_fails_closed_before_signature_scan(self):
        binary = bytes.fromhex("AA BB CC")
        signatures = [{
            "name": "example",
            "pattern": "AA BB",
            "expected_count": 1,
            "expected_offsets": [0],
        }]

        completed, report = self.run_verifier(binary, signatures, manifest_sha="0" * 64)

        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(report["verified"])
        self.assertFalse(report["binary"]["sha256_matches"])
        self.assertEqual(report["signatures"], [])

    def test_missing_signature_is_reported_as_missing_candidates(self):
        binary = bytes.fromhex("00 AA BC 00")
        signatures = [{
            "name": "missing",
            "pattern": "AA BB",
            "expected_count": 1,
            "expected_offsets": [1],
        }]

        completed, report = self.run_verifier(binary, signatures)

        self.assertNotEqual(completed.returncode, 0)
        result = report["signatures"][0]
        self.assertEqual(result["match_count_status"], "missing_candidates")
        self.assertEqual(result["actual_count"], 0)

    def test_extra_match_is_reported_as_extra_candidates(self):
        binary = bytes.fromhex("AA BB 00 AA BB 00 AA BB")
        signatures = [{
            "name": "FText_Constructor",
            "pattern": "AA BB",
            "expected_count": 2,
            "expected_offsets": [0, 3],
        }]

        completed, report = self.run_verifier(binary, signatures)

        self.assertNotEqual(completed.returncode, 0)
        result = report["signatures"][0]
        self.assertEqual(result["expected_count"], 2)
        self.assertEqual(result["actual_count"], 3)
        self.assertEqual(result["match_count_status"], "extra_candidates")

    def test_wildcards_do_not_turn_literal_bytes_into_regex_syntax(self):
        pattern = "48 8D 0D ?? ?? ?? ?? 48 8B D7 89 5C 24 20 44 8D 4B ??"
        match = bytes.fromhex("48 8D 0D 11 22 33 44 48 8B D7 89 5C 24 20 44 8D 4B FF")
        binary = b"prefix" + match + b"suffix"
        signatures = [{
            "name": "GUObjectArray",
            "pattern": pattern,
            "expected_count": 1,
            "expected_offsets": [6],
        }]

        completed, report = self.run_verifier(binary, signatures)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(report["signatures"][0]["match_offsets"], [6])

    def test_zero_expected_count_is_rejected(self):
        completed, report = self.run_verifier(
            b"AA",
            [{
                "name": "empty-proof",
                "pattern": "AA",
                "expected_count": 0,
                "expected_offsets": [],
            }],
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("expected_count must be strictly positive", report["error"])

    def test_oversized_manifest_is_rejected_before_parsing(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            manifest = temp / "manifest.json"
            manifest.write_bytes(b" " * (1024 * 1024 + 1))
            binary = temp / "server.exe"
            binary.write_bytes(b"")

            completed, report = self.run_verifier_paths(manifest, binary)

        self.assertEqual(completed.returncode, 2)
        self.assertIn("manifest exceeds 1048576-byte limit", report["error"])

    def test_oversized_binary_returns_before_hash_or_scan(self):
        binary = b"AA BB"
        signatures = [{
            "name": "must-not-scan",
            "pattern": "AA BB",
            "expected_count": 1,
            "expected_offsets": [0],
        }]
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            binary_path = temp / "server.exe"
            binary_path.write_bytes(binary)
            manifest = {
                "binary": {
                    "size": len(binary) - 1,
                    "sha256": hashlib.sha256(binary).hexdigest(),
                },
                "signatures": signatures,
            }
            manifest_path = temp / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            completed, report = self.run_verifier_paths(manifest_path, binary_path)

        self.assertEqual(completed.returncode, 1)
        self.assertFalse(report["binary"]["size_matches"])
        self.assertIsNone(report["binary"]["actual_sha256"])
        self.assertEqual(report["signatures"], [])

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFO test requires os.mkfifo")
    def test_fifo_binary_is_rejected_without_blocking(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            binary = temp / "server.exe"
            os.mkfifo(binary)
            manifest = {
                "binary": {
                    "size": 0,
                    "sha256": hashlib.sha256(b"").hexdigest(),
                },
                "signatures": [{
                    "name": "must-not-open",
                    "pattern": "AA",
                    "expected_count": 1,
                    "expected_offsets": [0],
                }],
            }
            manifest_path = temp / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            completed, report = self.run_verifier_paths(
                manifest_path, binary, timeout=2
            )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("binary must be a regular file", report["error"])

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFO test requires os.mkfifo")
    def test_fifo_manifest_is_rejected_without_blocking(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            manifest = temp / "manifest.json"
            os.mkfifo(manifest)
            binary = temp / "server.exe"
            binary.write_bytes(b"")

            completed, report = self.run_verifier_paths(
                manifest, binary, timeout=2
            )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("manifest must be a regular file", report["error"])


if __name__ == "__main__":
    unittest.main()
