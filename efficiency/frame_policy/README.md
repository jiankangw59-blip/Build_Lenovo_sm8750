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

Game events come from a verified ARM64 `Surface::queueBuffer` uprobe restricted
to the foreground game's mm. CPU control uses successful queue intervals;
GPU control uses the acquire fence's remaining wait. These are submission and
fence measurements, not proof of displayed FPS or GPU execution time. The
controller is independently authored GPL-2.0 code, not copied FAS-RS GPLv3 code.

Scene CPU feedback reads native cycle counters, scales for multiplexing, and
applies the imported margin thresholds. `sched_switch` events gate updates to
at most once per 80 ms; no periodic timer samples an idle system. Game inactivity
uses a single deadline to remove frequency floors after one second without frames.

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

The current loader explicitly selects one Scene limiter and one target FPS for
a bounded trial. Automatic per-app/mode/FPS selection is not implemented.
Scene GPU `mod_percent`, `core_ctl`, touch boost, CPU excludes/prefer, and the
full proprietary Scene policy are not implemented. GPU Scene behavior is only
the explicitly supplied frequency bounds. This candidate must not be presented
as complete Scene/FAS behavior parity or as a measured power improvement.
