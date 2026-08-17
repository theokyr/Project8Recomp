# Recompiler config

Committed, reviewable text. These files contain guest addresses, sizes and
register indices only - metadata, not code - so they are clean-room safe.

Contents: `thps_p8_manifest.toml`, the rexglue codegen manifest that REACHES
IN-GAME - 178 forced function starts, each with a discovery method, plus the
register-localization flags kept off per `../notes/disproved.md` D14. It is a
REPLACE_ME template; `make codegen` renders `*.local.toml` (gitignored) with
this machine's absolute paths and runs codegen from `../` so rexglue's
CWD-relative template lands in `../generated/`.

## Dialect

`rexglue`, following ADR 0003
(`docs/decisions/0003-thps8-recompilation-toolchain.md`). Recorded in
`../recomp.yml` under `config.dialect`.

This is not cosmetic. The two surveyed dialects put their keys in mutually
exclusive places - one under `[main]`, the other at the top level - so a file
written in the wrong shape is not a file with a few wrong keys, it is a file
the loader reads nothing from. `tools/recomp-lint` reports that as
`DIALECT-UNKNOWN` rather than letting it exit 0.

## Still to author

- **Switch-table modeling.** The manifest currently forces several
  mid-function jump-table targets as function starts. That boots, but it is
  the prime suspect for why every register-localization flag group crashes
  (D14); converting those entries to the codegen's switch-table support is
  the named unblock path for the gameplay CPU-speedup work.

The schema note stands: the SDK's config format changed incompatibly once
(flat single-binary form to manifest+config split). Take the shape from the
pinned build, and mind the `[entrypoint.functions]` nesting trap documented
in the manifest header.

## Values to carry across

Measured, and settled enough to write down before the file exists.

| Property | Value |
| --- | --- |
| Image base | `0x82000000` |
| Image size | `0x00A80000` |
| Code base | `0x82090000` |
| Code size | `0x0063D284` (combined span; **not** `0x0062CA64`) |
| Entry point | `0x823AC158` |

The eight ABI anchor addresses are in `../recomp.yml` under `abi_anchors`.
The chosen toolchain detects them by signature rather than requiring them, but
record them anyway: an anchor a detector gets wrong is otherwise undetectable,
and `platforms/xbox360/signatures/README.md` documents the three-step trust
test that catches it.

`setjmp` is `third-party-claim`, corroborated by a real unwind boundary at the
claimed address. `longjmp` is an unverified claim. Neither should be written
into a config as if measured; if the title turns out not to use them, omit both
properties rather than guessing.

## Rules for this directory

- Every forced function entry must carry its discovery method and confidence
  tier. An address list without provenance is not reviewable.
- Never copy addresses in from a third-party config without re-deriving them.
  Leads live in `../facts/upstream-p8-config-claims.md` and the full working
  record is `../analysis/address-ledger.tsv`. An address is promoted into a
  config only once it carries `evidence: observed` and a size.
- A generated switch-table file that has been hand-edited must say so in a
  header comment, so a regeneration does not silently discard the edits.
- Lint before generating, not after:

  ```sh
  python3 tools/recomp-lint/src/lint_recomp_config.py \
    games/tony-hawks-project-8/recomp/recomp.yml \
    games/tony-hawks-project-8/recomp/config/<config>.toml \
    --image games/tony-hawks-project-8/sources/install-media/derived/default.pe.bin
  ```

  Pass `--image`. Without it the linter range-checks against a declared span,
  which is a claim about the image rather than a measurement of it, and it
  says so.
