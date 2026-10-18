/*
  Hatari - sfp004.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef HATARI_SFP004_H
#define HATARI_SFP004_H

extern bool Sfp004_IsEnabled(void);
extern void Sfp004_InterceptIoMem(void);
extern void Sfp004_Reset(void);
extern void Sfp004_MemorySnapShot_Capture(bool bSave);

#endif /* HATARI_SFP004_H */
