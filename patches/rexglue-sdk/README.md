# ReXGlue v0.10.0 runtime patches

Project8Recomp uses the ReXGlue SDK `v0.10.0` tag plus the two patches in this
directory. They are required build inputs, not optional tuning. The runtime
patch carries the game's correctness fixes, Windows pacing mitigation, Steam
Deck performance paths, developer-console fixture support, and default-off
diagnostic counters. The second patch updates the bundled MoltenVK to 1.4.2 so
the Apple Silicon build presents its rendered frames correctly.

`series` is the machine-readable apply order. Apply it from a pristine SDK
checkout:

```sh
git checkout v0.10.0
while IFS= read -r patch; do
  git apply --check --index "/path/to/Project8Recomp/patches/rexglue-sdk/$patch"
  git apply --index "/path/to/Project8Recomp/patches/rexglue-sdk/$patch"
done < /path/to/Project8Recomp/patches/rexglue-sdk/series
git submodule sync --recursive
git submodule update --init --recursive
```

`--index` is required because the MoltenVK revision is a submodule gitlink. A
plain working-tree-only `git apply` does not advance the recorded commit for an
uninitialized submodule.

## What the overlay carries

- Deadline-based waits, bounded vblank catch-up, timer and ring blocking, and
  other pacing fixes. These mitigate the high CPU use and low frame rate
  reported on Windows, though Windows remains experimental until it is tested
  on more real Windows systems.
- Native residency, prefetch, index and vertex paths, plus the accepted sampler,
  texture and descriptor reuse paths used by the performance preset.
- Save diagnostics, stale-process protection, delete-on-close behavior, and the
  command-driven fixture hooks used for repeatable testing.
- Reproducible source-path mapping and a useful clang-cl warning level for
  portable release builds.
- Default-off performance counters and draw-census capture used to validate
  renderer changes. They do not run during normal play.

Every new patch must be listed exactly once in `series`.
`tools/check_patch_series.sh` checks both directions: an unlisted patch and a
missing listed patch are both failures.
