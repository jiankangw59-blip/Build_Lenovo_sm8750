"""Verify only the pinned Crystal 26-state + 6-codec KTAP cases, read-only.

Offline input is the early startup capture: boot UUID, sha256sum of
/sys/kernel/notes, /proc/version, then dmesg. --serial additionally reads the
current identity twice; it never installs, changes settings or reboots a device.
Offline parsing alone never sets runtime_verified=true.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from integrate_crystal import (DRIVER, PIN, integrity_patch_policy, kunit_config_patch_policy,
                               source_manifest)

UUID_RE = re.compile(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}")
NOTES_RE = re.compile(r"([0-9a-fA-F]{64})\s+\*?/sys/kernel/notes")
VERSION_RE = re.compile(r"( *)KTAP version (\d+)")
HEADER_RE = re.compile(r"( *)# Subtest: ([A-Za-z0-9_-]+)")
PLAN_RE = re.compile(r"( *)(\d+)\.\.(\d+)(?:\s+(.*))?")
RESULT_RE = re.compile(r"( *)(not ok|ok)\s+(\d+)\s+(?:-\s+)?([A-Za-z0-9_-]+)(?:\s+(.*))?")
NEGATIVE_CASE = "crystal_sddc_resident_hash_error_test"
STATE_SUITE = "crystal-sddc-state"


def expected_cases():
    manifest = source_manifest("crystal-lz4kds-test")
    integrity = integrity_patch_policy()
    test_overlay = kunit_config_patch_policy()
    result, source_hashes = {}, {}
    for filename, array, expected_count in [
        ("crystal_sddc.c", "crystal_sddc_state_test_cases", 26),
        ("crystal_sddc_codec_test.c", "crystal_sddc_codec_test_cases", 6),
    ]:
        name = DRIVER + "/sddc/" + filename
        raw = (HERE / "source" / name).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if digest != manifest["files"][name]:
            raise RuntimeError("Unreviewed KUnit declaration source: " + name)
        source_hashes[name] = digest
        text = raw.decode("utf-8")
        table = re.search(r"static struct kunit_case " + array + r"\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
        suite = re.search(r"static struct kunit_suite\s+\w+\s*=\s*\{([^}]+\.test_cases\s*=\s*" + array + r"[^}]*)\};", text, re.S)
        if not table or not suite:
            raise RuntimeError("Missing exact KUnit case table: " + array)
        label = re.search(r'\.name\s*=\s*"([^"]+)"', suite[1])
        if not label:
            raise RuntimeError("Missing KUnit suite name")
        names = re.findall(r"\bKUNIT_CASE\((\w+)\)", table[1])
        if filename == "crystal_sddc.c":
            patch = (HERE / "sddc-integrity.patch").read_text(encoding="utf-8")
            additions = re.findall(r"^\+\s*KUNIT_CASE\((\w+)\),$", patch, re.M)
            if len(names) != 24 or len(additions) != integrity["new_state_cases"]:
                raise RuntimeError("Unexpected original/overlay KUnit declaration count")
            # The pinned overlay inserts its two cases at the table's beginning.
            anchor = " static struct kunit_case " + array + "[] = {\n"
            if patch.count(anchor) != 1 or not patch.split(anchor, 1)[1].startswith(
                    "".join("+\tKUNIT_CASE(" + name + "),\n" for name in additions)):
                raise RuntimeError("Unexpected integrity KUnit insertion order")
            names = additions + names
        if len(names) != expected_count or len(set(names)) != expected_count:
            raise RuntimeError("Unexpected KUnit names or duplicates")
        result[label[1]] = names
    if set(result) != {STATE_SUITE, "crystal-sddc-codec"}:
        raise RuntimeError("Unexpected Crystal KUnit suite inventory")
    return result, {"upstream_commit": PIN, "source_sha256": source_hashes,
                    "integrity_patch_sha256": integrity["patch_sha256"], "expected_total_cases": 32,
                    "sddc_kunit_config_patch": test_overlay,
                    "installed_sddc_sha256": test_overlay["patched_sha256"]}


def strip_prefix(line):
    # Remove one printk separator, preserving the meaningful KTAP indentation.
    return re.sub(r"^(?:<\d+>)?\[\s*\d+(?:\.\d+)?\](?: ?\[\s*(?:T|C)\s*\d+\])? ?", "", line)


def captured_identity(lines):
    nonempty = [(n + 1, line) for n, line in enumerate(lines) if line.strip()]
    errors = []
    identity = {"boot_id": None, "kernel_notes_sha256": None, "kernel_version": None}
    if len(nonempty) < 3:
        return identity, ["Missing early-capture identity header"]
    first, second, third = [item[1].strip() for item in nonempty[:3]]
    if UUID_RE.fullmatch(first): identity["boot_id"] = str(uuid.UUID(first))
    else: errors.append("First capture line must be the boot UUID")
    notes = NOTES_RE.fullmatch(second)
    if notes: identity["kernel_notes_sha256"] = notes[1].lower()
    else: errors.append("Second capture line must hash /sys/kernel/notes")
    if third.startswith("Linux version "): identity["kernel_version"] = third
    else: errors.append("Third capture line must contain /proc/version")
    if sum(bool(UUID_RE.fullmatch(line.strip())) for line in lines) != 1:
        errors.append("Ambiguous boot headers in capture")
    if sum(bool(NOTES_RE.fullmatch(line.strip())) for line in lines) != 1:
        errors.append("Ambiguous kernel notes headers in capture")
    return identity, errors


def parse_suites(messages, expected):
    suites, errors, active, pending_version = [], [], None, None
    expected_names = {name for names in expected.values() for name in names}
    other_failures = []

    def finish_incomplete(reason):
        nonlocal active
        if active and active["name"] in expected:
            active["errors"].append(reason)
        active = None

    for line_no, message in messages:
        version, header = VERSION_RE.fullmatch(message), HEADER_RE.fullmatch(message)
        plan, outcome = PLAN_RE.fullmatch(message), RESULT_RE.fullmatch(message)
        if version:
            finish_incomplete("New KTAP header before suite result")
            pending_version = (line_no, len(version[1]), int(version[2]))
        elif header:
            finish_incomplete("New subtest before suite result")
            name = header[2]
            active = {"name": name, "header_line": line_no, "version_line": None,
                      "plan": None, "cases": [], "result": None, "errors": []}
            if name in expected:
                suites.append(active)
                if len(header[1]) != 4 or not pending_version or pending_version[1:] != (4, 1):
                    active["errors"].append("Missing canonical suite-local KTAP version 1 header")
                else: active["version_line"] = pending_version[0]
            pending_version = None
        elif plan:
            if active and active["name"] in expected:
                if active["plan"] is not None or active["cases"]:
                    active["errors"].append("Duplicate or late suite plan")
                active["plan"] = {"first": int(plan[2]), "last": int(plan[3]), "line": line_no}
                if len(plan[1]) != 4 or int(plan[2]) != 1 or int(plan[3]) != len(expected[active["name"]]) or plan[4]:
                    active["errors"].append("Suite plan/count/directive differs")
        elif outcome:
            indent, status, number, name, directive = len(outcome[1]), outcome[2], int(outcome[3]), outcome[4], outcome[5]
            record = {"name": name, "number": number, "status": status, "directive": directive,
                      "line": line_no, "message": message}
            if active and active["name"] in expected:
                if indent == 4:
                    active["cases"].append(record)
                elif indent == 0 and name == active["name"]:
                    active["result"] = record
                    active = None
                else:
                    active["errors"].append("Result crosses suite/case boundary: " + name)
            elif name in expected_names or name in expected:
                errors.append("Orphan/duplicate target result at line " + str(line_no))
            elif status != "ok" or directive:
                other_failures.append(record)
    finish_incomplete("Capture ended before suite result")
    for name, names in expected.items():
        found = [suite for suite in suites if suite["name"] == name]
        if len(found) != 1:
            errors.append(f"Expected exactly one {name} suite; found {len(found)}")
        for suite in found:
            if suite["plan"] is None: suite["errors"].append("Missing suite plan")
            if [case["name"] for case in suite["cases"]] != names:
                suite["errors"].append("Missing/extra/reordered target cases")
            if [case["number"] for case in suite["cases"]] != list(range(1, len(names) + 1)):
                suite["errors"].append("Case sequence numbers differ")
            if any(case["status"] != "ok" or case["directive"] for case in suite["cases"]):
                suite["errors"].append("A target case failed, skipped or has a directive")
            result = suite["result"]
            if not result or result["status"] != "ok" or result["directive"] or result["number"] < 1:
                suite["errors"].append("Missing/failed/skipped suite result")
            suite["passed"] = not suite["errors"]
    results = [suite["result"]["number"] for suite in suites if suite["result"]]
    if results != sorted(set(results)):
        errors.append("Target suite result sequence is not strictly increasing")
    return suites, errors, other_failures


def classify_integrity_logs(messages, suites, identity_matches):
    candidates = [(n, text) for n, text in messages if "sddc_integrity mismatch" in text]
    state = [suite for suite in suites if suite["name"] == STATE_SUITE and suite.get("passed")]
    interval = None
    if identity_matches and len(state) == 1:
        records = state[0]["cases"]
        for index, record in enumerate(records):
            if record["name"] == NEGATIVE_CASE:
                interval = (records[index - 1]["line"] if index else state[0]["plan"]["line"], record["line"])
    parsed = []
    for line_no, message in candidates:
        pairs = re.findall(r"(\w+)=([^\s]+)", message)
        fields = dict(pairs)
        fixed = {"dev": "unknown", "index": "0", "kind": "1", "ref": "41:23",
                 "ref_size": "4096", "target_size": "4096"}
        controlled = bool(interval and interval[0] < line_no < interval[1] and
                          len(pairs) == len(fields) and message.count("sddc_integrity mismatch") == 1 and
                          all(fields.get(key) == value for key, value in fixed.items()) and
                          fields.get("path") in ("resident_read", "resident_read_locked"))
        try:
            controlled = controlled and int(fields["expected_hash"], 0) != 0 and (
                int(fields["expected_hash"], 0) != int(fields["actual_hash"], 0))
        except (KeyError, ValueError): controlled = False
        parsed.append({"line": line_no, "message": message, "fields": fields,
                       "controlled_negative_selftest": controlled})
    paths = [row["fields"].get("path") for row in parsed if row["controlled_negative_selftest"]]
    if len(paths) != len(set(paths)):
        for row in parsed: row["controlled_negative_selftest"] = False
    return {"expected_negative_log_count": 2, "count_not_required_because_printk_may_be_ratelimited": True,
            "controlled": [row for row in parsed if row["controlled_negative_selftest"]],
            "unclassified": [row for row in parsed if not row["controlled_negative_selftest"]],
            "condition": "exact private fixture identity and interval inside a fully passing state suite"}


def verify_log(raw, expected_notes, expected_boot_id, live_before=None, live_after=None):
    expected, declarations = expected_cases()
    text = raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    captured, errors = captured_identity(lines)
    expected_notes = expected_notes.lower()
    expected_boot_id = str(uuid.UUID(expected_boot_id))
    if captured["boot_id"] != expected_boot_id: errors.append("Captured boot ID differs")
    if captured["kernel_notes_sha256"] != expected_notes: errors.append("Captured kernel notes differ")
    live_verified = live_before is not None and live_after is not None
    if live_before is not None or live_after is not None:
        if not live_verified or live_before != live_after:
            errors.append("Live identity changed or was not checked twice")
        for value in [live_before, live_after]:
            if value is not None and (value.get("model") != "PLQ110" or value.get("boot_id") != expected_boot_id or
                                      value.get("kernel_notes_sha256") != expected_notes):
                errors.append("Live device identity differs")
    identity_matches = not errors
    messages = [(n + 1, strip_prefix(line)) for n, line in enumerate(lines)]
    suites, protocol_errors, other_failures = parse_suites(messages, expected)
    errors.extend(protocol_errors)
    for suite in suites:
        errors.extend(suite["name"] + ": " + message for message in suite["errors"])
    integrity = classify_integrity_logs(messages, suites, identity_matches)
    if integrity["unclassified"]: errors.append("Unclassified SDDC integrity errors remain in the capture")
    passed = not errors and len(suites) == 2 and all(suite.get("passed") for suite in suites)
    return {"passed": passed, "runtime_verified": passed and live_verified,
            "scope": "Only the pinned 26 Crystal state and 6 codec cases; not all kernel KUnit or device integrity",
            "log_sha256": hashlib.sha256(raw).hexdigest(), "log_bytes": len(raw),
            "verifier_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "identity": {"expected_boot_id": expected_boot_id, "expected_notes_sha256": expected_notes,
                         "captured": captured, "live_before": live_before, "live_after": live_after},
            "declarations": declarations, "target_suites": suites,
            "actual_suite_order": [suite["name"] for suite in suites],
            "target_case_count": sum(len(suite["cases"]) for suite in suites),
            "controlled_selftest_logs": integrity, "other_observed_ktap_failures": other_failures,
            "other_kernel_errors_not_evaluated": True, "errors": errors}


def read_identity(serial):
    code = "set -e\ngetprop ro.product.model\ncat /proc/sys/kernel/random/boot_id\nsha256sum /sys/kernel/notes\n"
    process = subprocess.run(["adb", "-s", serial, "shell", "su -c " + shlex.quote(code)],
                             capture_output=True, check=True, timeout=30)
    lines = process.stdout.decode("utf-8", "replace").replace("\r", "").strip().splitlines()
    if len(lines) != 3 or not UUID_RE.fullmatch(lines[1]) or not NOTES_RE.fullmatch(lines[2]):
        raise RuntimeError("Unexpected read-only ADB identity response")
    return {"model": lines[0], "boot_id": str(uuid.UUID(lines[1])),
            "kernel_notes_sha256": NOTES_RE.fullmatch(lines[2])[1].lower()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--startup", type=Path, required=True)
    parser.add_argument("--expected-notes", required=True)
    parser.add_argument("--expected-boot-id", help="Required offline; otherwise derived from read-only --serial")
    parser.add_argument("--serial", help="Read current identity twice; never modifies the phone")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = {"passed": False, "runtime_verified": False}
    try:
        if not re.fullmatch(r"[0-9a-fA-F]{64}", args.expected_notes):
            raise ValueError("Expected notes must be a SHA256 digest")
        before = read_identity(args.serial) if args.serial else None
        boot = args.expected_boot_id or (before["boot_id"] if before else None)
        if not boot: raise ValueError("Provide --expected-boot-id or --serial")
        raw = args.startup.read_bytes()
        after = read_identity(args.serial) if args.serial else None
        result = verify_log(raw, args.expected_notes, boot, before, after)
        result["startup_path"] = str(args.startup.resolve())
        result["serial"] = args.serial
    except Exception as error:
        result["errors"] = [str(error)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "runtime_verified": result["runtime_verified"],
                      "target_case_count": result.get("target_case_count", 0), "errors": result.get("errors", [])}, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
