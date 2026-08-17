# Contributing

## The one rule

**No game content, ever.** Not assets, not audio, not video, not textures, not
`default.xex`, not a disc image, not decrypted intermediates, not platform keys,
and not the translation units the recompiler generates from a dump.

`tools/check_no_game_content.sh` checks this, and a pull request that trips it
will not be merged. If you are unsure whether a file counts, it counts.

Do not open issues or discussions asking where to obtain the game. They will be
closed without an answer.

## What is useful

**A Windows report.** This is the single most valuable thing anyone can send
right now. The launcher is built for Windows and has never run on real Windows
hardware. Whether it works or falls over, we want to know — see the Windows
issue template.

**A hash from another regional disc.** The launcher accepts one release today.
If you own a PAL or NTSC-J disc, the SHA-256 of its `default.xex` and the size
of that file are enough to add a row. Do not send the file.

**Bug reports with logs.** `logs/` in your install folder, plus the terminal
output.

**Fixes.** See the open issues.

## Before you send a pull request

```sh
cmake -S src/launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/launcher
ctest --test-dir build/launcher --output-on-failure
tools/check_no_game_content.sh
```

If your change adds, upgrades or removes a third-party dependency, regenerate
`NOTICE`:

```sh
tools/make_notice.py --sdk /path/to/rexglue-sdk > NOTICE
```

## Things worth knowing about this codebase

**Headless testing cannot verify the launcher's UI.** There is no window manager
in CI, so nothing delivers focus and synthetic input never reaches a control. CI
proves the documents parse and the code paths run. Anything input-driven needs a
human with a controller before it is called done — this has caught real defects
that a full headless pass reported as green.

**Settings render to argv by omission, not by empty values.** An unset setting
emits no flag at all, never `--flag=`, because an empty value is consumed as the
next argument and silently shifts the whole command line. `src/launcher/tests/`
holds that rule; do not route around it.

**The dump table is shared.** `src/common/supported_dumps.h` is used by both the
launcher's identity check and the game's own gate. Do not copy it — two copies
means one of them is untested.

**The game's host code and the launcher are separate builds on purpose.** The
launcher links no part of the SDK, so it still builds when the game does not.
Keep it that way.

## Style

Match the file you are editing. Comments explain why something is the way it is,
particularly when the obvious approach was tried and failed; they do not narrate
what the code does.
