"""Acceptance parser contracts; never builds a kernel or accesses a device."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from prepare_ci_evidence import verify_kunit
from run_kunit import HERE, expected_cases, legacy_sched_switch_source, validate_cases


class CoverageTests(unittest.TestCase):
    def setUp(self):
        self.expected = ["frame_test_one", "frame_test_two"]
        self.result = {"name": "KUnit Test Group", "test_cases": [], "sub_groups": [
            {"name": "frame_policy", "test_cases": [
                {"name": name, "status": "PASS"} for name in self.expected], "sub_groups": []}]}

    def test_exact_named_cases_pass(self):
        report = validate_cases(self.result, self.expected)
        self.assertEqual(report["case_count"], 2)
        self.assertEqual(report["executed"], self.expected)

    def test_same_count_wrong_case_does_not_pass(self):
        changed = copy.deepcopy(self.result)
        changed["sub_groups"][0]["test_cases"][1]["name"] = "frame_test_old"
        with self.assertRaises(ValueError):
            validate_cases(changed, self.expected)

    def test_duplicate_cannot_stand_in_for_missing_case(self):
        changed = copy.deepcopy(self.result)
        changed["sub_groups"][0]["test_cases"][1]["name"] = self.expected[0]
        with self.assertRaises(ValueError):
            validate_cases(changed, self.expected)

    def test_skipped_failure_or_absent_cases_rejected(self):
        for status in ("FAIL", "SKIP", None):
            changed = copy.deepcopy(self.result)
            changed["sub_groups"][0]["test_cases"][0]["status"] = status
            with self.assertRaises(ValueError):
                validate_cases(changed, self.expected)
        with self.assertRaises(ValueError):
            validate_cases({}, self.expected)


class LegacySourceTests(unittest.TestCase):
    def setUp(self):
        self.source = (HERE / "frame_policy.c").read_text(encoding="utf-8")
        self.old = "\t\tirq_work_queue(&scene_irq_work);"
        self.boundary = self.old + "\n}\n\nstatic void scene_irq("

    def test_only_sched_switch_changes_with_other_irq_call_sites(self):
        unrelated = "\nstatic void unrelated(void)\n{\n" + self.old + "\n}\n"
        source = unrelated + self.source + unrelated
        self.assertEqual(source.count(self.boundary), 1)
        expected = source.replace(self.boundary,
            "\t\tqueue_work(policy_wq, &scene_work);\n}\n\nstatic void scene_irq(", 1)
        self.assertEqual(legacy_sched_switch_source(source), expected)
        self.assertEqual(expected.count(self.old), source.count(self.old) - 1)

    def test_missing_target_cannot_replace_another_irq_call(self):
        source = self.source.replace(self.boundary, "\n}\n\nstatic void scene_irq(", 1)
        self.assertIn(self.old, source)
        with self.assertRaises(ValueError):
            legacy_sched_switch_source(source)

    def test_multiple_targets_or_functions_fail_closed(self):
        duplicate_target = self.source.replace(self.boundary, self.old + "\n" + self.boundary, 1)
        for source in (duplicate_target, self.source + self.source):
            with self.subTest(duplicate_function=source == self.source + self.source):
                with self.assertRaises(ValueError):
                    legacy_sched_switch_source(source)


class ArtifactIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.source = "a" * 40
        result = {"name": "frame_policy", "sub_groups": [], "test_cases": [
            {"name": name, "status": "PASS"} for name in expected_cases()]}
        coverage = validate_cases(result, expected_cases())
        coverage["test_header_sha256"] = {
            name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
            for name in ("frame_policy_kunit.h", "scene_policy_kunit.h")}
        (self.directory / "fixed.json").write_text(json.dumps(result))
        (self.directory / "case-coverage.json").write_text(json.dumps(coverage))
        (self.directory / "workflow-commit.txt").write_text(self.source + "\n")
        (self.directory / "kunit-provenance.json").write_text(json.dumps({"source": self.source}))

    def tearDown(self):
        self.temporary.cleanup()

    def test_matching_fixture_passes_identity_parser(self):
        self.assertEqual(verify_kunit(self.directory, self.source)["expected"], expected_cases())

    def test_another_commit_is_not_accepted(self):
        with self.assertRaises(ValueError):
            verify_kunit(self.directory, "b" * 40)

    def test_stale_coverage_hashes_are_not_accepted(self):
        target = self.directory / "case-coverage.json"
        coverage = json.loads(target.read_text())
        coverage["test_header_sha256"]["frame_policy_kunit.h"] = "0" * 64
        target.write_text(json.dumps(coverage))
        with self.assertRaises(ValueError):
            verify_kunit(self.directory, self.source)

    def test_manifest_cannot_hide_different_executed_case(self):
        target = self.directory / "fixed.json"
        result = json.loads(target.read_text())
        result["test_cases"][0]["name"] = "frame_test_legacy"
        target.write_text(json.dumps(result))
        with self.assertRaises(ValueError):
            verify_kunit(self.directory, self.source)


if __name__ == "__main__":
    unittest.main()
