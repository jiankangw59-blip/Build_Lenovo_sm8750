"""Exercise the service's actual OEM property wait without Android or sysfs writes."""

from pathlib import Path
import subprocess
import unittest


SERVICE = Path(__file__).with_name("service.sh")
MOCK = r"""
case_name=$1
calls=0
value=
read -r uptime_before_wait _ < /proc/uptime || exit 40
resetprop=mock_resetprop
stage=wait_vendor_post_boot

getprop() {
    [ "$1" = vendor.post_boot.parsed ] || exit 41
    printf '%s\n' "$value"
}

mock_resetprop() {
    [ "$#" = 5 ] && [ "$1" = --timeout ] &&
        [ "$2" -gt 0 ] && [ "$2" -lt "$startup_budget" ] &&
        [ "$3" = -w ] && [ "$4" = vendor.post_boot.parsed ] &&
        [ "$5" = "$value" ] || exit 42
    calls=$((calls + 1))
    case "$case_name:$value" in
        direct:|middle:0) value=1 ;;
        middle:) value=0 ;;
        invalid:) value=42 ;;
        timeout:) return 9 ;;
        unchanged:) ;;
        *) exit 43 ;;
    esac
}

case "$case_name" in
    ready) value=1 ;;
    expired) startup_budget=0 ;;
esac
wait_vendor_post_boot
result=$?
printf '%s:%s:%s:%s:%s:%s\n' "$result" "$calls" "$vendorprop_before" \
    "$vendorprop_after" "$vendor_wait_exit" "$stage"
"""


class ServiceStartup(unittest.TestCase):
    def test_oem_property_wait(self):
        functions, boundary, _ = SERVICE.read_bytes().partition(b"\ncapture() {")
        self.assertTrue(boundary, "Expected service functions before the log capture entry point")
        cases = {
            "ready": "0:0:1:1:0:wait_vendor_post_boot",
            "direct": "0:1::1:0:wait_vendor_post_boot",
            "middle": "0:2::1:0:wait_vendor_post_boot",
            "invalid": "1:1::42:0:wait_vendor_post_boot",
            "timeout": "1:1:::9:wait_vendor_post_boot",
            "unchanged": "1:1:::0:wait_vendor_post_boot",
            "expired": "1:0:::0:wait_vendor_post_boot_budget",
        }
        for name, expected in cases.items():
            with self.subTest(name=name):
                result = subprocess.run(
                    ["sh", "-s", "--", name],
                    input=functions + MOCK.encode(),
                    capture_output=True,
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(result.stdout.decode().strip(), expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
