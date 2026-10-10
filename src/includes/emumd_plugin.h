/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * File: emumd_plugin.h
 * Description: The interface between an emulator (EmuMD's fork of
 *              Hatari) and a SidecarTridge Multi-device firmware built
 *              for the host as a .mdfw plugin (a shared library).
 *
 * The emulator loads the plugin, calls its entry point once, and from then
 * on routes the ST's cartridge-port reads to it: ROM4 ($FA0000-$FAFFFF)
 * returns a word from the firmware's 64 KB window, ROM3 ($FB0000-$FBFFFF)
 * hands the firmware the low 16 address bits, which is how the ST sends it
 * commands. The firmware runs in between on the emulator's thread (and on
 * a second thread if it starts its second core), so to the emulated ST the
 * Multi-device is infinitely fast.
 *
 * GPL-2.0-or-later so that it can be built into Hatari. Both sides include
 * this same file: EmuMD's include/emumd_plugin.h, and its copy in the Hatari
 * fork's src/includes, which build-hatari.sh checks are the same.
 */

#ifndef EMUMD_PLUGIN_H
#define EMUMD_PLUGIN_H

#include <stdint.h>

/* Bumped on any incompatible change to the structures below. */
#define EMUMD_PLUGIN_ABI 1

/* The symbol every plugin exports: const emumd_plugin_t *emumd_plugin_v1(void) */
#define EMUMD_PLUGIN_ENTRY "emumd_plugin_v1"

/* What the emulator gives the firmware at power-on. */
typedef struct {
  uint32_t abi;        /* EMUMD_PLUGIN_ABI */
  const char *sd_dir;  /* host folder standing in for the microSD card, or NULL */
  const char *options; /* the user's --md-option key=value pairs, one per line */
  int verbose;         /* show the firmware's debug output */
  /* The emulator's log; one line of text, no trailing newline needed. */
  void (*log)(const char *line);
} emumd_host_t;

/* What the plugin gives the emulator. `now_us` is emulated time since the
 * emulator started, in microseconds; it never goes backwards. */
typedef struct {
  uint32_t abi;        /* EMUMD_PLUGIN_ABI */
  const char *name;    /* e.g. "ROTT Accelerator" */
  const char *version; /* e.g. "v0.1.0" */

  /* Power the device on (the ST was switched on, or cold-reset): boot the
   * firmware. Returns 0, or non-zero if it could not start (it logs why). */
  int (*power_on)(const emumd_host_t *host, uint64_t now_us);
  /* Power it off (cold reset or emulator exit). Flash contents survive. */
  void (*power_off)(void);

  /* The ST reads a word of ROM4; offset 0..0xFFFE (even). */
  uint16_t (*rom4_read)(uint32_t offset, uint64_t now_us);
  /* The ST reads ROM3; offset 0..0xFFFF is the low 16 address bits. */
  void (*rom3_read)(uint32_t offset, uint64_t now_us);
  /* Called once per ST frame (VBL), so the firmware's main loop also runs
   * while the ST leaves the cartridge alone. */
  void (*tick)(uint64_t now_us);
} emumd_plugin_t;

typedef const emumd_plugin_t *(*emumd_plugin_entry_t)(void);

/* What a plugin defines (EmuMD's runtime does it for you). */
const emumd_plugin_t *emumd_plugin_v1(void);

#endif /* EMUMD_PLUGIN_H */
