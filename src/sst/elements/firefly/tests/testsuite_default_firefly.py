# -*- coding: utf-8 -*-

from pathlib import Path

from sst_unittest import *
from sst_unittest_support import *


def function_body(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening:index + 1]
    raise AssertionError(f"Unclosed function body for {signature}")


class testcase_firefly(SSTTestCase):

    def test_firefly_empty_request_disabled_baseline(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/firefly_empty_request_regression.out"
        error = f"{out_dir}/firefly_empty_request_regression.err"
        self.run_sst(f"{test_dir}/empty_request_regression.py",
            output, error, timeout_sec=10)
        self.assertFalse(os_test_file(error, "-s"), f"Nonempty error file: {error}")
        text = Path(output).read_text(encoding="utf-8")
        self.assertEqual(1, text.count("Firefly ordinary empty Request:"))
        self.assertRegex(text, r"nic\.rcvdPkts.*Sum\.u64 = 1;")
        for statistic in (
                "collectiveEnqueued",
                "collectiveSchedulerSends", "collectiveSendRetries",
                "collectiveResultsCompleted"):
            self.assertNotIn(f"nic.{statistic}", text)

    def test_firefly_allreduce_recoverable_error(self):
        test_dir = self.get_testsuite_dir()
        out_dir = self.get_test_output_run_dir()
        output = f"{out_dir}/firefly_allreduce_recoverable_error.out"
        error = f"{out_dir}/firefly_allreduce_recoverable_error.err"
        self.run_sst(f"{test_dir}/allreduce_recoverable_error.py",
            output, error, timeout_sec=10)
        self.assertFalse(os_test_file(error, "-s"), f"Nonempty error file: {error}")
        text = Path(output).read_text(encoding="utf-8")
        self.assertEqual(1, text.count(
            "Firefly Allreduce accepted RecoverableError: restart=software state=cleared PASS"))

    def test_firefly_review_regressions(self):
        source_dir = Path(self.get_testsuite_dir()).parent
        nic = (source_dir / "nic.cc").read_text(encoding="utf-8")
        nic_header = (source_dir / "nic.h").read_text(encoding="utf-8")
        virt_nic = (source_dir / "virtNic.cc").read_text(encoding="utf-8")
        virt_nic_header = (source_dir / "virtNic.h").read_text(encoding="utf-8")
        recv = (source_dir / "nicRecvMachine.h").read_text(encoding="utf-8")
        allreduce = (source_dir / "funcSM" / "allreduce.cc").read_text(encoding="utf-8")
        allreduce_header = (source_dir / "funcSM" / "allreduce.h").read_text(encoding="utf-8")
        function_sm = (source_dir / "functionSM.cc").read_text(encoding="utf-8")

        publish = function_body(nic, "void Nic::tryPublishCollectiveRoute")
        submit = function_body(nic, "void Nic::handleCollectiveEvent")
        result = function_body(nic, "void Nic::processCollectivePacket")
        nic_constructor = function_body(nic, "Nic::Nic(ComponentId_t id")
        virt_constructor = function_body(virt_nic, "VirtNic::VirtNic")
        virt_init = function_body(virt_nic, "void VirtNic::init")
        all_ready = function_body(virt_nic, "void VirtNic::notifyReadyIfPossible")
        finish = function_body(allreduce, "CollectiveStartEvent* AllreduceOffloadFuncSM::finishOffload")
        complete = function_body(allreduce, "void AllreduceOffloadFuncSM::complete")
        self.assertIn("physical_endpoint_id = m_linkControl->getEndpointID()", publish)
        self.assertIn("physical_endpoint_id != m_myNodeId", publish)
        self.assertIn("collective->participant_logical_id", publish)
        self.assertIn("physical_endpoint_id = m_myNodeId", publish)
        self.assertIn("injection_dest_nid = collective->root_nid", publish)
        self.assertIn("collective->participant.logical_participant_id", submit)
        self.assertIn("makeStaticCollectiveContributionRequest", submit)
        self.assertIn("inspectStaticCollectiveResult", result)
        self.assertIn("if ( collective )", nic_constructor)
        self.assertEqual(1, nic_header.count("std::unique_ptr<FeatureState> m_featureState"))
        self.assertEqual(1, virt_nic_header.count(
            "std::unique_ptr<FeatureState> m_featureState"))
        self.assertIn("Firefly queue entry must retain its legacy footprint", nic_header)
        self.assertIn("Disabled Allreduce must not add per-rank state", allreduce_header)
        self.assertIn('module == "firefly"', function_sm)
        self.assertIn('implementation = "AllreduceOffload"', function_sm)
        self.assertNotIn("make_unique<FireflyCollectiveEndpoint>", virt_constructor)
        self.assertIn("collective_endpoint.emplace(*this)", virt_init)
        self.assertIn("state && state->collective_endpoint", all_ready)
        self.assertIn("delete req;\n                    return NULL;", recv)
        self.assertNotIn("fatal(", finish)
        self.assertIn("return restart_event", finish)
        self.assertIn("completion_status_ = status", complete)
        self.assertNotIn("status != CollectiveCompletionStatus::Success", complete)

    def test_firefly_collective_mapped_repeat(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        source = (test_dir.parent.parent / "ember" / "tests" /
            "ember_allreduce_innetwork.py").read_text(encoding="utf-8")
        source = source.replace(
            "endpoint_links=tuple((nid, nid, nid // 2, nid % 2) for nid in range(4)),",
            "endpoint_links=((0, 3, 0, 0), (1, 0, 0, 1), "
            "(2, 1, 1, 0), (3, 2, 1, 1)),")
        source = source.replace("Allreduce iterations=1", "Allreduce iterations=2")
        source = source.replace(
            'system.allocateNodes(job, "linear")',
            'system.allocateNodes(job, "random", 2)')

        config = out_dir / "firefly_collective_mapped_repeat.py"
        output = out_dir / "firefly_collective_mapped_repeat.out"
        error = out_dir / "firefly_collective_mapped_repeat.err"
        config.write_text(source, encoding="utf-8")
        self.run_sst(str(config), str(output), str(error),
            other_args='--model-options="supported"', timeout_sec=10)
        self.assertFalse(os_test_file(str(error), "-s"), f"Nonempty error file: {error}")
        text = output.read_text(encoding="utf-8")
        self.assertNotIn("SOFTWARE FALLBACK", text)
        self.assertNotIn("Event queue empty", text)
        self.assertEqual(1, text.count("Simulation is complete"))
        for rank in range(4):
            self.assertEqual(2, text.count(
                f"Firefly Allreduce rank {rank} OFFLOAD ACCEPTED"))
            self.assertEqual(1, text.count(f"Ember Allreduce verify rank {rank} "))
        self.assertIn("Allreduce: ranks 4, loop 2", text)

    def test_firefly_collective_required_service_missing(self):
        test_dir = Path(self.get_testsuite_dir())
        out_dir = Path(self.get_test_output_run_dir())
        source = (test_dir.parent.parent / "ember" / "tests" /
            "ember_allreduce_innetwork.py").read_text(encoding="utf-8")
        source = source.replace(
            "networkif.network_service_ids = [SERVICE_ID]",
            "networkif.network_service_ids = []")
        config = out_dir / "firefly_collective_missing_service.py"
        output = out_dir / "firefly_collective_missing_service.out"
        error = out_dir / "firefly_collective_missing_service.err"
        config.write_text(source, encoding="utf-8")
        self.run_sst(str(config), str(output), str(error),
            other_args='--model-options="supported"', expected_rc=1, timeout_sec=10)
        combined = output.read_text(encoding="utf-8") + error.read_text(encoding="utf-8")
        self.assertIn(
            "collectiveEnable=true but no validated collective service route is available",
            combined)
        self.assertNotIn("Simulation is complete", combined)
