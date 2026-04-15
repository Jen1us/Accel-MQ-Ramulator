#!/usr/bin/env python3

import csv
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("annotate_backend_trace.py")


class BackendTraceAnnotatorTest(unittest.TestCase):
    def run_annotator(self, meta_rows: str, trace_rows: str, layers: int = 2):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = pathlib.Path(tmpdir)
            meta = tmpdir / "backend_meta.tsv"
            trace = tmpdir / "hbf_requests.trace"
            out = tmpdir / "annotated.csv"
            meta.write_text(meta_rows, encoding="utf-8")
            trace.write_text(trace_rows, encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--meta",
                    str(meta),
                    "--trace",
                    str(trace),
                    "--output",
                    str(out),
                    "--layers",
                    str(layers),
                ],
                capture_output=True,
                text=True,
            )
            rows = []
            if out.exists():
                with out.open("r", encoding="utf-8") as fp:
                    rows = list(csv.DictReader(fp))
            return result, rows

    def test_annotates_region_layer_and_source_hint(self):
        result, rows = self.run_annotator(
            "\n".join(
                [
                    "# region base size backend label",
                    "weights_q 0x2000 0x200 mqsim weights_q",
                    "weights_k 0x3000 0x200 mqsim weights_k",
                    "weights_v 0x4000 0x200 mqsim weights_v",
                    "weights_o 0x5000 0x200 mqsim weights_o",
                    "weights_mlp_up 0x6000 0x200 mqsim weights_mlp_up",
                    "weights_mlp_gate 0x7000 0x200 mqsim weights_mlp_gate",
                    "weights_mlp_down 0x8000 0x200 mqsim weights_mlp_down",
                    "kv_cache 0x9000 0x400 ramulator kv_cache",
                ]
            ),
            "\n".join(
                [
                    "cycle,uid,partition,subpartition,addr,is_write,access_type,backend,compat_tag",
                    "1,11,0,0,0x2000,0,0,MQSIM,HBF",
                    "2,12,0,0,0x2100,0,0,MQSIM,HBF",
                    "3,13,0,0,0x3000,0,0,MQSIM,HBF",
                    "4,14,0,0,0x4000,0,0,MQSIM,HBF",
                    "5,15,0,0,0x5000,0,0,MQSIM,HBF",
                    "6,16,0,0,0x6000,0,0,MQSIM,HBF",
                    "7,17,0,0,0x7000,0,0,MQSIM,HBF",
                    "8,18,0,0,0x8000,0,0,MQSIM,HBF",
                    "9,19,0,0,0x9000,1,0,RAMULATOR,HBM",
                    "10,20,0,0,0x9200,0,0,RAMULATOR,HBM",
                ]
            ),
        )

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertEqual(rows[0]["region"], "weights_q")
        self.assertEqual(rows[0]["layer"], "0")
        self.assertEqual(rows[0]["source_hint"], "decoder_block_kernel:q_proj")

        self.assertEqual(rows[1]["region"], "weights_q")
        self.assertEqual(rows[1]["layer"], "1")
        self.assertEqual(rows[1]["source_hint"], "decoder_block_kernel:q_proj")

        self.assertEqual(rows[2]["region"], "weights_k")
        self.assertEqual(rows[2]["layer"], "0")
        self.assertEqual(rows[2]["source_hint"], "decoder_block_kernel:k_proj")

        self.assertEqual(rows[3]["region"], "weights_v")
        self.assertEqual(rows[3]["layer"], "0")
        self.assertEqual(rows[3]["source_hint"], "decoder_block_kernel:v_proj")

        self.assertEqual(rows[4]["region"], "weights_o")
        self.assertEqual(rows[4]["layer"], "0")
        self.assertEqual(rows[4]["source_hint"], "decoder_block_kernel:o_proj")

        self.assertEqual(rows[5]["region"], "weights_mlp_up")
        self.assertEqual(rows[5]["layer"], "0")
        self.assertEqual(rows[5]["source_hint"], "decoder_block_kernel:mlp_up")

        self.assertEqual(rows[6]["region"], "weights_mlp_gate")
        self.assertEqual(rows[6]["layer"], "0")
        self.assertEqual(rows[6]["source_hint"], "decoder_block_kernel:mlp_gate")

        self.assertEqual(rows[7]["region"], "weights_mlp_down")
        self.assertEqual(rows[7]["layer"], "0")
        self.assertEqual(rows[7]["source_hint"], "decoder_block_kernel:mlp_down")

        self.assertEqual(rows[8]["region"], "kv_cache")
        self.assertEqual(rows[8]["layer"], "0")
        self.assertEqual(rows[8]["source_hint"], "decoder_block_kernel:kv_write")

        self.assertEqual(rows[9]["region"], "kv_cache")
        self.assertEqual(rows[9]["layer"], "1")
        self.assertEqual(rows[9]["source_hint"], "decoder_block_kernel:kv_read")

    def test_rejects_unmapped_trace_row(self):
        result, _ = self.run_annotator(
            "weights_q 0x1000 0x100 mqsim weights_q",
            "\n".join(
                [
                    "cycle,uid,partition,subpartition,addr,is_write,access_type,backend,compat_tag",
                    "1,10,0,0,0x9999,0,0,RAMULATOR,HBM",
                ]
            ),
            layers=1,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unmapped", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
