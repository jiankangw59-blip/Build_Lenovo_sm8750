"""Inject errors at settlement's final observation; no Android/device I/O."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path

from test_policy_daemon_host import sources
from test_policy_host import Fixture


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    before = sources()
    cases = [
        ("gc_physical_read_error", "lz4kd", "zms_stat", "physical_read_failed_pages", 1),
        ("physical_write_error", "lz4kd", "zms_stat", "physical_write_failed_pages", 1),
        ("sddc_integrity_error", "lz4kds", "sddc_stat", "integrity_failures", 1),
        ("sddc_decode_error", "lz4kds", "sddc_stat", "decode_failures", 1),
        ("sddc_flatten_error", "lz4kds", "sddc_stat", "flatten_failures", 1),
        ("sddc_delta_proof_error", "lz4kds", "sddc_stat", "delta_proof_failures", 1),
        ("foreground_read_error", "lz4kd", "io_stat", "read", 1),
        ("foreground_write_error", "lz4kd", "io_stat", "write", 1),
        ("successful_late_physical_io", "lz4kd", "zms_stat", "physical_write_pages", 6144),
    ]

    def check(case):
        name, codec, filename, counter, value = case
        fixture = Fixture(args.output / name, args.bash, codec)
        fixture.run("cw_load_config && cw_begin_round")
        (fixture.node / "writeback_limit").write_text("0\n")
        fixture.stats(4096, dirty=1)
        if filename == "io_stat":
            inject = f"builtin printf '{value if counter == 'read' else 0} {value if counter == 'write' else 0} 0 0\\n' > \"$CW_NODE/io_stat\""
        else:
            inject = f'sed -i "s/^{counter}:.*/{counter}: {value}/" "$CW_NODE/{filename}"'
            if counter == "physical_write_pages":
                inject += f'\nsed -i "s/^physical_write_bytes:.*/physical_write_bytes: {value * 4096}/" "$CW_NODE/{filename}"'
        # Interpose only a statistics change immediately before the shipped
        # final observer; preserve and execute its real counter/ledger code.
        body = r'''eval "$(declare -f cw_observe_physical | sed '1s/cw_observe_physical/cw_uninjected_observe_physical/')"
cw_observe_physical() {
''' + inject + r'''
    cw_uninjected_observe_physical "$@"
}
settle_rc=0
cw_load_config && cw_settle_pending || settle_rc=$?
builtin printf '%s %s %s %s\n' "$CW_ROUND_PHYSICAL" "$CW_ROUND_FAILED" "$CW_ROUND_READ_FAILED" "$CW_ROUND_SOFTWARE_ERROR"
exit "$settle_rc"
'''
        error = name != "successful_late_physical_io"
        observed = fixture.run(body, rc=2 if error else 0).split()
        assert observed == [str(6144 if not error else 4096),
                            str(int(counter == "physical_write_failed_pages")),
                            str(int(counter == "physical_read_failed_pages")),
                            str(int(filename in ("sddc_stat", "io_stat")))]
        assert (fixture.node / "writeback_limit").read_text().strip() == "67232"
        pending = (fixture.state / "crystal-writeback.pending").read_text().split()
        if error:
            assert pending[0] == "v2" and pending[6] == "restored"
            assert fixture.daily() == "20261124 4096"
        else:
            assert pending == ["none"]
            assert fixture.daily() == "20261124 6144"
        return {"case": name, "passed": True, "settlement_rejected_error": error}

    with ThreadPoolExecutor(max_workers=2) as executor:
        results = list(executor.map(check, cases))
    assert sources() == before, "Production sources changed during snapshot regression"
    report = {"passed": True, "checks": len(results), "cases": results,
              "source_sha256": before, "source_hashes_stable": True,
              "kernel_io_mocked": True, "device_used": False,
              "android_runtime_verified": False}
    (args.output / "host-checks.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "checks": len(results)}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bash", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    run(parser.parse_args())
