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

The game CPU branch now adjusts maximum frequency as well as minimum requests.
It accumulates the target mm's actual scheduled runtime on context switches,
without periodic sampling, task-list scans or PMU counters. Matching frames can
lower a ceiling with load headroom; slow frames raise it with bounded slew.
The rendering policy and heavily used policies lock min/max to a supported OPP;
other policies retain their hardware floor. Continuous targets are kept separate
from OPP rounding so small reductions accumulate rather than getting stuck at a
frequency step. Requests start from the current frequency after warmup, not an
unconditional maximum-frequency boost. This is an independent implementation of
frame/load feedback, not exact reproduction of every FAS-RS tuning policy.

Frequency requests require a configured FPS grade to match observed cadence
over two consecutive 500 ms windows. Optional `fps_candidates` contains up to eight increasing
grades ending at `target_fps`; omission means only the explicit target is allowed.
A 60 FPS menu under a 165-only profile creates no frequency requests. Return-time
bursts do not reset a valid grade, and one unmatched window is tolerated. A grade
change, sustained unknown cadence or a long pause resets controller history; stale GPU
fences cannot feed a different grade. Observed cadence cannot distinguish an
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

The current loader explicitly selects one Scene limiter and the allowed FPS
grades for a bounded trial. Automatic per-app/mode configuration is not implemented.
Scene `core_ctl`, touch boost, CPU excludes/prefer, and the full proprietary
Scene policy are not implemented. This candidate must not be presented
as complete Scene/FAS behavior parity or as a measured power improvement.
