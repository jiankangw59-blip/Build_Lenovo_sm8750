# Standalone display policy: integration status

The pure C policy helper follows the inspected Scene build's global/per-app
selection, battery cap, brightness threshold, game range merge and absolute
inactive deadlines. Its tests use real mode IDs: high=0/120 Hz, middle=2/90 Hz,
low=1/60 Hz, peak=8/185 Hz. Mode IDs are not refresh rates. Unknown Video
optimization is explicitly unsupported rather than silently approximated.

`scene_refresh_display.c` discovers the current physical display's public
SurfaceFlinger catalog, filters the current resolution, validates complete
records and atomically replaces owned snapshots. The Android backend uses the
legacy `android.ui.ISurfaceComposer` service; transaction 1035 carries the mode
ID and physical display ID. This matches the documented platform backdoor and
the inspected Scene direct path. An ACK is not physical confirmation.

The read-only `sfrefreshctl probe` was run on the target phone. The vendor Binder
path rejected a transferred memfd with `STATUS_FAILED_TRANSACTION`; a pipe with
a concurrent, temporary dump writer succeeds without disk IO or idle polling.
The writer exits after that one dump. This is a probe implementation, not a
new resident daemon or a completed production display controller.

Do not call synchronous dumps on the affinity watcher's event loop. Before
integration, add bounded worker/helper failure handling, notification of faults,
generation invalidation on display changes, and explicit ownership/restoration
of display overrides. Filtering a snapshot does not prevent a stale catalog
from racing a later resolution change. LTPO/ramp flags likewise describe
execution requirements, not an implemented backend. No display-setting request
has been exercised or enabled by this addition.
