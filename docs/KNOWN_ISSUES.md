# Known issues

As of v0.2.1.

## In-level cutscenes run too fast without a frame cap

**Impact:** career cutscenes play at three to five times speed and skip.
**Workaround:** leave the frame rate capped, which is the default. The launcher
ships with it on and you have to go out of your way to turn it off.

The cause is localised but not fixed. The variable is the guest frame rate and
nothing else: uncapped, the game runs at ~200 fps and the in-level cutscene
update advances with it. Capped, it is correct. The front-end attract cinematic
is immune, so the front-end cinematic player is timebase-correct and the
in-level cutscene update is different code. That is the next thing to read.

One thing this is *not*: an earlier belief that pressing F5 fixed it was wrong.
Every session where F5 appeared to help was also a capped session.

## macOS performance

v0.2.1 restores a complete, self-contained Apple Silicon archive. It bundles
the Vulkan loader and MoltenVK, so players do not need Homebrew. The exact
archive was tested on a 2020 Apple M1 MacBook Pro: the packaged supervisor
started the game, the packaged Vulkan libraries loaded, and the marked fixture
reached a visually verified 2,704-draw Funpark frame.

It is functionally correct but not performance-playable in that severe scene.
Across the fixture's 2,500-plus-draw rows, mean frame time was 236.79 ms (4.22
effective FPS), with 233.75 ms p50 and 251.29 ms p95; the process used roughly
317% CPU. The much higher Linux desktop and Steam Deck figures elsewhere in the
documentation do not transfer to the base M1's MoltenVK path.

The binaries are ad-hoc signed but not notarized. Gatekeeper therefore requires
the right-click → Open step documented in the main README.

## Windows is mitigated and Proton-tested, not hardware-verified

The launcher, disc worker, supervisor, and game are all built for the MSVC ABI
and exercised under Proton. A community
report confirms that v0.1.0 started on Windows 10, but its CPU use and pacing
were unusable there. The maintainers do not own a Windows machine, so the
complete v0.2.1 build has not been verified on real Windows hardware.

What has been confirmed under the compatibility layer: the launcher opens and
navigates, a real disc image is identified correctly, a full 4.7 GB extraction
is byte-identical to the Linux one, the handoff to the game works, and the game
boots, loads its executable, creates a Vulkan swapchain and renders.

What that does **not** tell us is how any of it behaves on Windows. Proton is a
different implementation of the same interfaces, and the places it differs are
exactly the places an untested port breaks. Treat Windows as unverified.

v0.1.0 also had a source-level pacing defect consistent with the report that it
used excessive CPU, ran below speed, and made capture unusable. SDL's Windows
timer resolution was explicitly disabled, so a requested 16.67 ms vblank sleep
could land at 31.25 ms. The worker then marked only one vblank and discarded
the second elapsed interval, effectively pacing the guest near 32 Hz while its
waiters kept consuming CPU.

v0.2.0 stops disabling SDL's timer resolution, waits on a high-resolution
deadline timer, and delivers a bounded number of elapsed vblanks after a late
wake. Related guest waits now sleep or block on their actual producer instead
of truncating sub-millisecond waits into yields.

The complete v0.2.1 archive sustained a deterministic 2,600-plus-draw
late-game fixture under Proton 10 on a Steam Deck and rendered all nine marked
checkpoints. It held the player-default 30 fps presentation mode at 33.31 /
34.49 ms p50 / p95. Uncapped, it measured 27.83 / 32.02 ms versus native Linux
at 25.00 / 27.00 ms, or 35.64 versus 40.01 effective FPS.

Wine reparents the game outside the test supervisor, so this v0.2.1 run does
not provide a trustworthy process-CPU comparison. Earlier v0.2.0 lifetime
snapshots put the detached Windows process at roughly 319–349% CPU, compared
with 253–256% natively, but those figures are not precise enough for a release
to claim a CPU improvement.

This is a concrete mitigation for the reported mechanism, with a measurable
Proton overhead. Only a report from real Windows can tell us whether it fixes
the original machine's behaviour and close the hardware-verification gap.

If you run it, we want the report either way. "It opened and the buttons worked"
is as useful as a crash.

## The Stop button on Windows is abrupt

On Linux, stopping the game from the launcher sends `SIGTERM` and the game
flushes its state on the way out. Windows offers no equivalent for a windowed
process from outside, so that path terminates it directly. The Stop control has
not been exercised on real Windows hardware.

## Verification that needs a human

Headless testing can prove that code paths run. It cannot prove a user can
reach them: with no window manager there is nothing to deliver focus, so
synthetic clicks and keypresses never land. An earlier round found three
defects only when someone sat down with a controller: every button silently
discarded its click, B did nothing, and Play produced a black screen because a
GPU plugin flag was missing.

So these are known-unverified rather than known-good:

- Layout of the Display and About screens at sizes other than the default.
- Dragging the window between monitors with different scaling.
- Opening the launcher while the game is running, and stopping it from there.
- An unwritable `config/` directory.

## Smaller things

- The launcher has no visual regression test. Document parse failures are
  caught; layout drift is not.
- `thps_p8_identify` distinguishes "same game, different release" from "a
  different game" by comparing the size of `default.xex`. It is deliberately
  conservative and would misclassify a different game whose executable happened
  to be exactly 8,237,056 bytes.
- `Unimplemented XLIVEBASE message` at startup is harmless noise.
