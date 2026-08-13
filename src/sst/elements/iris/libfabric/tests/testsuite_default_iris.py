# -*- coding: utf-8 -*-

import os
from contextlib import contextmanager

from sst_unittest import SSTTestCase, setUpModule, tearDownModule


@contextmanager
def temporary_environment(values):
    old = {key: os.environ.get(key) for key in values}
    for key, value in values.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value
    try:
        yield
    finally:
        for key, value in old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class testcase_iris_fi_collective(SSTTestCase):

    def test_fi_collective_single_rank(self):
        self.run_collective("fi_collective_single_rank", 1)

    def test_fi_collective_world_and_subgroup(self):
        self.run_collective("fi_collective_world_and_subgroup", 8)

    def test_fi_collective_parameter_algorithm(self):
        parameter_output = self.run_collective(
            "fi_collective_parameter_algorithm", 8, param_algorithm="ring")
        environment_output = self.run_collective(
            "fi_collective_parameter_reference", 8, env_algorithm="ring")
        self.assertEqual(parameter_output, environment_output)

    def test_fi_collective_environment_algorithm(self):
        self.run_collective("fi_collective_environment_algorithm", 8,
                            env_algorithm="recdouble")

    def run_collective(self, name, ranks, param_algorithm=None,
                       env_algorithm=None):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        outfile = os.path.join(outdir, name + ".out")
        errfile = os.path.join(outdir, name + ".err")
        env = {
            "NRANKS": str(ranks),
            "SUMI_ALLREDUCE_PARAM": param_algorithm,
            "SUMI_ALLREDUCE_ALG": env_algorithm,
        }
        with temporary_environment(env):
            self.run_sst(os.path.join(test_path, "test_fi_allreduce.py"),
                         outfile, errfile, set_cwd=test_path)

        with open(outfile) as output_file:
            output = output_file.read()
        self.assertIn("PASS: fi_collectives ({} ranks".format(ranks), output)
        self.assertNotIn("FAIL:", output)
        return output
