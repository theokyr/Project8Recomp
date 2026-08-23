# Recompiler configuration

This directory contains the committed, reviewable input to ReXGlue codegen.
It records guest addresses, sizes, names, and register-localisation choices;
it contains no executable or generated game code.

## Files

- `thps_p8_manifest.toml` is the distributable template for the supported 2006
  retail disc build. It includes the code spans and forced function starts the
  recompiler needs.
- `*.local.toml` files are local renders with absolute paths. They are ignored
  and must never be published.
- `generated/` is codegen output. It is a mechanical translation of the game
  executable and is ignored under the same rule as extracted disc content.

The manifest deliberately contains `REPLACE_ME` path values. Copy or render it
to a `.local.toml` file, replace those values with paths to your own extracted
`default.xex` and output directory, then run ReXGlue codegen against the local
copy. Do not put personal paths into the committed template.

## Known image values

| Property | Value |
| --- | --- |
| Image base | `0x82000000` |
| Image size | `0x00A80000` |
| Code base | `0x82090000` |
| Code size | `0x0063D284` |
| Entry point | `0x823AC158` |

The forced entries carry their discovery method in the manifest. Do not add an
address copied from another project without re-deriving it against the
supported executable. A wrong forced start can compile cleanly and fail much
later at runtime.

## Rules

- Keep absolute paths and source material out of committed files.
- Keep generated translation units out of git and release source archives.
- Preserve the manifest schema and `[entrypoint.functions]` nesting used by the
  pinned SDK; older flat configuration examples are incompatible.
- Treat register-localisation flags as correctness-sensitive. Leave a group
  off unless it has passed boot, gameplay, and save/load checks.
