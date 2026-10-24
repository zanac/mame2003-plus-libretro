# Cave CV1000: analysis for a port from current MAME

Status: analysis only, nothing ported yet. This file lists what the board
needs, what the core already has, the work to do and the non-regression
tests the port would need.

Source in current MAME: `src/mame/cave/cv1k.cpp` (driver, 1184 lines),
`cv1k_v*.cpp/.h/.ipp` (the blitter, about 6000 lines), `cv1k_touch.cpp`,
`src/devices/cpu/sh/sh3comn.cpp`, `sh7709s.cpp` and the shared SH core
(`sh.cpp`, `sh4.cpp`), `src/devices/sound/ymz770.cpp`,
`src/devices/machine/rtc9701.cpp`, `nandflash.cpp`.

## The games

All are flagged `MACHINE_IMPERFECT_TIMING` in current MAME, and all are
playable there.

| Set | Game | PCB |
|---|---|---|
| `mushisam` (+2) | Mushihime-Sama | B |
| `ibara` (+1) | Ibara | B |
| `ibarablk` (+1) | Ibara Kuro Black Label | B |
| `espgal2` (+2) | Espgaluda II | B |
| `mushitam` (+1) | Puzzle! Mushihime-Tama | B |
| `pinkswts` (+4) | Pink Sweets: Ibara Sorekara (+ Suicide Club bootleg) | B |
| `futari15` (+2) | Mushihime-Sama Futari 1.5 / 1.0 | B |
| `futaribl` (+1) | Mushihime-Sama Futari Black Label | B |
| `mmpork` | Muchi Muchi Pork! | B |
| `deathsml` | Deathsmiles | B |
| `dsmbl` | Deathsmiles MegaBlack Label | D |
| `ddpdfk` (+1) | DoDonPachi Dai-Fukkatsu 1.5 / 1.0 | D |
| `dfkbl` | DoDonPachi Dai-Fukkatsu Black Label | D |
| `mmmbanc` | Medal Mahjong Moukari Bancho (touch screen) | B |

28 sets plus the mahjong game. Akai Katana and DoDonPachi SaiDaiOuJou have
ROM definitions in MAME but are commented out (not working there).

## The hardware

- **CPU**: Hitachi SH7709S, an SH-3 at 102.4 MHz, no FPU. The game copies its
  program to work RAM (8 MB on CV1000-B, 16 MB on CV1000-D) and runs from
  there. Big-endian (the CV1000 boards run it in big-endian mode).
