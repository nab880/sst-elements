# Copyright 2009-2026 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.

import os

from sst_unittest import SSTTestCase


class testcase_iris(SSTTestCase):

    def test_sumi_collectives(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        model = os.path.join(test_path, "test_sumi_collectives.py")

        cases = ((1, 32768, False), (2, 32768, False),
                 (3, 32768, False), (4, 32768, False),
                 (5, 32768, False), (5, 0, False), (5, 0, True))
        for ranks, eager, use_put in cases:
            protocol = "eager" if eager else ("put" if use_put else "get")
            with self.subTest(ranks=ranks, protocol=protocol):
                stem = "sumi_collectives_{}_{}".format(ranks, protocol)
                outfile = os.path.join(outdir, stem + ".out")
                errfile = os.path.join(outdir, stem + ".err")
                model_args = "--nodes {} --eager-cutoff {}".format(
                    ranks, eager)
                if use_put:
                    model_args += " --use-put-protocol"
                options = '--model-options="{}"'.format(model_args)
                self.run_sst(model, outfile, errfile, set_cwd=test_path,
                             other_args=options)
                with open(outfile) as output_file:
                    output = output_file.read()
                self.assertNotIn("FAIL", output)
                for rank in range(ranks):
                    self.assertIn("Rank {} PASS".format(rank), output)
