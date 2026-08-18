# Agent guide

Read this before changing anything here.

## What this repository is

A static recompilation of Tony Hawk's Project 8 (Xbox 360), published as
checkpointed releases.

- **`src/`, `config/` and `patches/` are synchronised from the project's
  development tree.** Changes to them are applied there and arrive here in a
  release, so a pull request against those paths may be applied as a patch
  rather than merged as a commit. It will be credited either way. Everything
  else — the README, `docs/`, `tools/`, `.github/` and the licence files —
  belongs to this repository and is edited here directly.
- **This repo has exactly one goal:** make it easy for someone who owns a legal
  copy to end up with a ready-to-play build. It is not a research dump.

## The rule that overrides everything

**No game content is ever committed or published.** Not assets, audio, video,
textures, `default.xex`, disc images, decrypted intermediates, platform keys, or
the translation units the recompiler generates from a dump.

`tools/check_no_game_content.sh` enforces it against the file listing, not the
ignore rules. Run it before any commit that adds files. There is no allowlist,
and adding one is not a change to make casually.

If you are unsure whether something counts: it counts.

## Layout

```
src/launcher/    the GUI launcher. Links no SDK — builds anywhere, needs no dump.
src/identify/    disc identity + extraction. Links the SDK; does what the GUI must not.
src/game/        the game's host code. Needs the SDK and generated sources.
src/common/      shared between the identity worker and the game's dump gate.
config/          recompiler configuration — addresses, sizes, names.
patches/         patches against the SDK the port depends on.
tools/           staging, licence generation, and the release gates.
docs/            end-user and contributor documentation.
```

## Things that are the way they are on purpose

**The launcher links no part of the SDK.** That is what lets CI build it, lets
it build when the game does not, and keeps it from failing in the dynamic
linker. Disc identity is answered by a separate binary that does link the SDK.
Do not "simplify" this by merging them.

**One dump table, two gates.** `src/common/supported_dumps.h` is used by both
the launcher's identity check and the game's own startup gate. Never copy it —
two copies means one of them is never tested.

**Settings render to argv by omission.** An unset setting emits no flag at all,
never `--flag=`. An empty value is consumed as the next argument and silently
shifts the whole command line. `src/launcher/tests/test_settings.cpp` holds this;
do not route around it.

**The dump gate has no override flag.** Now that the executable ships prebuilt,
that gate is the only remaining guarantee that a user's assets match the build
compiled against them.

**Generated sources are never committed.** They are a mechanical translation of
the user's own executable, regenerated locally from configuration that *is*
committed here. Addresses and table entries are facts about a binary; the binary
is not.

## Verification, and its limit

**Headless testing cannot verify this launcher's UI.** There is no window
manager in CI, so nothing delivers focus and synthetic input never reaches a
control. CI proves the RML/RCSS documents parse with zero diagnostics and that
code paths run. It cannot prove a user can reach them.

This is not a theoretical caveat. A previous fully-green headless pass shipped
three defects that a human with a controller found in minutes: every button
silently discarding its click, B doing nothing, and Play producing a black
screen from one missing flag.

So: **anything input-driven needs a human before it is called done.** Say
"implemented, unverified" and add it to `docs/KNOWN_ISSUES.md` rather than
implying a machine confirmed it. Do not fake a screenshot, and do not describe a
headless run as evidence that a control works.

**Windows is unverified in the same way, only more so.** The Win32 code is
written from documentation and has never run on real Windows. Never describe it
as working, and never describe a compatibility-layer result as evidence about
real Windows.

## Comments

Explain why, not what — particularly when the obvious approach was tried and
failed. That history is the most valuable thing in a comment and the easiest to
lose.

**Do not cite documents that are not in this repository.** No plan
identifiers, milestone numbers or release-gate item numbers: none of them exist
here, so they read as nonsense. State the reasoning the citation stood for.

## Before committing

```sh
tools/check_no_game_content.sh
cmake -S src/launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/launcher && ctest --test-dir build/launcher --output-on-failure
```

Touching a third-party dependency also means:

```sh
tools/make_notice.py --sdk /path/to/rexglue-sdk > NOTICE
```

Cutting a release also means:

```sh
tools/check_gpl_boundary.sh <every binary being shipped>
```

That last one exists because the SDK tree contains GPL binutils for the
recompiler, and no shipped binary may link it. It follows symlinks, because
staged runtimes usually are symlinks and a gate that silently skips its target
is worse than no gate.

## Tone in user-facing text

No developer vocabulary reaches a player. No "sha256", no "XEX", no "shared
memory segment", no exit codes. Say what is wrong in terms of the disc and the
game, then say what to do about it. The launcher's failure copy in
`src/launcher/src/launcher_app.cpp` is the reference.
