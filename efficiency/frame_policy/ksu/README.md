# Native frame policy configuration module

This ARM64 module packages a small root client and a WebUI. It adds no resident
process. It is specific to PLQ110 and the pinned libgui hash in `defaults/seed.json`.
First installation is dormant; an untouched installation does not disable an
existing manual test session. The saved configuration is applied once after the
boot-completed property changes. WebUI saves apply immediately and verify the
kernel's returned status.

The editor supports CPU/GPU/observation switches, all four Scene presets,
global and per-game FPS grades, game CPU bounds, and the imported static CPU
relationships. `sceneApps` maps exact packages to a mode string, `ignore`, or a
`{mode, category}` object. Categories include `normal`, `Camera` and `ELP`, with
Reader/Music/IM aliases. The numeric library contains no personal package list or
Scene preference file. Tables are encoded on save and loaded once at boot;
foreground events choose their profile inside the kernel.

Setting `games["com.tencent.tmgp.sgame"]` to `[120]`, for example,
qualifies only a 120 FPS submission cadence for that game's frame control; it
does not change the game's graphics options or force the application to render
120 FPS. Arrays such as `[30,45,60,90,120,130,144,165]` allow cadence selection.

Older ABI4 kernels without the optional game table receive global FPS settings
only; status reports this limitation. GPU activation requires the kernel's
`gpu_file_credentials` capability. Fast and per-app Scene settings require the
`scene_table` capability; unsupported kernels reject those settings before any
control changes. Basic presets can fall back to their global CPU/GPU bounds.
The module does not enable Scene's original
userspace performance scheduler or core_ctl.

FAS mode is independent from Scene: `fasMode` accepts `powersave`, `balance`,
`performance`, or `fast`. Optional `fasMarginFps` is a number or an advanced
object such as `{"base":1,"120":0.5,"165":0.3}`. Without an explicit margin the
four mode bases are 3, 1, 0.3 and 0 FPS. Numbers have at most three decimal places;
the base is at most 30 FPS, and up to eight per-FPS overrides are accepted.
Activation requires the exact `fas_tuning` kernel capability. The read-only
`framectl check-fas` validates capability and the 80-byte tuning before either
controller is stopped. `load-fas` issues the actual tuning ioctl before the main
configuration is enabled. A built blob or passing host test is not evidence
that this new controller has been flashed or accepted on a physical device.

Settings use FPS2 envelopes with a separate FAS blob. FPS1 envelopes and older
settings without FAS keys remain readable and gain balance/1 FPS tuning only.
Existing GPU switches, Scene selections and app/game rules are preserved.
Old immutable generations without `fas.bin` use the module's balance default;
rollback copies it into a new generation, never into the old directory.

To adopt an existing private settings file, use the production encoder directly
instead of replacing it with factory settings:

```sh
node efficiency/frame_policy/ksu/encode_settings.js \
  --settings /private/current-settings.json --output /private/settings-fps2.bin
```

The encoder writes a new private envelope and does not contact a device or
change its input. Pass the envelope to the existing `config.sh save HASH` stdin
transaction, using the fresh hash from `config.sh read`, or paste that same JSON
in WebUI and save. Optional `--seed` and `--scene-seed` accept matching private
seed files. An existing GPU=true/Scene=powersave selection remains that way;
only a missing FAS mode gains balance. Never upload private settings to source.

Before activation the client uses a pidfd to request graceful exit of the
installed `fas-eventd` supervisor and waits for its exit event. It does not
change the legacy module's persistent pause preference. A same-boot ownership
record permits restoration on disable/uninstall, after native control has been
removed. The client records boot/PID/starttime durably before SIGTERM and keeps
pending handoffs visible until both the supervisor identity and worker journals
confirm cleanup. An unconfirmed legacy cleanup refuses activation. Other external
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
flock; each immutable generation records its parent, while `current` is the sole
atomic commit point. A failed load or commit restores the previous running
configuration; failure to restore removes native requests before attempting a
confirmed legacy restoration. Boot reads the selected generation after acquiring
the same lock. The WebUI reads
status on user actions and has no refresh timer.
