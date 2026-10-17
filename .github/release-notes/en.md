**What's new**

- **Fixed Crash in 3D Missions (SRT Walker Overrun)**: Fixed a buffer boundary calculation bug and instruction decode length in the SRT walker signal handler (`flatten_extended_userdata_pass.cpp`) that caused an unhandled access violation when entering the 3D game world.
- **Fixed Window Visibility & Display**: Explicitly show and bring the window to the foreground on SDL creation, overriding inherited `SW_HIDE` states.
- **Fixed Single-Queue Readback Crash**: Gated readback queue registration on `IsAvailable()` to prevent null queue dereference crashes on single-queue devices.
- **Fixed Wave64 Atomic Uniformity & LDS Staleness**: Corrected lane-uniform classification for atomics to prevent divergent loop hangs, and cleared stale LDS validity in `ReadFirstLane`.
- **Fixed Multithreaded Buffer Cache Race**: Replaced unsynchronized `RegionBits` reads/writes with atomic load/store operations.
- **Fixed Async Compute Queue Index Collision**: Prevented `async_queue` and `compute_queue` from colliding on queue index 1 when both modes are enabled.
- **Fixed Pipeline Concurrency**: Synchronized `Handle()` and `SwapInOptimized()` to prevent pipeline handle races across worker threads.
- **Fixed CacheStorage & Signal Handling**: Resolved directory iteration deletion crashes on empty caches and properly filtered C++ exceptions (`0xE06D7363`) in the Windows signal handler.
