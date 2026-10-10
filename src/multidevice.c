/*
 * Hatari - multidevice.c
 *
 * This file is distributed under the GNU General Public License, version 2
 * or at your option any later version. Read the file gpl.txt for details.
 *
 * SidecarTridge Multi-device on the cartridge port. The firmware is a
 * .mdfw: its C sources built for the host with EmuMD's runtime, as a
 * shared library (see emumd_plugin.h). ROM4 reads ($FA0000-$FAFFFF) come
 * from the firmware's 64 KB window, ROM3 reads ($FB0000-$FBFFFF) are
 * handed to it as commands. memory.c routes the accesses here.
 *
 * Copyright (C) 2026 Neil Rackett
 */
const char MultiDevice_fileid[] = "Hatari multidevice.c";

#include <dlfcn.h>
#include <string.h>
#include <strings.h>

#include "main.h"
#include "configuration.h"
#include "cart.h"
#include "clocks_timings.h"
#include "cycles.h"
#include "file.h"
#include "log.h"
#include "m68000.h"
#include "emumd_plugin.h"
#include "multidevice.h"

static void *hLibrary;
static const emumd_plugin_t *pPlugin;
static bool bPoweredOn;
static emumd_host_t Host;

static void MultiDevice_Log(const char *line)
{
	Log_Printf(LOG_INFO, "%s\n", line);
}

/* Emulated time in microseconds. CyclesGlobalClockCounter counts CPU
 * cycles at the machine's base clock (8 MHz) whatever the CPU speed. */
static uint64_t MultiDevice_NowUs(void)
{
	uint32_t nFreq = MachineClocks.CPU_Freq_Emul >> nCpuFreqShift;
	if (nFreq == 0)
		nFreq = 8000000;
	return CyclesGlobalClockCounter * 1000000ULL / nFreq;
}

static bool HasExtension(const char *path, const char *ext)
{
	size_t n = strlen(path), e = strlen(ext);
	return n > e && strcasecmp(path + n - e, ext) == 0;
}

bool MultiDevice_IsFirmwareFile(const char *path)
{
	return HasExtension(path, ".mdfw") || HasExtension(path, ".uf2");
}

static bool MultiDevice_PowerOn(void)
{
	if (!pPlugin || bPoweredOn)
		return bPoweredOn;
	Host.abi = EMUMD_PLUGIN_ABI;
	Host.sd_dir = ConfigureParams.MultiDevice.szSdCardDirectory[0]
	              ? ConfigureParams.MultiDevice.szSdCardDirectory : NULL;
	Host.options = ConfigureParams.MultiDevice.szOptions;
	Host.verbose = ConfigureParams.MultiDevice.bVerbose;
	Host.log = MultiDevice_Log;
	if (pPlugin->power_on(&Host, MultiDevice_NowUs()) != 0)
	{
		Log_AlertDlg(LOG_ERROR, "Multi-device firmware '%s' did not start.", pPlugin->name);
		return false;
	}
	bPoweredOn = true;
	return true;
}

static void MultiDevice_PowerOff(void)
{
	if (pPlugin && bPoweredOn)
		pPlugin->power_off();
	bPoweredOn = false;
}

bool MultiDevice_Init(void)
{
	const char *path = ConfigureParams.MultiDevice.szFirmwareFileName;
	emumd_plugin_entry_t entry;

	if (pPlugin)
		return true;
	if (!ConfigureParams.MultiDevice.bEnabled || !path[0])
		return false;

	if (HasExtension(path, ".uf2"))
	{
		Log_AlertDlg(LOG_ERROR, "Multi-device: '%s' is a .uf2. Running .uf2 files "
		             "is not supported yet; build the firmware as a .mdfw with "
		             "EmuMD's 'mdfw build'.", path);
		return false;
	}
	if (!File_Exists(path))
	{
		Log_AlertDlg(LOG_ERROR, "Multi-device firmware '%s' not found.", path);
		return false;
	}
	hLibrary = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!hLibrary)
	{
		Log_AlertDlg(LOG_ERROR, "Multi-device firmware '%s' could not be loaded:\n%s",
		             path, dlerror());
		return false;
	}
	entry = (emumd_plugin_entry_t)dlsym(hLibrary, EMUMD_PLUGIN_ENTRY);
	pPlugin = entry ? entry() : NULL;
	if (!pPlugin || pPlugin->abi != EMUMD_PLUGIN_ABI)
	{
		Log_AlertDlg(LOG_ERROR, "'%s' is not a Multi-device firmware for this "
		             "Hatari (interface %u, need %u).", path,
		             pPlugin ? pPlugin->abi : 0, EMUMD_PLUGIN_ABI);
		pPlugin = NULL;
		dlclose(hLibrary);
		hLibrary = NULL;
		return false;
	}
	Log_Printf(LOG_INFO, "Multi-device: %s %s (%s)\n", pPlugin->name,
	           pPlugin->version, path);
	if (strlen(ConfigureParams.Rom.szCartridgeImageFileName) > 0)
		Log_Printf(LOG_WARN, "Multi-device: the cartridge image '%s' is ignored.\n",
		           ConfigureParams.Rom.szCartridgeImageFileName);
	return MultiDevice_PowerOn();
}

void MultiDevice_UnInit(void)
{
	MultiDevice_PowerOff();
	/* The library stays loaded: a firmware may have left threads running. */
	pPlugin = NULL;
}

bool MultiDevice_IsActive(void)
{
	return pPlugin != NULL && bPoweredOn;
}

bool MultiDevice_KeepsHatariCartridge(void)
{
	return Cart_UseBuiltinCartridge();
}

uint16_t MultiDevice_Rom4Read(uint32_t offset)
{
	return pPlugin->rom4_read(offset & 0xfffe, MultiDevice_NowUs());
}

void MultiDevice_Rom3Read(uint32_t offset)
{
	pPlugin->rom3_read(offset & 0xffff, MultiDevice_NowUs());
}

void MultiDevice_VBL(void)
{
	if (MultiDevice_IsActive())
		pPlugin->tick(MultiDevice_NowUs());
}

/* Called at the start of every reset, before memory is set up. The
 * Multi-device is powered from the cartridge port: a cold reset (power
 * cycle) starts or restarts the firmware, a warm one (reset button) does
 * not touch it. */
void MultiDevice_Reset(bool bCold)
{
	if (!bCold)
		return;
	if (!pPlugin)
	{
		MultiDevice_Init();
		return;
	}
	MultiDevice_PowerOff();
	MultiDevice_PowerOn();
}
