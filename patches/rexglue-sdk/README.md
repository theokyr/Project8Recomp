# rexglue-sdk runtime patches

Four families of patches against **rexglue-sdk `nightly-20260722-3eb9b511`
(0.9.0-dev)**: three general host-runtime perf fixes, the numbered
`000N-*` series belonging to the native-renderer project
(`../../notes/native-renderer/design.md`), and `0011`/`0012`, which belong to
the save-load investigation and are unrelated to the renderer. `0013` adds the
native macOS arm64 host port and the SPIR-V fixes required by MoltenVK. Apply
the perf fixes first; the numbered series applies on top of them, in order.

**Apply order is not numeric order.** Each patch is generated against a
pre-copy of the tree as it stood before that patch's edits, so the order that
round-trips is the order the pre-copies were taken in:

```
clock.patch  graphics_system.patch  mmio_handler.patch
0004  0005  0006  0009  0008  0010  0011  0012  0013
```

`0008` and `0010` were both cut from a tree that already had `0009` applied and
so must follow it. Nothing forces them apart in practice - `0009`'s three files
(`include/rex/perf/counter.h`, `src/core/perf/counter.cpp`,
`src/system/xmemory.cpp`) are disjoint from both - but the order above is the
one verified byte-identical to the live tree and is the one to use. `0011`'s three
files (`src/system/xfile.cpp`, `src/kernel/xam/xam_content.cpp`,
`src/system/kernel_state.cpp`) are disjoint from every other patch in the
series, so its position is free; last is simply where it was cut. `0012` shares
`src/system/xfile.cpp` with `0011` and was cut from a tree that had `0011`
applied, so it must follow it; its other file
(`src/kernel/xboxkrnl/xboxkrnl_io.cpp`) is touched by nothing else.

`0011` carries one behavioral fix alongside its debug-level enumeration
readout: `CompleteOverlappedDeferredEx` now resets the guest's completion
event before queueing deferred work (Win32 overlapped semantics: a call that
returns pending leaves the event non-signalled). Without it, a signal left
over from a previous immediate completion on the same XOVERLAPPED made the
guest's wait return instantly, reading `X_ERROR_IO_PENDING` as the
operation's result - in this title, the save-content enumeration "failed"
that way and LOAD GAME showed NO SAVES despite healthy enumeration and
mount. Upstream-relevant (any title using overlapped XamContent calls).

## Host-runtime perf fixes

These three take this title's Free Skate gameplay from **24.5 fps to ~97 fps**
(4x; a 3-minute soak past level-load warm-up holds a **91.5 fps floor with
zero seconds below 60**, uncapped, RTX 3060 + 5800X3D).

They are host-runtime fixes, not game-specific hacks - any Xenia-lineage
recomp on Linux should benefit. Upstream-ready; see
`../../notes/upstream-rexglue-as-local-bug.md` for the reporting context and
`../../notes/perf-attribution.md` for the measurements that motivated them.

