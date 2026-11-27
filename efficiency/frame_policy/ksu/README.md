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
Optional `fallbackFps` independently selects FPS grades for games without an
explicit `games` override, including relation-only entries imported from the
seed. Omit it to inherit `fps`, preserving older settings. This can retain a
six-grade global configuration while fallback and individual games use eight
grades; neither list silently overwrites the other.
Advanced imports may also retain `sceneBaseMode`, which selects the legacy base
configuration independently from the active `sceneMode` table. If omitted, the
old mapping is unchanged. A differing base/table selection requires native
Scene table support and is preserved through GUI edits; it adds no ordinary
panel control. This retains an existing balance base plus a powersave table
without silently rewriting either configuration.

Older ABI4 kernels without the optional game table receive global FPS settings
only; status reports this limitation. GPU activation requires the kernel's
`gpu_file_credentials` capability. Fast and per-app Scene settings require the
`scene_table` capability; unsupported kernels reject those settings before any
control changes. Basic presets can fall back to their global CPU/GPU bounds.
The module does not enable Scene's original
userspace performance scheduler or core_ctl.

Open KernelSU Manager → Modules → 原生帧调度配置 → WebUI → 游戏 FAS 模式.
Choose a mode and use 保存并应用. Each mode preserves its own base and overrides
when switching modes. Use 恢复当前档默认值 to clear the selected mode's settings,
or leave its base blank and its overrides set to `{}`. The default bases for
`powersave`, `balance`, `performance` and `fast` are 3, 1, 0.3 and 0 FPS.
Adding overrides with a blank base stores that mode's default base with its
overrides. A matching FPS override supplies an absolute margin and takes
precedence over the base.

`fasMode` selects the active FAS profile independently of Scene and defaults
to `balance`. Each entry in `fasProfiles` can be `null` for the mode default,
a number for a custom base, or an object such as
`{"base":1,"120":0.5,"165":0.3}`. Margins have up to three decimal places, the
base is at most 30 FPS, and each mode accepts up to eight per-FPS overrides.
An existing `fasMarginFps` is assigned to the mode selected in the legacy
configuration when the user edits or switches modes. Other modes start at
their defaults. The active profile uses the existing 80-byte kernel format
when its adaptive limit defaults to the value provided by the kernel.

With `fas_mode_strength_v1`, the base is used directly from 60 FPS up and scales
by FPS / 60 below 60. The four modes allow up to 3, 0.5, 0.2 and 0 FPS of extra
adaptive reduction. At 165 FPS the default internal targets range from
159–162, 163.5–164, 164.5–164.7 and 165 FPS, respectively. Actual frame rate
depends on the game, workload and thermal limits. Earlier FAS-capable kernels
scale the base by FPS / 60 at every grade and allow up to 3 FPS of extra adaptive
reduction. The WebUI displays the running mode separately from the draft preview.
The preview starts at 165 FPS. Changing it only recalculates the table and
leaves global and per-game FPS targets unchanged.

Activation requires the exact `fas_tuning` kernel capability. The read-only
`framectl check-fas` validates capability and the tuning data before either
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

Optional `sceneAux` defaults to false and never changes existing CPU/GPU, Scene,
FAS, fallback-FPS or package choices. Explicit activation uses an independent
typed auxiliary selector library; frequency-profile deduplication is not reused
for auxiliary states. `check-aux` validates the kernel capability and every
required fixed sysfs resource before legacy/native ownership is changed.
The loader uses a 24-byte pointer ioctl request; file descriptors and the current
Scene-table epoch are filled only in memory, never in saved generation files.

Aux-enabled saves use FPS3 with a separate typed aux body; saves without aux
retain FPS2 compatibility. New clients read FPS1/FPS2/FPS3. Each new generation
adds `aux.bin` and `aux-required`; an old immutable generation missing these
explicitly selects an empty auxiliary policy. Boot and rollback never inherit
the previous generation's auxiliary policy or rewrite the old directory.
All loads remain under the existing transaction lock, in this order: disable,
Scene, aux/clear, game rules, FAS tuning, main configuration. Initial `aux_error`
is checked as well as the base error before committing the new generation.

The CPU auxiliary format carries original additive offsets in kHz, separate
from the unchanged legacy frequency table's percent fields. Selection resolves
raw mode/category/state calls before deduplicating complete records: Camera
NONE cannot inherit a normal app's limiter, and off retains its separate
frequency policy while auxiliary records are empty. Global options carry the
original source and known configuration/default provenance; panel peak is the
physical capability, not the current application's refresh rate. A missing
stat_source uses the independently verified original default, not an asserted
explicit user preference. No personal package inventory enters the numeric seed.

Nonempty aux preflight requires the exact kernel aux ABI and table byte size.
Feedback/options additionally require aux_feedback_supported=1; an older
direct-only prototype cannot silently replace the complete controller. Legacy
FPS1/FPS2 clears remain compatible with a kernel that has no aux capability.

`sceneGpuLock` is an optional independent boolean override under `sceneAux`.
Without an override, the imported numeric seed supplies the audited setting:
this device explicitly enables it, distinct from Sun's original missing-key
default false. The UI can disable this GPU helper while retaining CPU auxiliary
policy. It adjusts only KGSL minimum/default startup levels; normal loaded GPU
scaling and thermal/maximum constraints remain active. It does not write
devfreq/min_freq or reinterpret mod_percent as a substitute. Preflight only
reads the fixed KGSL topology and verifies writable attributes, while the
kernel owns the independent lease and conditional restoration.

### Per-mode adaptive downshift limit

`fasUtilLimits` optionally maps `powersave`, `balance`, `performance`, and
`fast` to a limit in FPS (0 through 3, at most three decimal places). Missing
or null entries retain the original 3 FPS default. Zero disables only the
utilization-driven offset; base/per-FPS margins still apply separately and
retain the current kernel's original frame-rate scaling.

An explicit selected limit uses an 88-byte FAS body in the existing FPS2/FPS3
envelope. The first 80 bytes remain ABI1; the final words are the limit in
thousandths of an FPS and a zero reserved word. A separate `0x86` ioctl requires
the exact `fas_util_limit_v1` feature. The old 80-byte ioctl restores the default
limit so boot/rollback cannot inherit another generation's custom limit.
All four banks are validated even when inactive. Unsupported kernels reject
an explicit selected limit on save or rollback before disabling or stopping
either controller, including when committing a disabled generation. A disabled
generation with the legacy 80-byte tuning can still be committed without native
kernel support. When scheduling is enabled, the transaction checks
`fas_util_limit_mfps` in the loaded kernel status before committing. A mismatch
recovers the previous generation. A lower hot-applied bound clamps
the existing offset immediately, including a change to zero. This adds no
timer, worker, or daemon; the existing real-frame feedback events do the work.
