/*
 * Hatari - multidevice.h
 *
 * This file is distributed under the GNU General Public License, version 2
 * or at your option any later version. Read the file gpl.txt for details.
 *
 * SidecarTridge Multi-device on the cartridge port, running a firmware
 * built for the host with EmuMD (a .mdfw plugin).
 * Copyright (C) 2026 Neil Rackett
 */

#ifndef HATARI_MULTIDEVICE_H
#define HATARI_MULTIDEVICE_H

#include <stdbool.h>
#include <stdint.h>

/* Load the firmware set in ConfigureParams.MultiDevice and power it on
 * (MultiDevice_Reset does this at the first cold reset). Returns false,
 * after telling the user, if it could not. */
extern bool MultiDevice_Init(void);
extern void MultiDevice_UnInit(void);
extern bool MultiDevice_IsActive(void);

/* True if Hatari's own cartridge program (GEMDOS drive, VDI modes...)
 * must keep the first 1 KB of the cartridge; the firmware gets the rest. */
extern bool MultiDevice_KeepsHatariCartridge(void);

extern uint16_t MultiDevice_Rom4Read(uint32_t offset);
extern void MultiDevice_Rom3Read(uint32_t offset);
extern void MultiDevice_VBL(void);
extern void MultiDevice_Reset(bool bCold);

/* Is `path` a firmware file (.mdfw, or .uf2 for later)? */
extern bool MultiDevice_IsFirmwareFile(const char *path);

#endif
