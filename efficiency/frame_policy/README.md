# Native frame and Scene frequency policy candidate

This is an opt-in prototype for the pinned Ace 6 6.6.118 kernel. It is disabled
until a root client loads a configuration into `/dev/frame_policy`.

The existing affinity Scene event source selects exactly one branch:

| Context | Frequency controller | Resources removed before entry |
| --- | --- | --- |
| Foreground game, display on | Frame controller | Scene PMU counters and CPU/GPU QoS requests |
| Other foreground app | Scene CPU feedback and GPU bounds | Game uprobe, fences and CPU/GPU QoS requests |
| Display off | Scene idle bounds | Game probe/fences and Scene PMU counters |
| Event source disconnected or disabled | None | All resources owned by this component |

`apply_control()` is the sole transition worker. Requests from the old branch
are removed before the next branch starts. Each event increments a generation;
queued frame/Scene work from a previous generation cannot apply its results.
CPU and GPU can be enabled independently. Thermal and other system QoS limits
remain in force.

The GPU's existing `mod_percent` parameter is imported from Scene's
`common_app` and `common_gaming` presets. The loader opens that sysfs attribute
once and passes the file into the kernel; only transition work reads/writes it.
The component snapshots the original value, retains it across screen-off, and
restores it on disable if another owner has not replaced its value. A conflicting
writer causes control to stop with `EBUSY`. No vendor-private structure offsets
are used for this setting.

The client pins this sysfs inode and temporarily supplies the owner's write bit
if Scene has made it read-only, then restores the old mode immediately after
opening it. Kernel-worker I/O uses the root-opened file's saved credentials and
restores the worker's credentials on every return path. Fatal transition errors
disable this component, unwind its requests and remain visible in status; a
configuration ioctl returns the worker's error instead of reporting success.

Foreground events only publish a bounded snapshot under a spinlock and queue
the transition. They never wait on the controller mutex, PMU reads or probe
teardown, so the existing affinity provider cannot be held up by this backend.

Game events come from a verified ARM64 `Surface::queueBuffer` uprobe restricted
to the foreground game's mm. CPU control uses successful queue intervals;
GPU control uses the acquire fence's remaining wait. These are submission and
fence measurements, not proof of displayed FPS or GPU execution time. The
controller is independently authored GPL-2.0 code, not copied FAS-RS GPLv3 code.

After warmup, each selected successful submission supplies its raw interval to
the FAS CPU controller. It starts from the maximum continuous target across CPU
policies and adds a proportional correction with gain 0.0003 kHz per normalized
nanosecond. The effective FPS is the qualified grade minus its configured margin
plus a utilization offset bounded to -3 to 0 FPS. When the long submission
window is below the grade by more than 2 FPS, the controller invalidates the
cached utilization and skips the utilization cap. Otherwise a current sample
caps the common target at the highest reported current policy frequency times
the busiest game thread's utilization divided by 0.5.

Frame work samples only the target game's threads, at most once per 300 ms.
Runtime deltas are matched by thread ID and start time, including threads that
migrate between CPUs. The eight busiest measured threads supply affinity masks.
If the masks differ, the most common mask is excluded before taking their union.
Policies intersecting that union get matching min/max votes at a supported OPP.
Other policies retain their configured floor, bounded by the hardware minimum.
The per-CPU `sched_switch` counters remain available as diagnostics.

Each event applies the configured CPU relations before saving the continuous
targets, starting from the configured game maxima within hardware limits. OPP
rounding happens when a batch is submitted to QoS, so sub-OPP reductions carry
into later events. The worker coalesces frequency writes over roughly 20 ms.
Queue overflow is fatal and reported as `fas_queue_overflow`; it removes the
component's votes and game probe. GPU feedback remains a separate fence-based
extension. With an established grade, an actual submission can arm one late
CPU recovery step after two frame budgets or 50 ms, whichever is longer. If no
new submission arrives, that step uses the larger of elapsed time and the last
real interval. Late recovery does not sample tasks, update GPU votes, or repeat
during the same stall. The existing one-second inactivity deadline then removes
the frequency votes. System thermal management remains in force. The separate
FAS-RS temperature-driven FPS offset is outside this implementation.

`compile_game_rules.py` imports static `set_extra_policy_rel` package branches
from the existing FAS extension and per-game FPS arrays from its TOML files.
It rejects unknown active statements and cyclic graphs. `framectl load-rules`
installs the bounded table once; foreground events select the matching game or
fallback rule inside the kernel. CPU dependencies are applied in order before
per-policy OPP rounding, with hardware/thermal limits retaining priority.
The table supports 32 exceptions and independent target FPS arrays, including
45/130 FPS grades. No Lua interpreter or rules daemon runs in the kernel.

