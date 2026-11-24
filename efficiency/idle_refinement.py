"""Exact source-stage checks for the additive idle refinement profile only."""
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
PROFILE = "crystal-lz4kds-le9uo-idle"
POLICY = {
    "base_source_commit": "c1802dd6f3343e71c166a3c67c50554c81880b6e",
    "base_profile": "crystal-lz4kds-le9uo", "base_patch_count": 21, "base_ci_run": 45268976269,
    "patch_file": "patches/cpuidle-refinement-6.6.89.patch",
    "patch_sha256": "41d5f251b8b5cca264dcfeac4f14184022dbe5d433b5270aa9d4e0d92923e919",
    "files": {
        "drivers/cpuidle/governors/menu.c": {
            "before_sha256": "14baf13e9f815591eb0413977d34e4cba2f9b383913032c269f942a37e96b0ee",
            "after_sha256": "1014a5d30a2dffaa7592066a8b1686cf01f7126c4c96823894894a748fae3099",
        },
        "drivers/cpuidle/governors/teo.c": {
            "before_sha256": "ae8a4e511db0ef6957bd068a341c90385270ae36551fbb9390514872685cb75f",
            "after_sha256": "4a36b274c30fdc63dffd911001b21c2cdd14082b589da2520ab3a0142bf6ec43",
        },
    },
    "required_unchanged_files": {
        "drivers/cpuidle/governors/gov.h": "4adc865ad8f63dfd48130d823867c5296e71916973135f91c7ecbfaa30e7dc9b",
    },
    "upstream_commits": ["80606f4eb8d7484ab7f7d6f0fd30d71e6fbcf328", "e57c2bf2e89df3b176ab579abfd3ed54fd27034c",
                         "106a2662e655363db0dd73d9a91e1ec9afe9f4b1"],
    "global_kmi_layout_changed": False, "runtime_governor_changed": False, "fuzz": 0,
}


def load_118_port():
    port = json.loads((HERE / "ace6-6.6.118.json").read_text(encoding="utf-8"))
    idle = port["cpuidle_refinement"]
    if (port["base_kernel_version"] != "6.6.118" or
            port["base_commit"] != "65b954a773b183a9999b06bb1b2f921f12eaaf11" or
            port["profile"] != PROFILE or idle["base_source_commit"] != port["base_commit"] or
            any(idle[name] != POLICY[name] for name in (
                "base_profile", "base_patch_count", "upstream_commits",
                "global_kmi_layout_changed", "runtime_governor_changed", "fuzz")) or
            set(idle["files"]) != set(POLICY["files"]) or
            set(idle["required_unchanged_files"]) != set(POLICY["required_unchanged_files"])):
        raise RuntimeError("Unreviewed 6.6.118 efficiency port")
    return port


def refinement_policy(version="6.6.89"):
    if version == "6.6.89":
        review = json.loads((HERE / "cpuidle-refinement.json").read_text(encoding="utf-8"))
        if any(review.get(name) != value for name, value in POLICY.items()):
            raise RuntimeError("Unreviewed cpuidle refinement policy")
        policy = POLICY
    elif version == "6.6.118":
        policy = load_118_port()["cpuidle_refinement"]
    else:
        raise RuntimeError("Unreviewed cpuidle refinement kernel version: " + version)
    raw = (HERE / policy["patch_file"]).read_bytes()
    if hashlib.sha256(raw).hexdigest() != policy["patch_sha256"]:
        raise RuntimeError("Cpuidle refinement patch digest differs")
    old = re.findall(r"^--- a/(.+)$", raw.decode("utf-8"), re.M)
    new = re.findall(r"^\+\+\+ b/(.+)$", raw.decode("utf-8"), re.M)
    if len(old) != len(policy["files"]) or set(old) != set(policy["files"]) or new != old:
        raise RuntimeError("Cpuidle refinement destinations differ")
    return {**policy, "files": {name: dict(values) for name, values in policy["files"].items()},
            "required_unchanged_files": dict(policy["required_unchanged_files"]),
            "upstream_commits": list(policy["upstream_commits"])}


def check_source_hashes(observed, stage, policy=None):
    policy = refinement_policy() if policy is None else policy
    if stage not in ("before", "after"):
        raise ValueError("Unknown cpuidle refinement stage")
    normalized = {name.replace("\\", "/"): value for name, value in observed.items()}
    expected = {name: values[stage + "_sha256"] for name, values in policy["files"].items()}
    if len(normalized) != len(observed) or normalized != expected:
        raise RuntimeError("Cpuidle refinement " + stage + " source digests differ")


def source_report(kernel, stage="after", policy=None):
    kernel = Path(kernel).resolve()
    policy = refinement_policy() if policy is None else policy
    observed = {}
    for name in policy["files"]:
        target = (kernel / name).resolve()
        if not target.is_relative_to(kernel):
            raise RuntimeError("Cpuidle source escapes kernel tree")
        observed[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    check_source_hashes(observed, stage, policy)
    for name, expected in policy["required_unchanged_files"].items():
        target = (kernel / name).resolve()
        if not target.is_relative_to(kernel):
            raise RuntimeError("Cpuidle helper escapes kernel tree")
        observed[name] = hashlib.sha256(target.read_bytes()).hexdigest()
        if observed[name] != expected:
            raise RuntimeError("Cpuidle refinement prerequisite differs: " + name)
    return {"policy": policy, "source_sha256": observed, "passed": True}
