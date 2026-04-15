#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("gen_backend_map.py")


class BackendMapGeneratorTest(unittest.TestCase):
    def run_generator(self, rows: str):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = pathlib.Path(tmpdir)
            meta = tmpdir / "meta.tsv"
            out = tmpdir / "backend_map.txt"
            meta.write_text(rows, encoding="utf-8")

            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--input", str(meta), "--output", str(out)],
                capture_output=True,
                text=True,
            )
            text = out.read_text(encoding="utf-8") if out.exists() else ""
            return result, text

    def test_generates_expected_backends_and_labels(self):
        result, text = self.run_generator(
            "\n".join(
                [
                    "weights_q 0x1000 0x2000 mqsim weights_q",
                    "kv_cache 0x5000 0x4000 ramulator kv_cache",
                ]
            )
        )

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertEqual(
            text.strip().splitlines(),
            [
                "0x1000 0x2000 mqsim weights_q",
                "0x5000 0x4000 ramulator kv_cache",
            ],
        )

    def test_rejects_malformed_row(self):
        result, _ = self.run_generator("weights_q only_two_fields")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