```sh
python3 efficiency/frame_policy/compile_game_rules.py \
  --lua /path/to/installed-extension.lua --controls /path/to/explicit-controls.json \
  --standard-games /path/to/standard-games.toml --user-games /path/to/user-games.toml \
  --output games.bin
framectl load-rules games.bin
```

`compile_fas_tuning.py` imports the active mode's base FPS margin and optional
per-grade overrides from the FAS TOML files. `framectl load-fas TUNING.bin` loads
the 80-byte ABI 1 tuning independently of the existing configuration and rule
tables. `powersave`, `balance`, `performance`, and `fast` affect the next real
frame, preserving continuous targets and the margin captured by older queued
frames. The original extension's CPU hotplug actions are outside the importer.

Frequency requests require a configured FPS grade to match observed cadence
over two consecutive 500 ms windows. Optional `fps_candidates` contains up to eight increasing
grades ending at `target_fps`; omission means only the explicit target is allowed.
A 60 FPS menu under a 165-only profile creates no frequency requests. Return-time
bursts do not reset a valid grade, and one unmatched window is tolerated. Each
new grade or selected surface warms up with 60 intervals before voting. The
long submission window holds up to five seconds' worth at the qualified grade.
A grade change, sustained unknown cadence or a long pause clears frame history
and the effective-FPS offset. Stale GPU fences cannot feed a different grade.
Observed cadence cannot distinguish an
intentional frame cap from sustained rendering overload, so grade selection is
a bounded submission heuristic, not a recovery of a game's rendering intent.

Scene CPU feedback reads native cycle counters, scales for multiplexing, and
applies the imported margin thresholds. `sched_switch` events gate updates to
at most once per 80 ms; no periodic timer samples an idle system. Game inactivity
uses a single deadline to remove frequency floors after one second without frames.
The game accounting hook is unregistered before ordinary Scene PMU sampling
starts, and neither hook is registered when this component is disabled.

## Build and configuration

Install `efficiency/thread_affinity/install_builtin.py` first, then this directory's
`install_builtin.py`. The main build workflow has `enable_frame_policy=false` by
default. `Test_frame_policy.yml` checks host arithmetic, the ABI, and ARM64 objects
against the pinned 6.6.118 source.

```sh
python3 efficiency/frame_policy/compile_config.py \
  --scene-profile /path/to/profile.json --libgui /path/to/libgui.so \
  --controls /path/to/explicit-controls.json --observe --output observe.bin
framectl load observe.bin
framectl status
framectl disable
```

Keep `--observe` for initial on-device validation. Observation creates no
CPU/GPU frequency requests. Before enabling control, pause the old fas-rs core
and stop the old Scene CPU/GPU frequency control paths, verify their cleanup,
and retain a reversible backup. Internal branch arbitration does not disable
external controllers. No legacy controller is modified automatically by this
prototype.

Before flashing a new Crystal build, enroll its compiled notes in Scene module
2's existing allowlists. Each Image has a different identity even when only
this scheduler changes. Run `efficiency/enroll_scene_crystal.py` with the
downloaded preflash evidence, target serial, and a new backup directory. It
checks the Image against its linked ELF, the 32,921 KMI results and codec, then
preserves the previous kernel's writeback selection. It does not reset live
swap. An already-running unregistered build can use `--inherit-policy-from`
with the previously verified kernel notes SHA256. Early boot applies the
existing Scene size and compressor after reboot.

The base loader explicitly selects one Scene limiter. `compile_scene_policy.py`
can additionally import the installed profile/sidecars and explicit per-package
mode/category settings into a bounded optional table. `framectl load-scene`
installs it, and existing foreground/screen events select its numeric CPU/GPU
profile. Static camera/fast profiles do not start PMU sampling; ignored packages
release all Scene votes. Screen-off releases the GPU governor-file lease.
See `SCENE_PORT.md` for the table layout, import options and evidence limits.
The current provider has no reliable inactive-interaction or per-activity mode
event, so those states are import capabilities without a live-operation claim.
The configuration WebUI encodes the optional table from four Scene modes and
explicit per-package mode/category choices. It loads tables before enabling
requests, so it never applies a new table with old active frequency votes.
Scene `core_ctl`, touch boost, CPU excludes/prefer, and the full proprietary
Scene policy are not implemented. This candidate must not be presented
as complete Scene/FAS behavior parity or as a measured power improvement.
