# Known issues

As of the v0.2.0 release candidate.

## In-level cutscenes run too fast without a frame cap

**Impact:** career cutscenes play at three to five times speed and skip.
**Workaround:** leave the frame rate capped, which is the default. The launcher
ships with it on and you have to go out of your way to turn it off.

The cause is localised but not fixed. The variable is the guest frame rate and
nothing else: uncapped, the game runs at ~200 fps and the in-level cutscene
update advances with it. Capped, it is correct. The front-end attract cinematic
is immune, so the front-end cinematic player is timebase-correct and the
in-level cutscene update is different code — that is the next thing to read.

One thing this is *not*: an earlier belief that pressing F5 fixed it was wrong.
Every session where F5 appeared to help was also a capped session.

## macOS performance

Native Apple Silicon works and reaches gameplay, but is not fast. The ~100 fps
figure comes from a desktop Linux machine with a discrete GPU and does not
transfer: on an M1, Free Skate reaches gameplay at roughly 6 fps. It is
functionally correct and not performance-playable.

## Windows is mitigated and Proton-tested, not hardware-verified

Everything for Windows — the launcher, the disc worker, the supervisor and the
game itself — is built for the MSVC ABI and exercised under Proton. None of it
has run on real Windows hardware, because nobody on the project has a machine
to try it on.

What has been confirmed under the compatibility layer: the launcher opens and navigates, a real
disc image is identified correctly, a full 4.7 GB extraction is byte-identical
to the Linux one, the handoff to the game works, and the game boots, loads its
executable, creates a Vulkan swapchain and renders.

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
of truncating sub-millisecond waits into yields. The complete Windows archive
must sustain a Proton smoke before the release is tagged. This is a concrete
mitigation for the reported mechanism, but only a report from real Windows can
close the hardware-verification gap.

If you run it, we want the report either way. "It opened and the buttons worked"
is as useful as a crash.

## The Stop button on Windows is abrupt

On Linux, stopping the game from the launcher sends `SIGTERM` and the game
flushes its state on the way out. Windows offers no equivalent for a windowed
process from outside, so that path terminates it directly. Untested, along with
the rest of the Windows build.

## Verification that needs a human

Headless testing can prove that code paths run. It cannot prove a user can
reach them: with no window manager there is nothing to deliver focus, so
synthetic clicks and keypresses never land. An earlier round of this found three
defects only when someone sat down with a controller — every button silently
discarding its click, B doing nothing, and Play producing a black screen from a
missing GPU plugin flag.

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
