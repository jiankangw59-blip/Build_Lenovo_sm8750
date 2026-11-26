# Native frame policy configuration module

This ARM64 module packages a small root client and a WebUI. It adds no resident
process. It is specific to PLQ110 and the pinned libgui hash in `defaults/seed.json`.
First installation is dormant; an untouched installation does not disable an
existing manual test session. The saved configuration is applied once after the
boot-completed property changes. WebUI saves apply immediately and verify the
kernel's returned status.

The editor supports CPU/GPU/observation switches, one explicit Scene preset,
global and per-game FPS grades, game CPU bounds, and the imported static CPU
relationships. Setting `games["com.tencent.tmgp.sgame"]` to `[120]`, for example,
qualifies only a 120 FPS submission cadence for that game's frame control; it
does not change the game's graphics options or force the application to render
120 FPS. Arrays such as `[30,45,60,90,120,130,144,165]` allow cadence selection.

Older ABI4 kernels without the optional game table receive global FPS settings
only; status reports this limitation. GPU activation requires the kernel's
`gpu_file_credentials` capability. The module does not enable Scene's original
userspace performance scheduler, or implement FAS mode margins/core_ctl.

Before activation the client uses a pidfd to request graceful exit of the
installed `fas-eventd` supervisor and waits for its exit event. It does not
change the legacy module's persistent pause preference. A same-boot ownership
record permits restoration on disable/uninstall, after native control has been
removed. An unconfirmed legacy cleanup refuses activation. Other external
frequency controllers, including Scene performance scheduling, must remain off
during this trial.

The build workflow validates the binary protocol and packet decoder, compiles
only `framectl` with Android NDK, and packages `frame-policy-config.zip` as an
artifact. Generated default binary tables are kept outside version control:

```sh
node efficiency/frame_policy/ksu/test_protocol.js --defaults
python3 efficiency/frame_policy/ksu/package.py --controller /path/to/framectl \
  --output /path/to/frame-policy-config.zip
```

Configuration generations remain private under `/data/adb/frame-policy` and are
retained on uninstall. Concurrent editors use an expected content hash and
flock; a failed load does not advance the saved generation. The WebUI reads
status on user actions and has no refresh timer.