- **Blitter**: an Altera Cyclone FPGA (Cave's "EPIC12"): it copies sprites
  from an 8192 x 4096 pixel VRAM to the screen, with flipping, tinting,
  transparency and 8 source and 8 destination blend modes, and uploads graphics to VRAM. Its busy time
  is part of the game timing.
- **Sound**: Yamaha YMZ770 ("AMM"): 16 channels of MPEG-like compressed audio
  read from two 4 MB flash chips, plus a sequencer.
- **Storage**: a 16/32 Mbit NOR flash with the boot code and program (`U4`), a
  128 MB NAND flash with the graphics (`U2`, with its spare area and bad
  block table), the sound flashes (`U23`, `U24`).
- **RTC/EEPROM**: an Epson RTC9701 on a serial line driven by the SH-3 ports.
- **Inputs**: through the SH-3 I/O ports (C, D, F, L) and the FPGA (DIP
  switches).
- **Video**: 320 x 240, 60 Hz; vertical games use ROT270.

## What the core already has

| Part | In the core | Notes |
|---|---|---|
| 32-bit big-endian CPU interface with 32-bit addresses | yes | `32bedw`, as the SH-2: no change to `memory.c` |
| SH-2 interpreter (`src/cpu/sh2`) | yes | SH-2 only: no SH-3 control registers, exception model, banked registers |
| MPEG audio decoder (`src/sound/mpeg_audio.c`) | partly | C89 port of MAME's, layers I and II only: the AMM variant was left out |
| SH-3 core and SH7709S peripherals | no | |
| CV1000 blitter | no | |
| YMZ770 | no | |
| RTC9701, NAND flash | no | |

## The work

| # | Part | Size in MAME | Notes | Estimate |
|---|---|---|---|---|
| 1 | SH-3 CPU core, C89 | ~5000 of `sh.cpp`/`sh4.cpp`/`sh3comn.cpp` used by an SH-3 | A new core, not a change to the SH-2 one (so the SH-2 games can't regress): SH-3 instructions (banked registers, `LDTLB`, `PREF`, `SHAD`/`SHLD`, `CLRS`/`SETS`, `MAC`), the SH-3 exception and interrupt model (`EXPEVT`/`INTEVT`, `VBR` offsets), P0-P4 address areas. No FPU; no MMU (MAME doesn't emulate it either: the register is only stored). | 6-8 days |
| 2 | SH7709S peripherals | ~800 + parts of `sh3comn.cpp` | Interrupt controller (IRL/IRQ lines, priorities), timers (TMU), I/O ports A-L, the cache arrays at `0xf0000000` as RAM, bus control registers | 2-3 days |
| 3 | Blitter | ~6000 (9 generated blit variants) | The pixel loops become C89 macros or generated functions; blend modes, tint, flips, transparency; VRAM upload; the busy-time model (the game waits on it); screen update with the scroll and clip windows | 5-7 days |
| 4 | YMZ770 | 838 | 16 channels, the sequencer, volume/pan; on top of `mpeg_audio.c` | 2 days |
| 5 | AMM in `mpeg_audio.c` | ~150 | The AMM header and frame reading, as in MAME. Layers I/II must stay bit-identical (Sega DSB, Model 2) | 1-2 days |
| 6 | NAND flash, RTC9701 | ~1300 | Page reads with the spare area through `flash_io` and port E; serial RTC/EEPROM with NVRAM | 2 days |
| 7 | Driver | 1184 | Memory maps for CV1000-B and -D, inputs and DIPs, ROM loading, the per-game idle-skip speedups, 28 sets; the touch screen game left out at first | 2-3 days |
| 8 | libretro and metadata | | Video size and rotation, save states for all the new parts from the start (they are new code, so they can be complete), `mame2003-plus.xml`, `catver.ini`, `CHANGELOG.md`, a `docs/cave-cv1000.md` like the other ports | 1-2 days |
| 9 | Testing and fixes | | Every set to the first stages; compare with MAME's frames and audio where possible | 4-6 days |

**Total: about 4-6 weeks.** Parts 1-3 are most of it and carry most of the
risk.

## Risks

- **Speed.** MAME runs the SH-3 with its recompiler (DRC) and fast RAM ranges,
  and with idle-skip speedups for every game. Our SH-2 is an interpreter: on
  the test machine (Xeon 2.1 GHz) the Psikyo SH games (SH-2 at 28.6 MHz, with
  idle skip) run 3000 frames in 5.6-6.2 s of CPU, about 8 times real time. An
  SH-3 at 102.4 MHz is 3.6 times the clock; with the idle skips it should run
  at full speed on a PC. On ARM boards (Raspberry Pi 3/4) it is not likely,
  and the blitter adds a lot: MAME can run it in a separate thread, which
  this core doesn't do. A recompiler for the SH-3 is planned after the
  interpreter: our own, on the sljit already in the core for the MIPS and
  ADSP recompilers (x86, x86-64, ARMv7, AArch64), not MAME's UML DRC. It must
  give the same results as the interpreter, cycle for cycle (MAME's cache and
  bus timing included), so save states, rollback and netplay don't depend on
  it; big-endian hosts and the systems without run-time code generation
  keep the interpreter.
- **Blitter timing.** The games wait on the blitter; MAME models its busy
  time from the number of SRAM clocks and upload gaps. Getting this wrong
  gives slowdowns or missing frames, not crashes.
- **Big-endian hosts.** A big-endian CPU on little-endian hosts (and the
  reverse on PowerPC), 16-bit VRAM and byte-oriented flash data: the
  `32bedw` interface handles the CPU side, the blitter and NAND code must
  not assume the host order.
- **Size.** The core grows by about 1-2 MB (memory is allocated only when a
  CV1000 game runs: MAME keeps the 8192 x 4096 VRAM as 32-bit pixels, 128 MB;
  16-bit pixels would halve it. Plus up to 16 MB of RAM and the 128 MB NAND).

## Non-regression

The port is mostly new files. The shared files it touches, and the tests
each one needs:

| Shared file | Change | Test |
|---|---|---|
| `Makefile.common`, `cpuintrf.c`, `sndintrf.c`, `driver.c` | new CPU, sound chip, driver entries | builds: C89 (`-std=gnu89 -pedantic`), clang/macOS, MSVC legacy, armhf, aarch64, PowerPC; core size |
| `src/sound/mpeg_audio.c` | AMM variant | Sega DSB games and the Model 2 games that use MPEG audio: audio bit-identical before and after |
| `src/cpu/sh2` | none (separate SH-3 core) | none needed; if code is shared after all: ST-V, Psikyo SH, CPS-3, Model 2 sound, bit-identical runs |
| `memory.c`, `cpuexec.c` | none expected | if touched: the 34-game suite bit-identical |

And for every other game: frames and audio bit-identical to master on the
usual suite (34 games across Pac-Man/Epos, Astrocade, Fuuki, Psikyo SH,
Metro, Irem M92, System 32, Sega G80), plus the games of the touched sound
code.

## Suggested order

1. SH-3 core and peripherals, with the boot code of one CV1000-B game
   (Mushihime-Sama) running to its test screens without the blitter.
2. NAND flash and RTC: the game checks them at boot.
3. Blitter: first plain copies, then the blend modes.
4. YMZ770 and AMM.
5. The other sets, then CV1000-D (Deathsmiles MBL, Dai-Fukkatsu).

The first step tells if the speed is acceptable before the rest is done.
