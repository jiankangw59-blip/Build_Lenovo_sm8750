# PLQ110 / 6.6.89 Crystal candidate

This is an isolated replacement of the accepted native ZRAM data plane. It
retains the Kcompressd patch and all baseline OEM ABI checks. The source import
is pinned to `Wcoom/oneplus_sm8750_kernel` commit
`13d291aef3d1d9607ad3fa5ff30dc23b2888a3da`; `source-manifest.json` records each
original file digest. Original license and copyright notices remain intact.

`integrate_crystal.py` imports only the Crystal driver, ZMS, and required
headers. It does not import the proprietary framework, module overlay,
hardware overrides, sidecar allocator, or symbol resolver. The charging gate
uses the public battery power-supply API, accepting CHARGING/FULL and declining
on errors or unknown state. This is intentionally narrower than an external
power-connected test. Periodic GC remains periodic but uses a deferrable timer:
it will not itself wake a fully idle CPU, and can still run after other activity.
Dirty-object flush and readback work retain their original scheduling behavior.

The first build uses the existing Crypto API **LZ4KD**, with its primary codec
locked. SDDC, private delta codec, LZ4KDS, secondary recompression, size pinning,
legacy no-op APIs and default ERM policy override are disabled. A compiler guard
also rejects the SDDC options. The source-level LZ4KDS experiment is separate
under `../lz4kds_lab`; its tests do not enable this codec on the phone.

Build checks must include every Crystal object, original imported-symbol CRCs,
core BTF layouts, the final configuration, and boot kernel readback. A successful
source import or build is not runtime acceptance.

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
