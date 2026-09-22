# -*- coding: utf-8 -*-

from sst_unittest import *
from sst_unittest_support import *

from pathlib import Path

try:
    from sympy.polys.domains import ZZ
except:
    pass
try:
    from sympy.polys.galoistools import (gf_irreducible_p, gf_add, gf_mul, gf_rem)
except:
    pass
try:
    import networkx as nx
except:
    pass


class testcase_merlin_Component(SSTTestCase):

    def setUp(self):
        super(type(self), self).setUp()
        # Put test based setup code here. it is called once before every test

    def tearDown(self):
        # Put test based teardown code here. it is called once after every test
        super(type(self), self).tearDown()

    #####

    def test_merlin_dragon_128(self):
        self.merlin_test_template("dragon_128_test")

    def test_merlin_dragon_72(self):
        self.merlin_test_template("dragon_72_test")

    def test_merlin_fattree_128(self):
        self.merlin_test_template("fattree_128_test")

    def test_merlin_fattree_256(self):
        self.merlin_test_template("fattree_256_test")

    def test_merlin_torus_128(self):
        self.merlin_test_template("torus_128_test")

    def test_merlin_torus_5_trafficgen(self):
        self.merlin_test_template("torus_5_trafficgen")

    def test_merlin_torus_64(self):
        self.merlin_test_template("torus_64_test")

    def test_merlin_hyperx_128(self):
        self.merlin_test_template("hyperx_128_test")

    def test_merlin_dragon_128_platform(self):
        self.merlin_test_template("dragon_128_platform_test", True)

    def test_merlin_dragon_128_platform_cm(self):
        self.merlin_test_template("dragon_128_platform_test_cm", True)

    def test_merlin_dragon_128_fl(self):
        self.merlin_test_template("dragon_128_test_fl")

    def test_merlin_dragon_128_deferred(self):
        self.merlin_test_template("dragon_128_test_deferred")

    def test_merlin_request_ownership(self):
        self.merlin_test_template("request_ownership", exact=True, strict_stderr=True)

    def test_merlin_network_service_contract(self):
        self.merlin_test_template("network_service_contract", exact=True)

    def test_merlin_network_service_missing_processor(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        outfile = "{}/test_merlin_network_service_missing_processor.out".format(outdir)
        errfile = "{}/test_merlin_network_service_missing_processor.err".format(outdir)
        self.run_sst("{}/network_service_missing_processor.py".format(test_path), outfile, errfile,
            expected_rc=1, timeout_sec=5)
        combined = Path(outfile).read_text(encoding="utf-8") + Path(errfile).read_text(encoding="utf-8")
        self.assertIn("has no matching attached router processor", combined)

    def test_merlin_network_service_pass_tagged(self):
        self.merlin_test_template("network_service_pass_tagged", exact=True, strict_stderr=True)

    def test_merlin_network_service_vn_remap(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        outfile = f"{outdir}/test_merlin_network_service_vn_remap.out"
        errfile = f"{outdir}/test_merlin_network_service_vn_remap.err"
        self.run_sst(f"{test_path}/network_service_vn_remap.py", outfile, errfile,
                     timeout_sec=30)
        self.assertFalse(os_test_file(errfile, "-s"))
        text = Path(outfile).read_text(encoding="utf-8")
        self.assertEqual(1, text.count("Simulation is complete"))
        for scenario in ("permutation", "alias", "negotiated", "no_identity"):
            self.assertEqual(1, text.count(f"Merlin VN remap {scenario}: PASS"))

    def test_merlin_network_service_pass_baseline(self):
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        sdlfile = "{}/network_service_pass_baseline.py".format(test_path)
        disabled_out = "{}/test_merlin_network_service_pass_baseline_disabled.out".format(outdir)
        disabled_err = "{}/test_merlin_network_service_pass_baseline_disabled.err".format(outdir)
        enabled_out = "{}/test_merlin_network_service_pass_baseline_enabled.out".format(outdir)
        enabled_err = "{}/test_merlin_network_service_pass_baseline_enabled.err".format(outdir)

        self.run_sst(sdlfile, disabled_out, disabled_err)
        self.run_sst(sdlfile, enabled_out, enabled_err, other_args='--model-options="pass"')
        self.assertFalse(os_test_file(disabled_err, "-s"), "disabled baseline produced stderr")
        self.assertFalse(os_test_file(enabled_err, "-s"), "PASS baseline produced stderr")
        self.assertEqual(Path(disabled_out).read_bytes(), Path(enabled_out).read_bytes(),
            "installing the PASS processor changed ordinary traffic output or timing")

    @unittest.skipIf(not(('sympy.polys.galoistools' in sys.modules) and ('sympy.polys.domains' in sys.modules)), "Polarfly construction requires sympy")
    def test_merlin_polarfly_455(self):
        self.merlin_test_template("polarfly_455_test")

    @unittest.skipIf(not(('sympy.polys.galoistools' in sys.modules) and ('sympy.polys.domains' in sys.modules)), "Polarstar construction requires sympy")
    def test_merlin_polarstar_504(self):
        self.merlin_test_template("polarstar_504_test")

    @unittest.skipIf('networkx' not in sys.modules, "Anytopo tests require networkx")
    def test_merlin_anytopo_complete_4(self):
        self.merlin_test_template("anytopo_complete_4_test")

    @unittest.skipIf('networkx' not in sys.modules, "Anytopo tests require networkx")
    def test_merlin_anytopo_cubical(self):
        self.merlin_test_template("anytopo_cubical_test")

    @unittest.skipIf('networkx' not in sys.modules, "Anytopo tests require networkx")
    def test_merlin_anytopo_slimfly(self):
        self.merlin_test_template("anytopo_slimfly_test")

    @unittest.skipIf('networkx' not in sys.modules, "Anytopo tests require networkx")
    def test_merlin_anytopo_dallydragonfly(self):
        self.merlin_test_template("anytopo_dallydragonfly_test")

    @unittest.skipIf('networkx' not in sys.modules, "Anytopo tests require networkx")
    def test_merlin_anytopo_polarfly(self):
        self.merlin_test_template("anytopo_polarfly_test")

    @unittest.skipIf('networkx' not in sys.modules, "Anytopo tests require networkx")
    def test_merlin_anytopo_jellyfish(self):
        self.merlin_test_template("anytopo_jellyfish_test")

    @unittest.skipIf('networkx' not in sys.modules, "Anytopo tests require networkx")
    def test_merlin_anytopo_ember_complete_4(self):
        self.merlin_test_template("anytopo_ember_complete_4_test")


#####

    def merlin_test_template(self, testcase, cwd=False, exact=False, strict_stderr=False):
        # Get the path to the test files
        test_path = self.get_testsuite_dir()
        outdir = self.get_test_output_run_dir()
        tmpdir = self.get_test_output_tmp_dir()

        # Set the various file paths
        testDataFileName = "test_merlin_{0}".format(testcase)

        sdlfile = "{0}/{1}.py".format(test_path, testcase)
        reffile = "{0}/refFiles/{1}.out".format(test_path, testDataFileName)
        outfile = "{0}/{1}.out".format(outdir, testDataFileName)
        errfile = "{0}/{1}.err".format(outdir, testDataFileName)
        mpioutfiles = "{0}/{1}.testfile".format(outdir, testDataFileName)

        if cwd:
            self.run_sst(
                sdlfile, outfile, errfile, mpi_out_files=mpioutfiles, set_cwd=test_path
            )
        else:
            self.run_sst(sdlfile, outfile, errfile, mpi_out_files=mpioutfiles)

        # NOTE: THE PASS / FAIL EVALUATIONS ARE PORTED FROM THE SQE BAMBOO
        #       BASED testSuite_XXX.sh THESE SHOULD BE RE-EVALUATED BY THE
        #       DEVELOPER AGAINST THE LATEST VERSION OF SST TO SEE IF THE
        #       TESTS & RESULT FILES ARE STILL VALID

        # Perform the tests
        if os_test_file(errfile, "-s"):
            log_testing_note(
                "merlin test {0} has a Non-Empty Error File {1}".format(
                    testDataFileName, errfile
                )
            )
            if strict_stderr:
                self.fail("merlin test {} produced stderr: {}".format(testDataFileName, errfile))

        cmp_result = testing_compare_diff(testcase, outfile, reffile) if exact else \
            testing_compare_sorted_diff(testcase, outfile, reffile)
        if cmp_result == False:
            diffdata = testing_get_diff_data(testcase)
            log_failure(diffdata)
        self.assertTrue(
            cmp_result,
            "Output file {0} does not match Reference File {1}".format(
                outfile, reffile
            ),
        )
