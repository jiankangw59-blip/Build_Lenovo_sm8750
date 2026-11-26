# Scene display persistence support

This directory mirrors the reviewed `scene-sampling-opt-api102` production sources
and isolated test fixtures. The original project remains in the local tools
workspace; update the mirror explicitly when changing it.

`Build_scene_display_support.yml` exercises the production refresh interceptors
and compiles an **unsigned** API102 APK. It downloads the compile-only API using
a pinned SHA-256, excludes that API implementation from the APK, and never
receives the installed app's signing key, private Scene APK, preferences, or
device evidence. Sign the aligned artifact locally with the existing signer.

The exact Scene APK hash/version guard and existing sampling fixes remain in
place. The new interceptor only repairs the original quick-panel persistence
gate when full Scene CPU/GPU scheduling is disabled. The existing app exclusion,
display feature switch, rule serialization and manual RPC remain in Scene.

This APK alone does not supply the missing standalone automatic display policy.
The separate `thread_affinity/scene_refresh_policy.h` helper and read-only
`sfrefreshctl probe` are not yet wired into the foreground watcher. Display
ownership, asynchronous execution with bounded error handling, input events and
reboot acceptance must pass before claiming full automatic restoration.
