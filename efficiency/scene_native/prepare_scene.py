"""Prepare a narrow, hash-locked Scene 4.3.1 native-ZRAM compatibility diff."""
import argparse
import hashlib
from pathlib import Path

EXPECTED = {
    "startup.sh": "fa6fb614138e45ba42956ca138cd55d86051f232ec29ffe3773689c88b5091d1",
    "tools.sh": "0211ce173f6a6bc3aa37de9baeb09db19ecd29ce3e70aa9cba85885de4d6441a",
}


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected exactly one Scene source anchor: {old!r}")
    return text.replace(old, new, 1)


def prepare(source, output):
    contents = {}
    for name, digest in EXPECTED.items():
        raw = (source / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError(f"Unreviewed Scene source: {name}")
        contents[name] = raw.decode("utf-8").replace("\r\n", "\n")
    tools = contents["tools.sh"]
    tools = replace_once(tools, "oplus_hybridswap_needs_guard() {\n", """oplus_hybridswap_needs_guard() {
  if command -v ace6_native_profile_active >/dev/null 2>&1 && ace6_native_profile_active; then
    return 1
  fi
""")
    tools += '\n[ ! -f "$DIR/native_writeback.sh" ] || source "$DIR/native_writeback.sh"\n'
    startup = contents["startup.sh"]
    startup = replace_once(startup, "case \"${1:-}\" in\n", "SCENE_NATIVE_ONLY=false\ncase \"${1:-}\" in\n")
    startup = replace_once(startup, "  --check-only) SCENE_SWAP_CHECK_ONLY=true ;;", """  --check-only) SCENE_SWAP_CHECK_ONLY=true ;;
  --native-writeback-only) SCENE_SWAP_CHECK_ONLY=true; SCENE_NATIVE_ONLY=true ;;""")
    startup = replace_once(startup, "zram_writeback=${zram_writeback:-default}\n", """zram_writeback=${zram_writeback:-default}
if command -v ace6_native_profile_active >/dev/null 2>&1 && ace6_native_profile_active; then
  if [ "$zram" = true ] && [ "${zram_size:-0}" != 0 ] && [ "$zram_writeback" = default ]; then
    zram_writeback=true
  fi
  trap 'ace6_native_cleanup_loop' EXIT
fi
""")
    startup = replace_once(startup, '      if [ "$zram_increase" = 0 ]; then valid_backing_device "$current_backing" || return 1; fi', """      if [ "$zram_increase" = 0 ]; then
        if command -v ace6_native_profile_active >/dev/null 2>&1 && ace6_native_profile_active; then
          ace6_native_backing_matches "$current_backing" || return 1
        else
          valid_backing_device "$current_backing" || return 1
        fi
      fi""")
    startup = replace_once(startup, "    swap_state=0\n    zram_swap_active || swap_state=$?\n    case \"$swap_state\" in\n      0) swapoff", """    if command -v ace6_native_profile_active >/dev/null 2>&1 && ace6_native_profile_active &&
       [ "$zram_writeback" = true ] && [ "$zram_size" != 0 ]; then
      ace6_native_transition_safe "$backing_dev" || return 1
      ace6_native_prepare_backing || return 1
      ace6_native_ram_headroom || return 1
    fi
    swap_state=0
    zram_swap_active || swap_state=$?
    case "$swap_state" in
      0) swapoff""")
    startup = replace_once(startup, "set_zram_writeback() {\n", """set_zram_writeback() {
  if command -v ace6_native_profile_active >/dev/null 2>&1 && ace6_native_profile_active; then
    ace6_native_setup_backing
    return $?
  fi
""")
    startup = replace_once(startup, "    if oplus_hybridswap_needs_guard; then\n      echo '[SAFE-HS] Preserve vendor swap policy; skip indirect disable request'", """    if oplus_hybridswap_needs_guard ||
       { command -v ace6_native_profile_active >/dev/null 2>&1 && ace6_native_profile_active; }; then
      echo '[SAFE-HS] Preserve vendor swap policy; skip indirect disable request'""")
    startup = replace_once(startup, "if [ \"$SCENE_SWAP_CHECK_ONLY\" = true ]; then\n  zram_check_only", """if [ "$SCENE_NATIVE_ONLY" = true ]; then
  ace6_native_profile_active || exit 1
  set_zram || { ace6_native_restore_ram_swap; exit 1; }
  exit 0
fi
if [ "$SCENE_SWAP_CHECK_ONLY" = true ]; then
  zram_check_only""")
    startup = replace_once(startup, "set_zram || { echo '[SAFE-HS] ZRAM setup failed; abort memory reconfiguration' >&2; exit 1; }", """set_zram || {
  if command -v ace6_native_profile_active >/dev/null 2>&1 && ace6_native_profile_active; then
    ace6_native_restore_ram_swap || echo '[ACE6-NATIVE] RAM swap recovery failed' >&2
  fi
  echo '[SAFE-HS] ZRAM setup failed; abort memory reconfiguration' >&2
  exit 1
}""")
    output.mkdir(parents=True, exist_ok=True)
    for name, text in {"startup.sh": startup, "tools.sh": tools}.items():
        (output / name).write_text(text, encoding="utf-8", newline="\n")
    for name in ["native_writeback.sh", "native_writeback_daemon.sh", "start_native_writeback.sh", "early_native_writeback.sh"]:
        helper = Path(__file__).with_name(name).read_text(encoding="utf-8")
        (output / name).write_text(helper, encoding="utf-8", newline="\n")
    service = (source.parent / "service.sh").read_bytes()
    if hashlib.sha256(service).hexdigest() != "593bb2081d6416085378e0c849a7c5963208397c4bec03e66efe5a0d19afc08a":
        raise ValueError("Unreviewed Scene service source")
    service = service.decode("utf-8").replace("\r\n", "\n")
    # Hook only after the existing startup succeeded, before the remaining work.
    service = replace_once(service, "echo \"\" >> $LOG\n", 'sh "$MODPATH/scripts/start_native_writeback.sh" || echo "Native writeback service start failed" >> "$LOG"\n\necho "" >> $LOG\n')
    (output.parent / "service.sh").write_text(service, encoding="utf-8", newline="\n")
    postfs = (source.parent / "post-fs-data.sh").read_bytes()
    if hashlib.sha256(postfs).hexdigest() != "7d6ab93904d824b3248c3529132b911fbf769f8cc7d2000a4dbea313f7de5868":
        raise ValueError("Unreviewed Scene post-fs source")
    postfs = postfs.decode("utf-8").replace("\r\n", "\n")
    postfs = replace_once(postfs,
        "# This device-specific filesystem startup entry deliberately performs no work.\n# The unchanged service.sh retains the existing swap policy and safeguards.",
        "# Initialize the verified native ZRAM profile before app memory grows.\n# service.sh retains the remaining swap policy and safeguards.")
    postfs = replace_once(postfs, "exit 0\n", 'sh "${0%/*}/scripts/early_native_writeback.sh"\nexit 0\n')
    (output.parent / "post-fs-data.sh").write_text(postfs, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output)
