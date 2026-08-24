# ReXGlue runtime patches

Project8Recomp uses ReXGlue SDK `nightly-20260722-3eb9b511` (`v0.9.0`) plus the
patches in this directory. They are required release input, not optional local
tuning: they include correctness fixes, the Windows pacing mitigation, the
Steam Deck performance work, and the instrumentation used to verify it.

`series` is the machine-readable apply order. It is intentionally not filename
order. Apply from a pristine checkout:

```sh
git checkout nightly-20260722-3eb9b511
while IFS= read -r patch; do
  git apply "/path/to/Project8Recomp/patches/rexglue-sdk/$patch"
done < /path/to/Project8Recomp/patches/rexglue-sdk/series
```

The build must refuse if a patch does not apply. Do not skip it and do not edit
the SDK tree until the entire series is present.

## Release-relevant groups

- `clock.patch`, `graphics_system.patch`, `mmio_handler.patch`: the original
  host-runtime fixes carried by v0.1.0.
- `0014`: corrects Windows timer resolution and vblank delivery. It is the
  direct mitigation for the high-CPU/low-frame-rate Windows report; Proton and
  Windows testing are still required because the original report came from
  real Windows hardware.
- `0015`, `0017`-`0020`, `0027`: bound or block polling paths that otherwise
  burn CPU while waiting.
- `0024`-`0026`, `0029`-`0034`, `0036`: the accepted graphics residency and
  reuse work used by the current performance preset. `0033` and `0035` add
  default-off draw-census capture used to verify renderer changes; neither is
  active during normal play.
- `0028`: developer-console launch commands used by repeatable smoke fixtures.
- `0037`: removes build-machine source and binary directory prefixes from
  diagnostic strings that remain in stripped release binaries.
- `0038`: uses clang-cl's `/W4` warning level instead of its pathological
  `-Wall`/`/Wall` interpretation during Windows cross-builds.

Every new patch must be listed exactly once in `series`.
`tools/check_patch_series.sh` and CI check both directions: an unlisted patch
and a missing listed patch are failures. Release preparation additionally
applies the complete series to the pristine SDK revision above and refuses any
hunk that does not apply.
