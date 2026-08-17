# Known issues

As of v0.1.0.

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

## Windows is unverified

Everything for Windows — the launcher, the disc worker, the supervisor and the
game itself — is cross-compiled from Linux and exercised under Wine. None of it
has ever run on real Windows hardware, because nobody on the project has a
machine to try it on.

What has been confirmed under Wine: the launcher opens and navigates, a real
disc image is identified correctly, a full 4.7 GB extraction is byte-identical
to the Linux one, the handoff to the game works, and the game boots, loads its
executable, creates a Vulkan swapchain and renders.

What that does **not** tell us is how any of it behaves on Windows. Wine is a
different implementation of the same interfaces, and the places it differs are
exactly the places an untested port breaks. Treat Windows as unverified.

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