| Patch | Fixes | Measured |
| --- | --- | --- |
| `mmio_handler.patch` | **P0** - every GPU write-watch fault called `memory::QueryProtect`, which on Linux opens and linearly parses `/proc/self/maps`. Watch churn fragments the 4 GB guest mapping into thousands of VMAs, so this ran per guest store fault. Reordered rather than removed: the watch-owner callback (which consults in-memory page tables) runs first, and the racing-unwatch recheck now runs only when that callback declines the fault - i.e. only on the path that would otherwise deliver a fatal signal. Hot path never touches `/proc`; the race is still detected. | the dominant share of the ~37% of guest-thread time that was in the fault path |
| `clock.patch` | **P4** - `Clock::QueryGuestTickCount` (every recompiled `mftb`; this game's profiler hammers it) took a shared `std::mutex`, did a 64-bit divide and read-modify-wrote two shared globals. Replaced with a seqlock-published linear scale (`guest = base + ((host-base) * mul128) >> 64`), falling back to the old path before first publish. | the ~15% of guest-thread time in timebase emulation |
| `graphics_system.patch` | Vblank worker polled on a 1 ms sleep loop, making every vblank up to 1 ms late and delivering oversleep as bursts of back-to-back interrupts. Now sleeps to the absolute next deadline and marks at most one vblank per wake, with resync if it falls far behind. | pacing regularity (not a throughput win on its own) |

## Native-renderer series

Project-specific, not upstream-ready, and gated so that with every new cvar at
its default the runtime behaves as it did without them.

| Patch | Adds |
| --- | --- |
| `0004-native-residency-telemetry.patch` | The `gpu_native_*` cvars, twelve `CounterId` entries that flow into `frames.csv`, legacy-path instrumentation (`MakeRangeValid` calls, watch re-arms, vertex-versus-texture `RequestRange` byte split, index request bytes), the per-frame command-processor drain timer, the draw suppression oracle, the resolve-destination log, and the Vulkan mirror of the D3D12 `Translation::Dump` call. |
| `0005-native-index-residency.patch` | The `NativeResidency` class (`src/graphics/vulkan/native_residency.{h,cc}`, new files) and the native index path behind `gpu_native_residency` + `gpu_native_index`. A new `PrimitiveProcessor::TryMakeIndexBufferNativelyResident` hook lets the Vulkan backend satisfy a plain `kGuestDMA` index buffer out of the per-frame converted-index upload pool instead of `SharedMemory::RequestRange`, which is what arms the write watch. The copy is byte-identical (the index endian swap happens in the vertex shader, so swapping here would swap twice) and the result is reported as `kHostConverted`, which the draw tail already treats identically for endianness, primitive reset and shader flags. Per-draw refusal falls back to the untouched emulated path and counts `native_fallback_draws`; a draw is never dropped. |
| `0006-native-vertex-residency.patch` | The vertex half, behind `gpu_native_residency` + `gpu_native_vertex`. `SharedMemory::UploadRangeUnwatched` (plus the `UploadRangeUnwatchedImpl` backend virtual and its Vulkan implementation) writes guest bytes into the shared-memory buffer at their guest offsets with the same barrier and staging discipline as `UploadRanges`, but never calls `MakeRangeValid`, so no page validity changes and no watch is armed - which is what makes mixing the two paths safe in either order. `NativeResidency::RequestRangeNative` decides per range: epoch hit, resolve-destination guard, XXH3 fingerprint match, or copy-and-restamp, with `gpu_native_dedup` off forcing a copy per request, `gpu_native_hash_max_bytes` capping what the path will own, and an eviction sweep every 64 frames. `gpu_native_verify` re-hashes every hit, bypasses the caller's fetch-constant cache so intra-frame rewrites cannot hide behind it, warns, and blacklists the range. Three consecutive frames above a 50% fallback ratio clear `gpu_native_residency` with a `REXGPU_WARN`; setting it again re-arms. Also folds in the converted-index cache's watch re-arm (~500/frame, the Process-side mprotect residual 0005 did not touch) via two new `PrimitiveProcessor` virtuals, `MayFingerprintConversionSource` / `IsConversionSourceUnchanged`: the conversion already lives in host per-frame storage, so the cache's guest source is fingerprinted instead of watched, with the cache's own per-frame clear as the trust boundary. Counters `native_draws` / `native_fallback_draws` are now totalled once per draw rather than once per range. **Page write sequencing (2026-07-31, the M3.4 corruption fix):** a fingerprint entry claims something about the *buffer*, and its key `(start, length)` does not own the bytes - an overlapping window with a different key, or the emulated path's page-granular `UploadRanges` for a texture sharing a 4 KiB page, rewrites bytes inside the window without touching guest memory, so every guest-side check (epoch, XXH3, and the verify oracle, which re-hashes guest bytes too) is blind to it and the window keeps being served spliced. `SharedMemory` now stamps every system page it writes with a monotonically increasing sequence (`StampWrittenPages`, called from `RequestRanges`, `UploadRangeUnwatched`, `RangeWrittenByGpu` and `ClearCache`), `UploadRangeUnwatched` hands its sequence back, and `RequestRangeNative` demotes a hit to a re-upload on **both** hit paths - the same-epoch fast path and the hash-match path - when `MaxWriteSeq(start, length) > entry.upload_seq`, counting `native_seq_evictions`. Also in this patch: the `gpu_native_stats` printer read `kDrawCalls`, which only the D3D12 backend increments, so it printed `draws 0` for every Vulkan run - it now reads `kNativeDraws` (and every key in its `key=value` tail matches its frames.csv column name), and the Vulkan command processor gained the `PROFILE_DRAW_CALL()` / `PROFILE_VERTICES()` call sites D3D12 already had, so the debug overlay's backend-agnostic draw counter works on Vulkan too. `src/graphics/flags.cpp` therefore appears in both 0004 and 0006 (0004 adds the printer, 0006 fixes it), the same way `include/rex/perf/counter.h` appears in both 0004 and 0009. |
| `0009-stale-protect-lognoise.patch` | Log-noise fix for the section below: in `Memory::AccessViolationCallback` the direct already-writable hit - the benign racing-unwatch case, 100% of observed events - increments the new `stale_protect_recoveries` counter and logs at debug instead of warn. The deeper genuine-staleness fallbacks keep the warning. Behavior is otherwise identical, so the defaults-off rule does not apply. Also carries `kNativeSeqEvictions` (counter files live in this patch by the M3 build-gate's split, even though the counter belongs to 0006's fix - so 0006 does not build without 0009), plus the append-only comment block that lists every consumer to re-check on an enum change. |
| `0008-cp-parse-fastpath.patch` | M5.1. A cvar-gated slimming of the command-processor parse loop, all of it behind the `gpu_cp_fastpath` master switch (default false), with three sub-levers that are ANDed with it and latched once per `ExecutePrimaryBuffer` batch so the mode cannot change under a half-parsed packet. `gpu_cp_fastpath_reg_info` answers `WriteRegister`'s unknown-register check from a bitmap instead of `RegisterFile::GetRegisterInfo`, a 3434-case switch jumped into on every register write. `gpu_cp_fastpath_bulk_regs` splits a register run at every register whose write has a side effect and stores the rest straight into the register file (`copy_and_swap` for runs of 4+), preserving order exactly: a side-effecting register still runs its `WriteRegister` with every preceding register stored and every following one not. The "plain" bitmap is derived from the actual branch sets of all three `WriteRegister` implementations, not guessed - base excludes `SCRATCH_REG0..7`, `COHER_STATUS_HOST` and the four `DC_LUT_*` gamma-ramp registers; the Vulkan and D3D12 overrides (identical sets) exclude `SHADER_CONSTANT_000_X..511_W`, `SHADER_CONSTANT_BOOL_000_031..LOOP_31` and `SHADER_CONSTANT_FETCH_00_0..31_5`. Over-marking a register non-plain is safe, under-marking is not, so unknown registers are excluded from `plain` as well - they still owe the debug log. `gpu_cp_fastpath_loop` answers "is a trace open" inline instead of through two cross-TU calls per packet. `gpu_cp_fastpath_stats` is independent of the master and off by default: a 1 Hz packet/register mix line that instruments the stock path and the fast path alike. The bulk path in `ExecutePacketType0` calls `CommandProcessor::WriteRegistersFromMem` **non-virtually**, which still routes every side-effecting register through the virtual `WriteRegister`, so backend overrides see exactly the writes they saw before; the `write_one_reg` shape is excluded from it, since that rewrites one register `count` times and every write has to keep its side effects. **One semantic delta to know about:** the stock per-register store is `volatile` (for the `WAIT_REG_MEM` poll loop) and the bulk store is not. Both run on the command-processor thread and the poll side still reads volatile, so this is a compiler-visibility question rather than a correctness one - but it is the reason the lever is default-off and wants the parity re-pass before it is recommended on. |
| `0010-fingerprint-cost.patch` | M5.3. `gpu_native_hash_min_bytes` (uint32, default 0): a native vertex range shorter than this is made resident by copying it every frame with **no fingerprint** - still epoch-deduped within a frame, still invalidated by the buffer's page write sequence, still refused over a resolve destination, only the cross-frame "have these bytes changed" compare is dropped. Correct by construction: the bytes copied are the same bytes the emulated path would have uploaded, so no setting can introduce staleness. Default 0 = no range qualifies = byte-identical to the behaviour before the cvar existed; setting it above `gpu_native_hash_max_bytes` makes every range this path owns copy-always. Motivated by the Phase B idle measurement, where hashing 3.16-3.67 MB/frame prevented only 0.74-0.96 MB/frame of uploads (24-26% bytes hit rate) because the overlap-invalidation fix forces the upload on most windows every frame regardless of the compare. The threshold is latched once per frame in `LatchFrameModeAtSwap` alongside the others and is **forced to 0 whenever `gpu_native_verify` is set**, so the oracle always has fingerprints to compare and the verify soak keeps its full sensitivity at any setting. `Entry` gains a `bool hash_valid` rather than encoding validity as `hash == 0`, which XXH3 can legitimately return; an entry born unhashed that later meets the threshold is re-hashed and re-uploaded rather than compared against a field that was never filled in. `MayFingerprintConversionSource` / `IsConversionSourceUnchanged` are deliberately **out of scope** and keep hashing at every threshold - their alternative is re-arming the conversion-cache watch (mprotect returns), not a copy - so conversion-source hashing (~0.33 MB/frame) is the accepted residual. No `CounterId` changes: the existing `native_bytes_hashed` / `native_bytes_uploaded` / `native_seq_evictions` / `cp_drain_us` columns carry the whole A/B. |

## Save-load investigation

| Patch | Adds |
| --- | --- |
| `0011-content-enumeration.patch` | **Instrumentation only - no behaviour change at any log level below `debug`, and none at all.** Every added statement is a `REXFS_DEBUG` / `REXKRNL_DEBUG`, and `REX_LOG_IMPL` tests `should_log` before it formats anything, so at the default `info` level these are a null pointer check per call and nothing else. Three call sites in `XFile::QueryDirectory` (`src/system/xfile.cpp`) log the inputs (device mount path, entry name and absolute path, search pattern, `restart`, `child_count()`, `find_index_`, buffer length) and both outcomes (the returned entry's name, index, attributes and size, or which of the two failure statuses was returned). Two in `src/kernel/xam/xam_content.cpp` log each `XCONTENT_DATA` the enumerator appends (device id, content type, file name, title id, xuid) and the result of every `xeXamContentCreate` (root name, flags, disposition, the content data it resolved, result, license mask). Written for the 2026-08-01 "storage has no save data" investigation; the read-outs are `tmp/runs/20260801-004312-saveload-m6probe-concurrent/run.log` (owner's data root) and `tmp/runs/20260801-005248-saveload-m6scratch-concurrent/run.log` (scratch copy, two saves present). Together they show the enumeration is healthy - the package mounts, all children are returned in order, and the terminating `X_STATUS_NO_MORE_FILES` is the normal end of a FindFirst/FindNext walk, not a failure. Keep the patch - it is the only window onto the guest's save enumeration, and it costs nothing when unused. |
| `0012-delete-on-close.patch` | **Behavioural fix, upstream-relevant.** Delete-on-close was recorded and never acted on: `NtSetInformationFile(FileDispositionInformation)` called `entry->SetForDeletion(true)`, and `Entry::delete_on_close()` had exactly one reference in the whole tree - its own definition. The guest's "delete this file when I close the handle" was silently dropped. This title overwrites a save by deleting the old file and recreating it, so the delete vanishing left the file in place and the game re-asked `OK to overwrite the existing GAME PROGRESS file 'Skater'?` forever; the only way to save was a new slot, against a 20-slot limit. `XFile::~XFile()` now reads the entry and its path *before* `file_->Destroy()` (which deletes the vfs `File`, closing the host handle - required before the host file can be removed) and calls `entry->Delete()` after it, logging the deletion at debug and a failure at warn. Also wires up the other route to the same flag: `FILE_DELETE_ON_CLOSE` (`0x00001000`) in `NtCreateFile`'s create options, previously not in the `CreateOptions` list at all, now sets the same entry flag on a successful open. **Known limitation:** the entry is deleted when the handle carrying the flag closes, not when the *last* handle to it closes, and `Entry::Delete()` destroys the entry, so a second `File` still holding that `Entry*` would dangle. Windows defers to last-close; matching that needs an open-handle count on `Entry`, which nothing in the tree has today. Not hit by this title (one handle) and no worse than dropping the delete outright. |

## Native macOS arm64 host port

`0013-native-macos-arm64.patch` is cut after every patch above it. It adds an
AppleClang/macOS platform target, Darwin memory mappings and exception context
handling, Mach semaphores, native `.dylib` discovery, and SDL's Metal-backed
Vulkan surface. Apple Silicon's 16 KiB host pages require the same 4 KiB
physical-view compensation used by Windows and host-page-aligned protection
changes.

The patch also fixes two Vulkan shader translation assumptions exposed by
SPIRV-Cross: rectangle-list fallback now mirrors `gl_PerVertex` in an
undecorated local struct and avoids emitting a second terminator for an already
terminated branch. These changes are renderer fixes rather than macOS
conditionals, but are needed before MoltenVK can create all pipelines used by
the title.

Finally, `Clock::host_tick_frequency_platform()` is fixed to report the units
of `clock_gettime` (nanoseconds), not `clock_getres` precision. Apple Silicon
reports a 42 ns precision here; treating that as frequency made guest time run
about 2.1 times faster and caused the observed fast FMVs and slow-fast pacing.

### How `0012` was verified (2026-08-01)

The pass condition is on disk, not on screen. Fixture:
`../../tools/input-scripts/save-overwrite-loop.jsonl` (the owner's own take of
the bug), replayed against a scratch `--user_data_root` copy of the real save
set at `--log_level=debug`.

**Control, before the fix** - the owner's two runs of the same take:
`tmp/runs/20260801-032630-record/run.log:616` and
`tmp/runs/20260801-032718-replay/run.log:617` both end at
`NtSetInformationFile set deleting flag for skater-progress on close to: true`
and nothing else. `skater-progress` kept its **2026-07-30 21:00:48** mtime
through every attempt, while `skater 3/4/5-progress` - new slots made the same
night - are stamped 03:12, 03:15 and 03:17. New saves landed; overwrites never
did.

**After the fix** - `tmp/runs/20260801-033820-saveoverwrite-fix/run.log`:

```
[warning] [krnl] NtSetInformationFile set deleting flag for skater-progress on close to: true
[debug]   [fs]   Deleted '\Device\Content\6\skater-progress' on handle close (delete-on-close)
[warning] [krnl] [NtCreateFile] FAILED: path='save:\skater-progress' -> 0xc000000f
```

That third line is the game reopening the file it just deleted and correctly
being told it is gone - the expected probe, not an error. 106 ms later the
file is recreated and written: mtime **03:38:53**, content hash
`dfb70156…` -> `861c9013…`, size unchanged at 98304. The game showed
`SUCCESSFUL! / Overwrite successful` (owner-confirmed on screen), which is the
dialog the loop never reached before.

No file appears in both 0008 and 0010: 0008 owns `src/graphics/command_processor.cpp`
alone, and 0010 owns the four `gpu_native_hash_min_bytes` files. `flags.cpp` and
`flags.h` now appear in 0004, 0006 and 0010, which is the same
generated-against-a-pre-copy pattern the series already uses.

`include/rex/perf/counter.h` and `include/rex/graphics/flags.h` change in 0004;
`include/rex/graphics/primitive_processor.h`,
`include/rex/graphics/vulkan/primitive_processor.h` and
`include/rex/graphics/vulkan/command_processor.h` change in 0005;
`include/rex/graphics/primitive_processor.h`,
`include/rex/graphics/shared_memory.h`,
`include/rex/graphics/vulkan/primitive_processor.h` and
`include/rex/graphics/vulkan/shared_memory.h` change again in 0006;
`include/rex/perf/counter.h` again in 0009; and `include/rex/graphics/flags.h`
again in 0010. So the prefix's `include/` copy has to be refreshed along with
the libraries. 0008 touches no installed header - its cvars are defined in
`src/graphics/command_processor.cpp` itself, since nothing outside that
translation unit reads them.

`CounterId` is **append-only**: a new value goes directly before the `kCount`
sentinel and nowhere else, because the frames.csv column set is positional at
write time, so an insert silently renames every column after it in every CSV
already written. On any change to it, re-check all six consumers -
`include/rex/perf/counter.h` (installed; refresh the prefix `include/` copy),
`kCounterNames` and `kIsGauge` in `src/core/perf/counter.cpp` (both
`static_assert`-guarded against `kCount`), the frames.csv emitter (trailing
append only), the `gpu_native_stats` printer in `src/graphics/flags.cpp`, the
debug overlay in `src/ui/overlay/debug_overlay.cpp`, and
`../../tools/residency_report.py`, which resolves columns by header name and so
only needs a new `CounterSpec` to report the new column.

### Generating a patch in this series

Files in the numbered series overlap, so `git diff -- <file>` against the
pinned tag folds every patch into one. Each patch is generated instead by
diffing against a copy of the file taken *before* that patch's edits
(`git diff --no-index pre/<relpath> <relpath>`, headers rewritten to `a/`,
`b/` form). Verify a new patch by adding a detached worktree at the pinned
commit, applying the whole series in order, and diffing every touched file
against the live tree - they must be byte-identical.

## Applying

```sh
cd <your rexglue-sdk clone>                          # at nightly-20260722-3eb9b511
git apply /path/to/recomp/patches/rexglue-sdk/clock.patch \
  /path/to/recomp/patches/rexglue-sdk/graphics_system.patch \
  /path/to/recomp/patches/rexglue-sdk/mmio_handler.patch \
  /path/to/recomp/patches/rexglue-sdk/0004-native-residency-telemetry.patch \
  /path/to/recomp/patches/rexglue-sdk/0005-native-index-residency.patch \
  /path/to/recomp/patches/rexglue-sdk/0006-native-vertex-residency.patch \
  /path/to/recomp/patches/rexglue-sdk/0009-stale-protect-lognoise.patch \
  /path/to/recomp/patches/rexglue-sdk/0008-cp-parse-fastpath.patch \
  /path/to/recomp/patches/rexglue-sdk/0010-fingerprint-cost.patch \
  /path/to/recomp/patches/rexglue-sdk/0011-content-enumeration.patch \
  /path/to/recomp/patches/rexglue-sdk/0012-delete-on-close.patch \
  /path/to/recomp/patches/rexglue-sdk/0013-native-macos-arm64.patch
cmake --preset linux-amd64 -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF
ninja -C out/build/linux-amd64 -f build-RelWithDebInfo.ninja rexruntime rexgpu-xenos
```

On an Apple Silicon Mac with MoltenVK installed by Homebrew, configure the
patched source explicitly (the pinned upstream tree has no macOS preset):

```sh
cmake -S . -B out/build/macos-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_INSTALL_PREFIX="<your SDK prefix>-macos-arm64" \
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF
cmake --build out/build/macos-arm64 --target install
```

For Linux perf experiments, a prefix may mix the stock SDK with the two rebuilt
libraries and point the game at it
(`REXGLUE_SDK=<prefix> recomp/tools/perf_run.sh ...`);
`librexgpu-xenosrd.so` must also sit next to the executable, since the plugin
loader only searches the exe's directory. The macOS command above installs a
complete native prefix and does not use this mixed-prefix shortcut.

## Expected warning: "Recovered stale physical page protection"

`mmio_handler.patch` re-routes the racing-unwatch case (thread T2 clears a
watch while T1 is already faulted on it) from the old silent
`QueryProtect` pre-check into the stock stale-protection recovery path
(`src/system/xmemory.cpp:485-536`), which logs one warning per event before
doing a redundant `mprotect` on the already-writable page and resuming.
Newly *logged*, not newly *occurring* - investigated 2026-07-31: onset
matches the patched prefix going live to within 17 s, and 100% of sampled
events (1741 across three runs) carry the already-writable
`guest_protect 0000000B` race signature via the direct-hit branch
(`xmemory.cpp:490-492`), never the genuine-staleness fallbacks.

Expect **~150-550 warnings per 2-3 minute run at ~100 fps** (scales with
fps; fires even idling in-level from watch churn). Track the per-run count
through the native-residency milestones: ranges the native path owns are
never watched, so counts should only *fall* - a step increase after a
residency change means asymmetric watch state and possible stale GPU copies,
which this recovery would otherwise paper over.

Since `0009-stale-protect-lognoise.patch` the direct already-writable hit no
longer logs at warn level: it increments `stale_protect_recoveries`, which
lands in `frames.csv` like any other counter, and logs at debug. The canary is
unchanged, only cheaper to read - sum the column per run instead of grepping
the log, and watch for step increases rather than for the warnings themselves.
Any warning that *does* still appear now comes from the deeper
genuine-staleness branches (`xmemory.cpp:509,519,526` with the series applied),
which have never been observed in this title and should be treated as a real
finding.

## Gotcha found while testing these

Hard-killing runs leaks their guest memory: each instance maps ~340 MB into
`/dev/shm` under `xenia_memory_*` and only unlinks it on a clean shutdown.
A few dozen `kill -9`s filled a 16 GB tmpfs, after which the next launch
took **SIGBUS on its own guest memory** (mapped but unbackable) - once deep
in guest code, once inside `Memory::Initialize`. Both look exactly like a
runtime or codegen bug and cost real debugging time. `perf_run.sh` and
`make stop` now reclaim those segments; check `df -h /dev/shm` before
believing any new SIGBUS.

## Prefix hygiene incident (2026-07-31)

The prefix's `include/rex/ppc/context.h` copy was found carrying the
**rejected** CR-packing experiment (perf-attribution: "inside noise") as a
hand edit with no patch file - a leftover from the morning experiment session
that the M6 clean-checkout gate would not have reproduced. Reverted to the
stock SDK header and the app rebuilt the same day, before any M1.4 baseline
run. Rule going forward: the prefix `include/` copy must only ever differ
from the SDK source tree by hunks present in a committed patch file.

## Tested and rejected

A fourth patch - a per-block **write-watch fault cooldown** in
`SharedMemory` (blocks faulting repeatedly stop being re-watched and are
always re-uploaded) - was implemented and measured. It made things **worse**
(90.0 fps vs 97.3 fps uncapped) because the extra re-uploads cost more than
the faults it avoided, and it carries a stale-texture hazard. Not shipped.
Do not re-derive it: the fault path is already cheap once P0 lands.
