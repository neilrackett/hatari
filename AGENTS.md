# Working on Neil Rackett's Hatari fork

This is [Neil Rackett](https://neilrackett.com/atarist)'s fork of
[Hatari](https://github.com/hatari/hatari), the Atari ST/STE/TT/Falcon
emulator. He adds features to it that he needs for his own Atari
development: running SidecarTridge Multi-device firmware, driving Hatari
from scripts and coding agents, and hardware Hatari didn't emulate, such as
the SFP-004 FPU. Everything else, including `readme.txt` and `doc/`, is
upstream Hatari's; this file covers only what is different here.

## Branches

- `emumd` is the fork's default branch, and the one to work on: Hatari
  2.6.1 (tag `v2.6.1`) with one commit per added feature.
- `main` mirrors upstream's `main`, updated now and then. Don't commit
  fork features to it.
- The `upstream` remote is [hatari/hatari](https://github.com/hatari/hatari).

## What the fork adds

Each of these is a single commit on `emumd`; `git log v2.6.1..emumd` lists
them, and each message says why.

### SidecarTridge Multi-device (EmuMD)

Runs a [SidecarTridge Multi-device](https://sidecartridge.com) firmware on
the cartridge port, built for the host by EmuMD as a `.mdfw` shared
library. ROM4 reads come from the firmware's 64 KB window, ROM3 reads go to
it as commands, and it runs once a VBL in between.

- `--md-firmware <file.mdfw>`, or a `.mdfw` as the last argument
- `--md-sd <dir>`: a folder standing in for its microSD card
- `--md-option <key=value>`: repeatable, `none` clears them
- `--md-verbose <bool>`: show the firmware's debug output
- Config file: a `[MultiDevice]` section (`bEnabled`, `szFirmwareFileName`,
  `szSdCardDirectory`, `szOptions`, `bVerbose`)

When Hatari needs its own cartridge program (a GEMDOS drive, extended VDI
modes), it keeps the first 1 KB of the cartridge and the firmware gets the
rest. Code: `src/multidevice.c`, `src/includes/multidevice.h`, the
cartridge hooks in `src/cpu/memory.c`, and the plugin interface
`src/includes/emumd_plugin.h` (see Making a change).

The window title starts with `EmuMD - ` so the fork's windows are easy to
tell from a stock Hatari.

### Scripting

- `--cmd-fifo` takes `hatari-event leftdown`, `leftup` and
  `mousemove <dx> <dy>`, so scripts can use the ST's mouse as well as its
  keyboard. `mousemove` is relative and in ST pixels: move far up and left
  first to pin the pointer in the corner. Code: `src/control.c`.
- `--run-vbls <n>` quits as closing the window does, so a video being
  recorded is finished and playable. Code: `src/main.c`.

### GEMDOS drives

Each program gets its own current folder on a GEMDOS drive
(`--harddrive`), as in TOS: when a program ends, any folder change it made
is undone. Upstream keeps one per drive, so a program that changed folder
(STinG, from AUTO) left the next one looking in the wrong place. Code:
`src/gemdos.c`.

### SFP-004 / Mega STE FPU

A 68000 or 68010 has no coprocessor interface, so on an ST, Mega ST, STE
or Mega STE a 68881/68882 is memory-mapped at $FFFA40 (Atari's SFP-004
card, the Mega STE's FPU socket), and programs drive its coprocessor
interface registers themselves. Give such a machine a 68881 or 68882 FPU
and it gets this one:

```sh
hatari --machine megaste --fpu 68882      # --fpu after --machine, which resets it
```

or `nCpuLevel = 0` and `n_FPUType = 68882` (or `68881`) in the config
file's `[System]` section, or the CPU dialog's FPU buttons. The status bar
shows `68882(SFP-004)`, EmuTOS sets the `_FPU` cookie's SFP-004 bit, and
`--trace sfp004` logs every register access.

`src/sfp004.c` emulates the whole peripheral protocol from the
MC68881/MC68882 User's Manual (chapter 7): every operand format, FMOVECR,
FMOVE(M) of data and control registers, conditional predicates,
FSAVE/FRESTORE and enabled exceptions, with each chip's response
primitives. The arithmetic is the CPU core's softfloat with the FPU's own
registers. Timing comes from the manual's tables 8-2 and 8-3: an
instruction starts on the response read after its command, the FPU stays
busy for the table's time at 16 MHz while polls read `$8900`, and at
16 MHz the CPU waits for the bus handshake (`SFP004_BUS_CLOCKS`). It was
checked against a real Mega STE with a 68882 (see Testing).

## Building

You need CMake, a C compiler and SDL2; libpng and zlib are optional
(macOS: `brew install cmake sdl2 libpng`). Build outside the checkout, so
nothing needs ignoring:

```sh
cmake -S . -B ../hatari-build -DCMAKE_BUILD_TYPE=Release -DENABLE_OSX_BUNDLE=0
cmake --build ../hatari-build -j8          # -> ../hatari-build/src/hatari
```

On macOS add `-DCMAKE_OSX_ARCHITECTURES=$(uname -m)`.
`-DENABLE_OSX_BUNDLE=1` (the default on macOS) builds `src/Hatari.app`,
with Hatari's menu bar; that needs Xcode, whose `ibtool` compiles the
menus. Its binary, `Hatari.app/Contents/MacOS/Hatari`, takes the same
arguments.

EmuMD builds the fork itself, at the commit it pins (see Where it's used),
in `~/.cache/emumd/hatari-fork`. Don't work in that folder: EmuMD checks it
out again whenever its pin changes.

## Running it from a script

Agents usually can't see Hatari's window, so run it headless and read its
console output and screenshots. A program on a GEMDOS drive, started
automatically, with an empty config file so that the user's own settings
are neither used nor changed:

```sh
mkdir -p /tmp/st/hd && cp PROGRAM.TOS /tmp/st/hd/ && : > /tmp/st/hatari.cfg
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../hatari-build/src/hatari \
  --configfile /tmp/st/hatari.cfg \
  --tos ~/.cache/emumd/emutos-256k-1.4/etos256uk.img \
  --machine megaste --fpu 68882 \
  --harddrive /tmp/st/hd --protect-hd on --auto 'C:\PROGRAM.TOS' \
  --conout 2 --fast-forward on --sound off --run-vbls 3000
```

- `--conout 2` copies what the ST prints to its console to stdout, so a
  test program's `printf` output can be read directly.
- `--run-vbls` ends the run after that many VBLs (50 per emulated second;
  fast forward runs over 1000 a second). It also ends programs that wait
  for a key.
- For screenshots, add `--cmd-fifo /tmp/st/fifo --screenshot-dir /tmp/st
  --screenshot-format png` and write `hatari-shortcut screenshot` to the
  FIFO. They show the ST screen only, without the status bar. The FIFO also
  takes `hatari-debug statesave <file>` (load it with `--memstate`),
  `hatari-debug quit`, and `hatari-event` key and mouse events.
- EmuTOS 1.4 is downloaded to `~/.cache/emumd/emutos-256k-1.4/` by EmuMD's
  build. The 256 KB images don't run the TT or Falcon; Hatari then shows an
  alert that a headless run never gets past. The user's own TOS images
  may be somewhere a sandboxed agent can't read.
- EmuMD's `mdfw run --headless` (in its repo) does all of this for
  Multi-device firmware.

## Testing

- `ctest` in the build folder runs upstream's tests (about 100, in a few
  seconds). The four `tidy-*` checks of `doc/*.html` fail with the 2006
  `tidy` that macOS ships, on pages this fork never touched too; use
  `ctest -E tidy` unless a current tidy-html5 is installed.
- Multi-device: EmuMD's tests and examples (see its AGENTS.md).
- SFP-004: [atarist-sfp004](https://github.com/neilrackett/atarist-sfp004)'s
  test programs. `tests/FPUTEST.TOS` checks results bit for bit against
  soft-float and prints a CIR trace; `tests/FPUBENCH.TOS` times the FPU
  against soft-float. On a Mega STE both run at 8 MHz, 16 MHz and 16 MHz
  with cache. The examples (`JETMESH.TOS` and others) show their FPU
  transform time top left. The real Mega STE with a 68882 they were
  measured on gave:

  | FPUBENCH µs/op (68882) | 8 MHz | 16 MHz | 16 MHz + cache |
  | ---------------------- | ----- | ------ | -------------- |
  | arith                  | 98.1  | 96.2   | 56.8           |
  | sqrt                   | 60.6  | 57.5   | 35.0           |
  | FixedDiv               | 75.0  | 73.0   | 44.0           |

  The emulation is within about 4% of these, except FixedDiv at 16 MHz with
  cache (41). FPUTEST's trace reads the same on both at every speed (`in :
  resp=1504 poll=4000 ->8900`, the same for `add`, and `out: resp=8900
  poll=0 ->0802`), and the jet demo's FPU time flips between 25 and 30 ms on
  both. Hatari's 68000 runs about 3% faster than the real Mega STE (the
  benchmark's soft-float rows), so compare speed-ups rather than absolute
  times.

## Making a change

1. Branch from `emumd`, make one commit per change, then fast-forward
   `emumd` and push it.
2. Unless a change is about the Multi-device, keep it free of EmuMD, so it
   could go upstream as it is. Follow Hatari's style: tabs, a
   GPL-2.0-or-later header like the other files', a
   `const char Module_fileid[]`, `Module_Function()` names. Give new
   hardware its reset hook (`src/reset.c`) and snapshot capture
   (`src/memorySnapShot.c`), and document new options in `src/options.c`'s
   help, `doc/hatari.1` and `doc/manual.html`.
3. Commit messages: a summary line in the imperative, and a body that says
   what changed and why. No attribution or Co-Authored-By lines.
4. Then update EmuMD: set `HATARI_COMMIT` in its `hatari/build-hatari.sh`
   to the new commit, run `tools/mdfw hatari` and its tests, and push it.
   Never push EmuMD naming a commit the fork doesn't have yet.
5. `src/includes/emumd_plugin.h` must stay byte-identical to EmuMD's
   `include/emumd_plugin.h`; `build-hatari.sh` refuses to build otherwise.
   A change to its structs or calls bumps `EMUMD_PLUGIN_ABI`, and
   `PLUGIN_ABI` in EmuMD's `tools/mdfw`.

## Where it's used

- [EmuMD](https://github.com/neilrackett/emumd) runs SidecarTridge
  Multi-device firmware in this Hatari, built for the host. Its
  `hatari/build-hatari.sh` (`mdfw hatari`) builds the fork at a pinned
  commit, as `Hatari.app` on a Mac with Xcode, and `mdfw run` starts it with
  the right options. Its AGENTS.md ("Changing Hatari") and `docs/GUIDE.md`
  describe the fork from EmuMD's side.
- [atarist-sfp004](https://github.com/neilrackett/atarist-sfp004), a C
  library for the SFP-004 FPU, and its demos are developed and tested on
  the fork's SFP-004 emulation, and its tests are how that emulation was
  checked against real hardware.

## Gotchas

- `--machine` resets the FPU type (and CPU), so give `--fpu` after it.
- Without `--configfile`, Hatari uses the user's own settings
  (`~/Library/Application Support/Hatari/hatari.cfg` on macOS,
  `~/.config/hatari/hatari.cfg` on Linux); scripted runs should pass their
  own file.
- zsh doesn't split an unquoted variable into words, so a variable holding
  several Hatari options arrives as one argument and Hatari rejects it.
  Use an array, or `${=var}`.
- The app bundle build shows a Dock icon even with `SDL_VIDEODRIVER=dummy`.
