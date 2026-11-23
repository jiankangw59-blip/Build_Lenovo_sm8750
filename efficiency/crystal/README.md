# PLQ110 / 6.6.89 Crystal candidate

This is an isolated replacement of the accepted native ZRAM data plane. It
retains the Kcompressd patch and all baseline OEM ABI checks. The source import
is pinned to `Wcoom/oneplus_sm8750_kernel` commit
`13d291aef3d1d9607ad3fa5ff30dc23b2888a3da`; `source-manifest.json` records the
18 common file digests and `sddc-source-manifest.json` records 12 additional
codec/SDDC/test files. Both manifests must name the same repository and commit.
Original license and copyright notices remain intact.

`integrate_crystal.py` imports only the Crystal driver, ZMS, and required
headers. It does not import the proprietary framework, module overlay,
hardware overrides, sidecar allocator, or symbol resolver. The charging gate
uses the public battery power-supply API, accepting CHARGING/FULL and declining
on errors or unknown state. This is intentionally narrower than an external
power-connected test. Periodic GC remains periodic but uses a deferrable timer:
it will not itself wake a fully idle CPU, and can still run after other activity.
Dirty-object flush and readback work retain their original scheduling behavior.

The isolated builds and the final combined profile have distinct, checked policies:

| Profile | Build-time primary default | Primary lock | SDDC/private codec | SDDC KUnit |
| --- | --- | --- | --- | --- |
| `crystal` | `lz4kd` via existing Crypto API | yes | disabled | disabled |
| `crystal-lz4kds-test` | `lz4kd` | no | enabled for independent zramN trials | enabled |
| `crystal-lz4kds` | `lz4kds` | yes | enabled | disabled |
| `crystal-lz4kds-le9uo` | `lz4kds` | yes | enabled, with le9uo and read-only integrity stats | disabled |

The combined profile retains Kcompressd and adds the unchanged, already accepted
le9uo patch after it: `anon_min_ratio=0`, `clean_low_ratio=5`,
`clean_min_ratio=0`, and `workingset_protection=1` by default. The original
patches apply sequentially with `--fuzz=0`; no rebased replacement is used.
The three earlier isolated profiles retain their existing dependencies and
source/overlay chains. No performance or power benefit is inferred from combining
previously separate functional checks.

The test profile keeps the main device's build-time default on ordinary
compression, while allowing a private zramN to select `lz4kds`. Its ordinary
`lz4kd` backend uses the same pinned private base encoder as `lz4kds`; only the
latter exposes the delta operations and creates an SDDC manager. This supports
a comparison without mixing different base encoders on the test kernel.

All Crystal profiles disable secondary recompression, size pinning, legacy no-op
APIs and default ERM policy override. The pure profile has a compiler guard
rejecting SDDC. The explicit LZ4KDS profiles instead require SDDC, its private
codec and 4 KiB pages, while requiring **native SDDC-to-ZMS to remain disabled**.
This first trial covers resident alias/delta and flatten-to-ordinary compressed
objects packed into ZMS; it does not validate native alias/delta wire data on
disk. The charging API and deferrable GC adaptations are retained in all profiles.

The test profile precompiles the codec, manager, optional NEON object and codec
KUnit object. Its 6 codec and 26 state KUnit cases (32 total) execute only when the ARM64
kernel boots with KUnit enabled; compilation is not proof that they ran. The
test profile preserves the OEM command line and appends exactly one
`kunit.enable=1 kunit.filter_glob=crystal-sddc-*`, replacing duplicate values of
those two parameters. Precompile and boot preparation reject missing, repeated
or different values, including the earlier test build without these parameters.
The filter selects the two Crystal suites even when `KUNIT_DEFAULT_ENABLED=n`.
Other profiles do not append these parameters. Final-profile KUnit is
disabled. The source-level experiment in `../lz4kds_lab` is host-only and is not
copied into the kernel. It has not identified or repaired a demonstrated kernel
wrong-page/zero-page failure.

All SDDC-enabled profiles apply the separately reviewed
`sddc-integrity.patch` after checking the immutable source manifest. GNU patch
uses `--fuzz=0`; its digest, original source digest and exact patched digest are
pinned in the integrator and recorded in the integration report. The overlay
compares the reconstructed delta proof bytes with the original compressed
stream, and propagates a page-hash mismatch as `-EIO` through the resident,
locked-resident and native restore entry points. Two additional private KUnit
cases exercise equal-length proof corruption and real resident read error
propagation. Native SDDC-to-ZMS remains disabled in these profiles.

This is a defense against silent incorrect reads, not proof that a reported
crash's root cause has been found or repaired. The original 30 source digests
remain unchanged; `installed_sha256` records the overlaid SDDC file. Boot
preparation requires the exact overlay record and the profile-specific installed
digest for every LZ4KDS profile. The pure `crystal` profile never reads or requires this patch,
so the earlier pure-control build remains independently verifiable.

Only `crystal-lz4kds-le9uo` additionally applies `sddc-integrity-stats.patch`.
It appends the read-only `integrity_checks`, `integrity_failures` and
`integrity_skipped` counters while retaining the previous 46 `sddc_stat` fields
in order. This enables a later policy controller to observe integrity state;
it does not itself enable an automatic policy or validate that policy at runtime.

The combined source chain is checked at each stage: the original pinned files,
the integrity overlay (SDDC source `e51140d12...`), then the three-file stats
overlay (SDDC `ed2e2a521...`, private header `c35659776...`, raw driver
`997e92108...`). Charging/deferrable-GC adaptation follows those overlays, making
the final installed driver `b4931a77a...`. Source reports retain all intermediate
digests; the final installed digest is not confused with the raw stats digest.
The two earlier LZ4KDS builds continue to require `e51140d12...` as their final
SDDC source and do not read or require the stats patch or its review record.

The final LZ4KDS profile must first pass the independent-device tests, including
actual alias/delta counters, reference/generation reuse, concurrent overwrite
and discard, and packed-backing **cold** reads with known-page checksums. A
primary compressor lock rejects repeated writes of the same value as well;
userspace must read the selected algorithm before attempting to set it.

Build checks must include every Crystal object, original imported-symbol CRCs,
core BTF layouts, the final configuration, and boot kernel readback. A successful
source import or build is not runtime acceptance. The three isolated Crystal
profiles retain exactly the accepted 19 baseline patches plus Kcompressd; the
combined profile adds le9uo as item 21. Boot preparation checks
their order/digests, the profile's source inventory, default codec, primary lock
and KUnit selection before any device-side packing operation.

Runtime tests must use a new private backing file. The native Scene controller
must not be reused unchanged: Crystal decrements writeback_limit by **logical
slots**, while bd_stat reports **physical backing pages**. Measure those values
separately. A writeback can leave packed data dirty in RAM; follow writeback with
fsync/fdatasync on the block device, require ZMS dirty_pages=0, and verify an
actual backend-read increase on the first readback. Do not use global drop_caches.
Do not assume that subsequent reads are cold because clean ZMS blocks may remain
cached. Use a newly created private test device for each cold-read trial.

For comparisons, report original data bytes, compressed object bytes, actual
packed backing writes including flush/GC, physical reads, and integrity results.
No UFS write reduction, power reduction, or stability improvement is claimed
until these paths have been measured on PLQ110.
