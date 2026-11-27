Scene frequency-policy table
============================

`scene_policy_uapi.h` defines an optional, root-only `FP_SCENE_TABLE` ioctl.
The existing `FP_CONFIG` ABI remains 592 bytes. The table is 14,384 bytes:
48-byte header, sixteen 336-byte profiles and sixty-four 140-byte package rules.
It must be allocated with `kvmalloc`/`kvzalloc`, never on the kernel stack.

Each package rule selects three profiles: active, inactive and display off.
Only exact package IDs match. Mode and category choices are compiled into these
indices, so an ordinary foreground switch does not parse JSON/XML, allocate a
table, write sysfs from a scheduler hook or wake a new daemon.

Supported behavior
------------------

* Per-package powersave/balance/performance/fast numeric frequency settings.
* CPU limiter threshold/margin steps and minimum/maximum bounds.
* Explicit `ignore` and Scene's existing `igoned` spelling: no native Scene vote.
* Camera `limiter NONE`: static, hardware-bounded CPU minima without PMU feedback.
  Removing the previous app's dynamic maximum is intentional; vendor/thermal
  requests retain priority through normal QoS aggregation.
* Normal-app GPU min/max requests and leased `mod_percent`, using the existing
  credential-aware save/write/readback/restore implementation.
* Explicit off-limiter configuration and release of our GPU percentage lease.
* When `game && !display_off`, profile selection always returns NULL. Scene's
  CPU PMU and QoS path must be released before FAS owns frame-based requests.

Integration contract
--------------------

Validate a private table copy with `fp_scene_table_valid` before publishing it
under `config_lock`. Verify CPU IDs against the current `fp_config` with
`fp_scene_topology_matches` in both loading directions. Invalid updates retain
the old table and requests. Every successful replacement increments the existing
generation and restarts the existing ordered transition work; no new timer is
needed. Free the old copy only after all consumers protected by `config_lock`
have stopped using it.

In `apply_control`, after setting foreground/game/off state and releasing old
requests, select a profile with:

```c
profile = fp_scene_select(scene_table, foreground, source_game,
                          display_off, inactive);
```

An absent optional table preserves the existing `fp_config` fallback. With a
table installed, a NULL non-game selection means explicitly no Scene requests;
it must not fall back to the old global profile. CPU bounds come from
`profile->cpus[i].limits`, GPU bounds from `profile->gpu`. Start PMU sampling only
when `fp_scene_uses_feedback(profile, cpu_count)` is true and the display is on.
Static camera/fast profiles therefore need no cycle counters or Scene sampling
hook. In `apply_scene`, use `fp_scene_profile_ceiling` for configured policies.
A zero GPU percentage releases the old lease, rather than writing zero.

The current provider supplies package/game/off events. It does not supply a
reliable active-versus-inactive interaction event. Until that is extended through
the existing provider, pass `inactive=false`; inactive profiles are an interface
and import capability, not a claim of live operation. Per-activity settings also
need an existing provider update, not a guessed package-name heuristic.

Compiler
--------

`compile_scene_policy.py` accepts the actual Scene profile, three sidecars
(`_apps.json`, `_elp.json`, `_camera.json`), existing hardware controls and explicit
settings such as:

```json
{
  "mode": "balance",
  "off_limiter": "idle",
  "packages": {
    "com.example.reader": {"mode": "powersave", "category": "Reader"},
    "com.example.camera": {"mode": "performance", "category": "Camera"},
    "com.example.overlay": "igoned"
  }
}
```

The optional `--powercfg` import requires `--known-packages` (a JSON list of actual
installed package IDs). It imports exact package overrides and filters out
activity keys, without publishing private preferences to the repository. Explicit
JSON settings take priority. The compiler reports only a skipped-entry count;
the full generated rule/report files should remain private when using personal
preferences.

Frequency output is a partial migration. The generated report explicitly lists
unported actions and limiter fields: CPU `excludes`/`prefer`, `core_ctl`, governor
target-load/hispeed/latency settings, cpusets, FPS/display control, input boosts
and vendor switches are not executed by this table format. Category labels use
the supplied mapping; package categories are not guessed from names. The off
profile is an explicit policy choice, not evidence that Scene emits inactivity.

Validation
----------

Run `python -m unittest test_scene_policy.py` for importer, boundary and layout
tests. Build/run `test_scene_policy.c` for shared kernel/helper arithmetic and
optional binary-table validation. Include `scene_policy_kunit.h` before the
existing case array and add `FP_SCENE_KUNIT_CASES,` for five isolated kernel cases.
The fifth drives the actual foreground-event worker and synthetic CPU/GPU QoS
devices through static Scene, ignored app, display off, game and Scene handoff.
Its qualified-game state models real outgoing QoS requests; Android libgui and
the physical game's rendering are outside that isolated test.
These tests do not replace testing real foreground transitions, QoS limits,
camera behavior and ownership handoff on the device.
