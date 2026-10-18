/*
  Hatari - sfp004.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  SFP-004: an MC68881 or MC68882 floating point coprocessor in peripheral
  mode, memory-mapped at $FFFA40. This is how Atari's SFP-004 card adds an
  FPU to a Mega ST, and how the FPU socket of a Mega STE is wired. It's
  what a 68881/68882 FPU setting gives a 68000 or 68010 ST/STE machine.

  A 68000 has no coprocessor interface, so programs talk to the FPU through
  its coprocessor interface registers (CIRs) themselves, doing in software
  what a 68020 does in hardware: write a command word, read the response
  primitives the FPU returns, and move operands through the operand
  register. The protocol is in chapter 7 of the MC68881/MC68882 User's
  Manual, and the registers are:

    $FFFA40  response          word, read
    $FFFA42  control           word, write
    $FFFA44  save              word, read
    $FFFA46  restore           word, read/write
    $FFFA48  operation word    word, write
    $FFFA4A  command           word, write
    $FFFA4E  condition         word, write
    $FFFA50  operand           long, read/write
    $FFFA54  register select   word, read
    $FFFA58  instruction addr  long, write
    $FFFA5C  operand address   long, read/write

  The arithmetic uses the softfloat library of the CPU core's 68881/68882
  emulation, with its own registers, so it doesn't depend on the CPU's FPU
  settings.

  Timing: an instruction starts when the response register is read after
  its command word was written, and the FPU then works on its own for as
  long as the manual's execution timing tables (8-2 and 8-3) give, at
  16 MHz. Until it is ready for the next step, the response register reads
  as the "come again" primitive $8900, so programs that poll it wait as
  long as they would on a real FPU, and the CPU runs concurrently with the
  FPU just as it does there. Each register access also costs a few wait
  states for the FPU's bus handshake at 16 MHz. With these, the benchmark
  and demos of the atarist-sfp004 library run within a few percent of the
  speed measured on a Mega STE with a 68882, at 8 MHz, 16 MHz and 16 MHz
  with cache, and its CIR trace reads the same.
*/
const char Sfp004_fileid[] = "Hatari sfp004.c";

#include "main.h"
#include "configuration.h"
#include "cycles.h"
#include "ioMem.h"
#include "log.h"
#include "m68000.h"
#include "memorySnapShot.h"
#include "sfp004.h"

#include "softfloat/softfloat.h"
#include "softfloat/softfloat-macros.h"
#include "softfloat/softfloat-specialize.h"


#define SFP004_BASE		0xfffa40

/* FPU clocks for the FPU to answer a bus cycle to a CIR (about 250 ns) */
#define SFP004_BUS_CLOCKS	4

/* Response primitives (MC68881/MC68882 User's Manual, 7.4.2) */
#define PRIM_CA			0x8000	/* come again */
#define PRIM_PC			0x4000	/* pass the program counter */
#define PRIM_DR			0x2000	/* transfer from the FPU */
#define PRIM_MULTI		0x0100	/* transfer multiple coprocessor registers */
#define PRIM_NULL		0x0800
#define PRIM_IA			0x0100	/* null: interrupts allowed */
#define PRIM_PF			0x0002	/* null: processing finished */
#define PRIM_MAIN_REG		0x0c00	/* transfer single main processor register */
#define PRIM_EVAL_EA		0x1000	/* evaluate <ea> and transfer data */
#define PRIM_PRE_EXC		0x1c00	/* take pre-instruction exception */
#define PRIM_MID_EXC		0x1d00	/* take mid-instruction exception */

#define RESP_IDLE		(PRIM_NULL | PRIM_PF)		/* $0802 */
#define RESP_BUSY		(PRIM_CA | PRIM_NULL | PRIM_IA)	/* $8900 */
#define RESP_RELEASE		(PRIM_NULL | PRIM_IA)		/* $0900 */

/* Valid <ea> classes of the evaluate <ea> primitive */
#define EA_DATA_ALTERABLE	0x0100
#define EA_MEMORY_ALTERABLE	0x0200
#define EA_ALTERABLE		0x0300
#define EA_DATA			0x0500
#define EA_MEMORY		0x0600
#define EA_ANY			0x0700

/* Save CIR format words */
#define SAVE_NULL		0x0018
#define SAVE_NOT_READY		0x0118
#define RESTORE_INVALID		0x0200
#define FPU_VERSION		0x1f

/* Exception vectors */
#define VEC_FLINE		11
#define VEC_BSUN		48
#define VEC_INEX		49
#define VEC_DZ			50
#define VEC_UNFL		51
#define VEC_OPERR		52
#define VEC_OVFL		53
#define VEC_SNAN		54

/* FPSR bits */
#define FPSR_CC_N		0x08000000
#define FPSR_CC_Z		0x04000000
#define FPSR_CC_I		0x02000000
#define FPSR_CC_NAN		0x01000000
#define FPSR_QUOT_SIGN		0x00800000
#define FPSR_QUOT		0x007f0000
#define FPSR_BSUN		0x00008000
#define FPSR_SNAN		0x00004000
#define FPSR_OPERR		0x00002000
#define FPSR_OVFL		0x00001000
#define FPSR_UNFL		0x00000800
#define FPSR_DZ			0x00000400
#define FPSR_INEX2		0x00000200
#define FPSR_INEX1		0x00000100
#define FPSR_AE_IOP		0x00000080
#define FPSR_AE_OVFL		0x00000040
#define FPSR_AE_UNFL		0x00000020
#define FPSR_AE_DZ		0x00000010
#define FPSR_AE_INEX		0x00000008
#define FPSR_MASK		0x0ffffff8
#define FPCR_MASK		0x0000fff0

/* Operand formats (source specifier field) */
enum { FMT_L, FMT_S, FMT_X, FMT_P, FMT_W, FMT_D, FMT_B, FMT_P_DYN };
static const int FormatSize[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };

enum {
	PHASE_IDLE,		/* no dialog in progress */
	PHASE_COMMAND,		/* command or condition written, starts on a response read */
	PHASE_IN,		/* expecting operand data from the CPU */
	PHASE_OUT,		/* operand data for the CPU to read */
	PHASE_CONVERT,		/* FMOVE out converting, data ready at data_ready */
	PHASE_PRIMITIVE,	/* next response read returns 'primitive' */
	PHASE_RELEASE,		/* transfers done, next response read ends the dialog */
	PHASE_EXCEPTION		/* take exception primitive until acknowledged */
};

/* What to do once an operand transfer completes */
enum {
	XFER_SOURCE,		/* source operand of an arithmetic instruction */
	XFER_KFACTOR,		/* dynamic k-factor of FMOVE.P out */
	XFER_CONTROL,		/* FMOVE(M) <ea>,FPcr */
	XFER_DYNLIST,		/* dynamic register list of FMOVEM */
	XFER_FMOVEM,		/* FMOVEM <ea>,FPn */
	XFER_RESTORE,		/* FRESTORE state frame */
	XFER_OUT_CA,		/* data read out, the CPU then reads the response again */
	XFER_OUT,		/* data read out, the dialog ends there */
	XFER_SAVE		/* FSAVE state frame read out */
};

static struct {
	floatx80 fp[8];
	uint32_t fpcr, fpsr, fpiar;
	bool null_state;	/* no instruction since reset or a null FRESTORE */
	int phase;
	bool cond;		/* the dialog is for the condition CIR */
	uint16_t cmd;		/* command word or conditional predicate */
	uint16_t primitive;	/* primitive returned once ready */
	int xfer;
	uint8_t buf[8*12];	/* operand transfer, up to 8 extended registers */
	int len, pos;
	uint8_t reglist;	/* FMOVEM register list (register select CIR) */
	int exc_vector;		/* pending exception, 0 = none */
	uint16_t exc_prim;	/* take exception primitive being returned */
	bool exc_mid;		/* it's a mid-instruction exception of an FMOVE out */
	uint16_t restore;	/* restore CIR read back value */
	uint32_t opword, opaddr;
	uint64_t accept_at;	/* clock from which the next instruction can start */
	uint64_t apu_free;	/* clock when the current calculation is finished */
	uint64_t data_ready;	/* clock when an FMOVE out conversion is finished */
} sfp;

static float_status fs;


/*-----------------------------------------------------------------------*/
/* Timing                                                                */
/*-----------------------------------------------------------------------*/

/* Execution times in FPU clocks for register source operands, from table
 * 8-2 (MC68881 total) and table 8-3 (MC68882 tail) of the user's manual,
 * and whether the instruction is dyadic (memory source adders differ). */
static const struct {
	uint16_t mc68881, mc68882;
	bool dyadic;
} OpTiming[0x40] = {
	[0x00] = {  33,   4, false },	/* FMOVE */
	[0x01] = {  55,  37, false },	/* FINT */
	[0x02] = { 687, 669, false },	/* FSINH */
	[0x03] = {  55,  37, false },	/* FINTRZ */
	[0x04] = { 107,  89, false },	/* FSQRT */
	[0x05] = { 107,  89, false },
	[0x06] = { 571, 553, false },	/* FLOGNP1 */
	[0x07] = { 571, 553, false },
	[0x08] = { 545, 527, false },	/* FETOXM1 */
	[0x09] = { 661, 643, false },	/* FTANH */
	[0x0a] = { 403, 385, false },	/* FATAN */
	[0x0b] = { 403, 385, false },
	[0x0c] = { 581, 563, false },	/* FASIN */
	[0x0d] = { 693, 675, false },	/* FATANH */
	[0x0e] = { 391, 373, false },	/* FSIN */
	[0x0f] = { 473, 455, false },	/* FTAN */
	[0x10] = { 497, 479, false },	/* FETOX */
	[0x11] = { 567, 549, false },	/* FTWOTOX */
	[0x12] = { 567, 549, false },	/* FTENTOX */
	[0x13] = { 567, 549, false },
	[0x14] = { 525, 507, false },	/* FLOGN */
	[0x15] = { 581, 563, false },	/* FLOG10 */
	[0x16] = { 581, 563, false },	/* FLOG2 */
	[0x17] = { 581, 563, false },
	[0x18] = {  35,  17, false },	/* FABS */
	[0x19] = { 607, 589, false },	/* FCOSH */
	[0x1a] = {  35,  17, false },	/* FNEG */
	[0x1b] = {  35,  17, false },
	[0x1c] = { 625, 607, false },	/* FACOS */
	[0x1d] = { 391, 373, false },	/* FCOS */
	[0x1e] = {  45,  27, false },	/* FGETEXP */
	[0x1f] = {  31,  13, false },	/* FGETMAN */
	[0x20] = { 103,  87, true },	/* FDIV */
	[0x21] = {  70,  54, true },	/* FMOD */
	[0x22] = {  51,  35, true },	/* FADD */
	[0x23] = {  71,  55, true },	/* FMUL */
	[0x24] = {  69,  53, true },	/* FSGLDIV */
	[0x25] = { 100,  84, true },	/* FREM */
	[0x26] = {  41,  25, true },	/* FSCALE */
	[0x27] = {  59,  43, true },	/* FSGLMUL */
	[0x28] = {  51,  35, true },	/* FSUB */
	[0x29] = {  51,  35, true },
	[0x2a] = {  51,  35, true },
	[0x2b] = {  51,  35, true },
	[0x2c] = {  51,  35, true },
	[0x2d] = {  51,  35, true },
	[0x2e] = {  51,  35, true },
	[0x2f] = {  51,  35, true },
	[0x30] = { 451, 433, false },	/* FSINCOS */
	[0x31] = { 451, 433, false },
	[0x32] = { 451, 433, false },
	[0x33] = { 451, 433, false },
	[0x34] = { 451, 433, false },
	[0x35] = { 451, 433, false },
	[0x36] = { 451, 433, false },
	[0x37] = { 451, 433, false },
	[0x38] = {  33,  17, true },	/* FCMP */
	[0x39] = {  33,  17, true },
	[0x3a] = {  33,  15, false },	/* FTST */
	[0x3b] = {  33,  15, false },
	[0x3c] = {  33,  17, true },
	[0x3d] = {  33,  17, true },
	[0x3e] = {  33,  15, false },
	[0x3f] = {  33,  15, false },
};

/* FMOVE FPn,<ea> conversion, by destination format: the "FMOVE to memory"
 * totals of tables 8-2 and 8-3 less the bus cycles of the dialog */
static const uint16_t MoveOutClocks68881[8] = { 86, 66, 58, 1988, 86, 72, 86, 2002 };
static const uint16_t MoveOutClocks68882[8] = { 96, 24, 36, 1992, 96, 30, 96, 2006 };

static bool Sfp004_Is68882(void)
{
	return ConfigureParams.System.n_FPUType == FPU_68882;
}

/* The CPU's clock counter, in CPU cycles at the current CPU clock */
static uint64_t Sfp004_Now(void)
{
	return Cycles_GetClockCounterImmediate();
}

/* FPU clocks (16 MHz) to CPU cycles */
static uint64_t Sfp004_Cycles(int clocks)
{
	return ((uint64_t)clocks << nCpuFreqShift) / 2;
}

/**
 * FPU clocks that an arithmetic instruction keeps the FPU busy after it has
 * its operands, for a register (fmt < 0) or memory source operand. For the
 * MC68882 this is the tail; the head is the dialog the CPU goes through.
 * The MC68881 can't overlap two instructions, so it is busy for the total
 * less the MC68882's head.
 */
static int Sfp004_OpClocks(int opmode, int fmt)
{
	static const int head[8] = { 21, 30, 42, 13, 21, 36, 21, 13 };
	bool dyadic = OpTiming[opmode].dyadic;
	int clocks;

	if (Sfp004_Is68882())
	{
		clocks = OpTiming[opmode].mc68882;
		if (fmt == FMT_L || fmt == FMT_W || fmt == FMT_B)
			clocks += (opmode == 0) ? 4 : dyadic ? 19 : 11;
		else if (fmt == FMT_S || fmt == FMT_D || fmt == FMT_X)
			clocks += (opmode == 0) ? 0 : 3;
		else if (fmt == FMT_P)
			clocks += (opmode == 0) ? 805 : 793;
		return clocks;
	}

	clocks = OpTiming[opmode].mc68881;
	switch (fmt)
	{
	 case FMT_L: case FMT_W: case FMT_B: clocks += dyadic ? 29 : 27; break;
	 case FMT_S: clocks += dyadic ? 21 : 19; break;
	 case FMT_D: clocks += dyadic ? 27 : 25; break;
	 case FMT_X: clocks += dyadic ? 25 : 23; break;
	 case FMT_P: clocks += 837; break;
	}
	return clocks - (fmt < 0 ? 17 : head[fmt]);
}

/**
 * Charge the CPU for the FPU's bus handshake on a CIR access: the 68000
 * looks for DTACK two of its clocks into a bus cycle, so it waits for as
 * much longer as the FPU takes to answer (no time at 8 MHz, at 16 MHz
 * the CPU is twice as quick to look).
 */
static void Sfp004_BusCycle(void)
{
	int words = (nIoMemAccessSize + 1) / 2;
	int wait = (int)Sfp004_Cycles(SFP004_BUS_CLOCKS) - 2;

	if (wait > 0)
		M68000_WaitState(wait * words);
}


/*-----------------------------------------------------------------------*/
/* Status and exceptions                                                 */
/*-----------------------------------------------------------------------*/

static void Sfp004_SetMode(void)
{
	set_float_detect_tininess(float_tininess_before_rounding, &fs);
	switch (sfp.fpcr & 0xc0)
	{
	 case 0x40: set_floatx80_rounding_precision(32, &fs); break;
	 case 0x00: set_floatx80_rounding_precision(80, &fs); break;
	 default:   set_floatx80_rounding_precision(64, &fs); break;
	}
	switch (sfp.fpcr & 0x30)
	{
	 case 0x00: set_float_rounding_mode(float_round_nearest_even, &fs); break;
	 case 0x10: set_float_rounding_mode(float_round_to_zero, &fs); break;
	 case 0x20: set_float_rounding_mode(float_round_down, &fs); break;
	 case 0x30: set_float_rounding_mode(float_round_up, &fs); break;
	}
}

/* Start an instruction's status: clear the exception status byte */
static void Sfp004_ClearStatus(void)
{
	sfp.fpsr &= ~0xff00;
	fs.float_exception_flags = 0;
}

/* Set the exception status and accrued exception bytes from softfloat */
static void Sfp004_MakeStatus(void)
{
	uint8_t flags = fs.float_exception_flags;

	if (flags & float_flag_signaling)
		sfp.fpsr |= FPSR_SNAN;
	if (flags & float_flag_invalid)
		sfp.fpsr |= FPSR_OPERR;
	if (flags & float_flag_divbyzero)
		sfp.fpsr |= FPSR_DZ;
	if (flags & float_flag_overflow)
		sfp.fpsr |= FPSR_OVFL;
	if (flags & float_flag_underflow)
		sfp.fpsr |= FPSR_UNFL;
	if (flags & float_flag_inexact)
		sfp.fpsr |= FPSR_INEX2;
	if (flags & float_flag_decimal)
		sfp.fpsr |= FPSR_INEX1;

	if (sfp.fpsr & (FPSR_BSUN | FPSR_SNAN | FPSR_OPERR))
		sfp.fpsr |= FPSR_AE_IOP;
	if (sfp.fpsr & FPSR_OVFL)
		sfp.fpsr |= FPSR_AE_OVFL;
	if ((sfp.fpsr & FPSR_UNFL) && (sfp.fpsr & FPSR_INEX2))
		sfp.fpsr |= FPSR_AE_UNFL;
	if (sfp.fpsr & FPSR_DZ)
		sfp.fpsr |= FPSR_AE_DZ;
	if (sfp.fpsr & (FPSR_OVFL | FPSR_INEX2 | FPSR_INEX1))
		sfp.fpsr |= FPSR_AE_INEX;
}

/* Condition code byte from a result */
static void Sfp004_SetResult(floatx80 res)
{
	sfp.fpsr &= ~0xff000000;
	if (floatx80_is_negative(res))
		sfp.fpsr |= FPSR_CC_N;
	if (floatx80_is_any_nan(res))
		sfp.fpsr |= FPSR_CC_NAN;
	else if (floatx80_is_zero(res))
		sfp.fpsr |= FPSR_CC_Z;
	else if (floatx80_is_infinity(res))
		sfp.fpsr |= FPSR_CC_I;
}

/* Vector of the highest priority enabled exception, or 0 */
static int Sfp004_EnabledException(void)
{
	static const struct { uint32_t bit; int vector; } prio[] = {
		{ FPSR_BSUN, VEC_BSUN }, { FPSR_SNAN, VEC_SNAN },
		{ FPSR_OPERR, VEC_OPERR }, { FPSR_OVFL, VEC_OVFL },
		{ FPSR_UNFL, VEC_UNFL }, { FPSR_DZ, VEC_DZ },
		{ FPSR_INEX2 | FPSR_INEX1, VEC_INEX }
	};
	uint32_t enabled = sfp.fpsr & sfp.fpcr & 0xff00;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(prio); i++)
	{
		if (enabled & prio[i].bit)
			return prio[i].vector;
	}
	return 0;
}


/*-----------------------------------------------------------------------*/
/* Data formats                                                          */
/*-----------------------------------------------------------------------*/

static uint64_t Sfp004_Get64(uint8_t *p)
{
	return ((uint64_t)do_get_mem_long(p) << 32) | do_get_mem_long(p + 4);
}

static void Sfp004_Put64(uint8_t *p, uint64_t v)
{
	do_put_mem_long(p, v >> 32);
	do_put_mem_long(p + 4, (uint32_t)v);
}

static floatx80 Sfp004_Normalize(floatx80 f)
{
	if (floatx80_is_unnormal(f) || floatx80_is_denormal(f))
		f = floatx80_normalize(f);
	return f;
}

/**
 * Packed decimal to extended (from the CPU core's fpp_softfloat.c)
 */
static floatx80 Sfp004_FromPacked(const uint32_t *wrd)
{
	floatx80 f;
	int32_t exp = 0;
	int64_t mant;
	uint32_t pack_exp, pack_int, pack_se, pack_sm;
	uint64_t pack_frac;
	int i;

	if (((wrd[0] >> 16) & 0x7fff) == 0x7fff)
	{
		/* infinity has extended exponent and all 0 packed fraction,
		 * NaNs are copied bit by bit */
		f.high = wrd[0] >> 16;
		f.low = ((uint64_t)wrd[1] << 32) | wrd[2];
		return f;
	}
	if (!(wrd[0] & 0xf) && !wrd[1] && !wrd[2])
	{
		/* exponent doesn't matter if mantissa is zero */
		f.high = (wrd[0] & 0x80000000) >> 16;
		f.low = 0;
		return f;
	}

	pack_exp = (wrd[0] >> 16) & 0xfff;
	pack_int = wrd[0] & 0xf;
	pack_frac = ((uint64_t)wrd[1] << 32) | wrd[2];
	pack_se = (wrd[0] >> 30) & 1;
	pack_sm = (wrd[0] >> 31) & 1;

	for (i = 0; i < 3; i++)
	{
		exp *= 10;
		exp += (pack_exp >> (8 - i * 4)) & 0xf;
	}
	if (pack_se)
		exp = -exp;
	exp -= 16;
	if (exp < 0)
	{
		exp = -exp;
		pack_se = 1;
	}

	mant = pack_int;
	for (i = 0; i < 16; i++)
	{
		mant *= 10;
		mant += (pack_frac >> (60 - i * 4)) & 0xf;
	}

	f.high = exp & 0x3fff;
	f.high |= pack_se ? 0x4000 : 0;
	f.high |= pack_sm ? 0x8000 : 0;
	f.low = mant;

	return floatdecimal_to_floatx80(f, &fs);
}

/**
 * Extended to packed decimal (from the CPU core's fpp_softfloat.c)
 */
static void Sfp004_ToPacked(floatx80 src, int kfactor, uint32_t *wrd)
{
	floatx80 f = floatx80_to_floatdecimal(Sfp004_Normalize(src), &kfactor, &fs);
	uint32_t pack_exp = 0, pack_exp4 = 0, pack_int = 0;
	uint64_t pack_frac = 0, significand, digit;
	uint32_t exponent;
	int32_t len;

	if ((f.high & 0x7fff) == 0x7fff)
	{
		wrd[0] = (uint32_t)f.high << 16;
		wrd[1] = f.low >> 32;
		wrd[2] = (uint32_t)f.low;
		return;
	}

	exponent = f.high & 0x3fff;
	significand = f.low;

	len = kfactor;		/* softfloat returns the length in kfactor */
	while (len > 0)
	{
		len--;
		digit = significand % 10;
		significand /= 10;
		if (len == 0)
			pack_int = (uint32_t)digit;
		else
			pack_frac |= digit << (64 - len * 4);
	}

	len = 4;
	while (len > 0)
	{
		len--;
		digit = exponent % 10;
		exponent /= 10;
		if (len == 0)
			pack_exp4 = (uint32_t)digit;
		else
			pack_exp |= digit << (12 - len * 4);
	}

	wrd[0] = pack_exp << 16;
	wrd[0] |= pack_exp4 << 12;
	wrd[0] |= pack_int;
	wrd[0] |= (f.high & 0x4000) ? 0x40000000 : 0;
	wrd[0] |= (f.high & 0x8000) ? 0x80000000 : 0;
	wrd[1] = pack_frac >> 32;
	wrd[2] = pack_frac & 0xffffffff;
}

/**
 * Convert an operand written to the operand CIR to extended precision
 */
static floatx80 Sfp004_Load(int fmt, uint8_t *p)
{
	floatx80 f;
	uint32_t wrd[3];

	switch (fmt)
	{
	 case FMT_L:
		return int32_to_floatx80((int32_t)do_get_mem_long(p));
	 case FMT_W:
		return int32_to_floatx80((int16_t)do_get_mem_word(p));
	 case FMT_B:
		return int32_to_floatx80((int8_t)p[0]);
	 case FMT_S:
		return Sfp004_Normalize(float32_to_floatx80_allowunnormal(do_get_mem_long(p), &fs));
	 case FMT_D:
		return Sfp004_Normalize(float64_to_floatx80_allowunnormal(Sfp004_Get64(p), &fs));
	 case FMT_X:
		f.high = do_get_mem_word(p);
		f.low = Sfp004_Get64(p + 4);
		return Sfp004_Normalize(f);
	 default:	/* packed */
		wrd[0] = do_get_mem_long(p);
		wrd[1] = do_get_mem_long(p + 4);
		wrd[2] = do_get_mem_long(p + 8);
		return Sfp004_Normalize(Sfp004_FromPacked(wrd));
	}
}

/**
 * Convert a register for reading from the operand CIR, return its size
 */
static int Sfp004_Store(int fmt, floatx80 v, int kfactor, uint8_t *p)
{
	floatx80 f;
	uint32_t wrd[3];

	switch (fmt)
	{
	 case FMT_L:
		do_put_mem_long(p, floatx80_to_int32(Sfp004_Normalize(v), &fs));
		return 4;
	 case FMT_W:
		do_put_mem_word(p, floatx80_to_int16(Sfp004_Normalize(v), &fs));
		return 2;
	 case FMT_B:
		p[0] = floatx80_to_int8(Sfp004_Normalize(v), &fs);
		return 1;
	 case FMT_S:
		do_put_mem_long(p, floatx80_to_float32(Sfp004_Normalize(v), &fs));
		return 4;
	 case FMT_D:
		Sfp004_Put64(p, floatx80_to_float64(Sfp004_Normalize(v), &fs));
		return 8;
	 case FMT_X:
		f = floatx80_to_floatx80(v, &fs);
		do_put_mem_long(p, (uint32_t)f.high << 16);
		Sfp004_Put64(p + 4, f.low);
		return 12;
	 default:	/* packed */
		Sfp004_ToPacked(v, kfactor, wrd);
		do_put_mem_long(p, wrd[0]);
		do_put_mem_long(p + 4, wrd[1]);
		do_put_mem_long(p + 8, wrd[2]);
		return 12;
	}
}


/*-----------------------------------------------------------------------*/
/* Instructions                                                          */
/*-----------------------------------------------------------------------*/

/* FMOVECR constants, with their rounding (from the CPU core's fpp.c) */
static const struct {
	uint32_t val[3];
	bool inexact;
	int8_t rndoff[4];
} RomConstant[22] = {
	{ {0x40000000, 0xc90fdaa2, 0x2168c235}, 1, {0,-1,-1, 0} },	/* pi */
	{ {0x3ffd0000, 0x9a209a84, 0xfbcff798}, 1, {0, 0, 0, 1} },	/* log10(2) */
	{ {0x40000000, 0xadf85458, 0xa2bb4a9a}, 1, {0, 0, 0, 1} },	/* e */
	{ {0x3fff0000, 0xb8aa3b29, 0x5c17f0bc}, 1, {0,-1,-1, 0} },	/* log2(e) */
	{ {0x3ffd0000, 0xde5bd8a9, 0x37287195}, 0, {0, 0, 0, 0} },	/* log10(e) */
	{ {0x00000000, 0x00000000, 0x00000000}, 0, {0, 0, 0, 0} },	/* 0.0 */
	{ {0x3ffe0000, 0xb17217f7, 0xd1cf79ac}, 1, {0,-1,-1, 0} },	/* ln(2) */
	{ {0x40000000, 0x935d8ddd, 0xaaa8ac17}, 1, {0,-1,-1, 0} },	/* ln(10) */
	{ {0x3fff0000, 0x80000000, 0x00000000}, 0, {0, 0, 0, 0} },	/* 1e0 */
	{ {0x40020000, 0xa0000000, 0x00000000}, 0, {0, 0, 0, 0} },	/* 1e1 */
	{ {0x40050000, 0xc8000000, 0x00000000}, 0, {0, 0, 0, 0} },	/* 1e2 */
	{ {0x400c0000, 0x9c400000, 0x00000000}, 0, {0, 0, 0, 0} },	/* 1e4 */
	{ {0x40190000, 0xbebc2000, 0x00000000}, 0, {0, 0, 0, 0} },	/* 1e8 */
	{ {0x40340000, 0x8e1bc9bf, 0x04000000}, 0, {0, 0, 0, 0} },	/* 1e16 */
	{ {0x40690000, 0x9dc5ada8, 0x2b70b59e}, 1, {0,-1,-1, 0} },	/* 1e32 */
	{ {0x40d30000, 0xc2781f49, 0xffcfa6d5}, 1, {0, 0, 0, 1} },	/* 1e64 */
	{ {0x41a80000, 0x93ba47c9, 0x80e98ce0}, 1, {0,-1,-1, 0} },	/* 1e128 */
	{ {0x43510000, 0xaa7eebfb, 0x9df9de8e}, 1, {0,-1,-1, 0} },	/* 1e256 */
	{ {0x46a30000, 0xe319a0ae, 0xa60e91c7}, 1, {0,-1,-1, 0} },	/* 1e512 */
	{ {0x4d480000, 0xc9767586, 0x81750c17}, 1, {0, 0, 0, 1} },	/* 1e1024 */
	{ {0x5a920000, 0x9e8b3b5d, 0xc53d5de5}, 1, {0,-1,-1, 0} },	/* 1e2048 */
	{ {0x75250000, 0xc4605202, 0x8a20979b}, 1, {0,-1,-1, 0} }	/* 1e4096 */
};

static floatx80 Sfp004_RomConstant(int offset)
{
	floatx80 f;
	uint32_t low;
	int entry;

	if (offset == 0x00)
		entry = 0;
	else if (offset >= 0x0b && offset <= 0x0f)
		entry = offset - 0x0b + 1;
	else if (offset >= 0x30 && offset <= 0x3f)
		entry = offset - 0x30 + 6;
	else
		entry = 5;	/* undefined offsets read as 0.0 here */

	low = RomConstant[entry].val[2];
	if (RomConstant[entry].inexact)
	{
		fs.float_exception_flags |= float_flag_inexact;
		low += RomConstant[entry].rndoff[(sfp.fpcr >> 4) & 3];
	}
	f.high = RomConstant[entry].val[0] >> 16;
	f.low = ((uint64_t)RomConstant[entry].val[1] << 32) | low;

	switch (sfp.fpcr & 0xc0)
	{
	 case 0x40: f = floatx80_round32(f, &fs); break;
	 case 0x80: case 0xc0: f = floatx80_round64(f, &fs); break;
	}
	return f;
}

/**
 * Do an arithmetic instruction: dst = FPn op src. Status must have been
 * cleared before converting the source operand.
 */
static void Sfp004_Calculate(int opmode, floatx80 src, int reg)
{
	floatx80 dst = sfp.fp[reg];
	floatx80 cosine;
	uint64_t quot = 0;
	flag sign = 0;
	bool quotient = false;

	src = Sfp004_Normalize(src);
	dst = Sfp004_Normalize(dst);

	switch (opmode)
	{
	 case 0x00: dst = floatx80_move(src, &fs); break;
	 case 0x01: dst = floatx80_round_to_int(src, &fs); break;
	 case 0x02: dst = floatx80_sinh(src, &fs); break;
	 case 0x03: dst = floatx80_round_to_int_toward_zero(src, &fs); break;
	 case 0x04: case 0x05: dst = floatx80_sqrt(src, &fs); break;
	 case 0x06: case 0x07: dst = floatx80_lognp1(src, &fs); break;
	 case 0x08: dst = floatx80_etoxm1(src, &fs); break;
	 case 0x09: dst = floatx80_tanh(src, &fs); break;
	 case 0x0a: case 0x0b: dst = floatx80_atan(src, &fs); break;
	 case 0x0c: dst = floatx80_asin(src, &fs); break;
	 case 0x0d: dst = floatx80_atanh(src, &fs); break;
	 case 0x0e: dst = floatx80_sin(src, &fs); break;
	 case 0x0f: dst = floatx80_tan(src, &fs); break;
	 case 0x10: dst = floatx80_etox(src, &fs); break;
	 case 0x11: dst = floatx80_twotox(src, &fs); break;
	 case 0x12: case 0x13: dst = floatx80_tentox(src, &fs); break;
	 case 0x14: dst = floatx80_logn(src, &fs); break;
	 case 0x15: dst = floatx80_log10(src, &fs); break;
	 case 0x16: case 0x17: dst = floatx80_log2(src, &fs); break;
	 case 0x18: dst = floatx80_abs(src, &fs); break;
	 case 0x19: dst = floatx80_cosh(src, &fs); break;
	 case 0x1a: case 0x1b: dst = floatx80_neg(src, &fs); break;
	 case 0x1c: dst = floatx80_acos(src, &fs); break;
	 case 0x1d: dst = floatx80_cos(src, &fs); break;
	 case 0x1e: dst = floatx80_getexp(src, &fs); break;
	 case 0x1f: dst = floatx80_getman(src, &fs); break;
	 case 0x20: dst = floatx80_div(dst, src, &fs); break;
	 case 0x21:
		dst = floatx80_mod(dst, src, &quot, &sign, &fs);
		quotient = true;
		break;
	 case 0x22: dst = floatx80_add(dst, src, &fs); break;
	 case 0x23: dst = floatx80_mul(dst, src, &fs); break;
	 case 0x24: dst = floatx80_sgldiv(dst, src, &fs); break;
	 case 0x25:
		dst = floatx80_rem(dst, src, &quot, &sign, &fs);
		quotient = true;
		break;
	 case 0x26: dst = floatx80_scale(dst, src, &fs); break;
	 case 0x27: dst = floatx80_sglmul(dst, src, &fs); break;
	 case 0x30: case 0x31: case 0x32: case 0x33:
	 case 0x34: case 0x35: case 0x36: case 0x37:
		dst = floatx80_sincos(src, &cosine, &fs);
		sfp.fp[opmode & 7] = cosine;
		break;
	 case 0x38: case 0x39: case 0x3c: case 0x3d:	/* FCMP */
		Sfp004_SetResult(floatx80_cmp(dst, src, &fs));
		Sfp004_MakeStatus();
		return;
	 case 0x3a: case 0x3b: case 0x3e: case 0x3f:	/* FTST */
		Sfp004_SetResult(floatx80_tst(src, &fs));
		Sfp004_MakeStatus();
		return;
	 default:	/* FSUB */
		dst = floatx80_sub(dst, src, &fs);
		break;
	}

	sfp.fp[reg] = dst;
	Sfp004_SetResult(dst);
	if (quotient)
	{
		sfp.fpsr &= ~(FPSR_QUOT_SIGN | FPSR_QUOT);
		sfp.fpsr |= (quot << 16) & FPSR_QUOT;
		sfp.fpsr |= sign ? FPSR_QUOT_SIGN : 0;
	}
	Sfp004_MakeStatus();
}

/**
 * Run an arithmetic instruction (opclass 000 or 010) once it has its
 * source operand, and keep the FPU busy for as long as it takes.
 */
static void Sfp004_Execute(floatx80 src, int fmt, uint64_t now)
{
	int opmode = sfp.cmd & 0x7f;
	uint64_t start = now > sfp.apu_free ? now : sfp.apu_free;

	if ((sfp.cmd & 0xfc00) == 0x5c00)	/* FMOVECR */
	{
		sfp.fp[(sfp.cmd >> 7) & 7] = src;
		Sfp004_SetResult(src);
		Sfp004_MakeStatus();
		sfp.apu_free = start + Sfp004_Cycles(Sfp004_Is68882() ? 22 : 12);
	}
	else
	{
		Sfp004_Calculate(opmode, src, (sfp.cmd >> 7) & 7);
		sfp.apu_free = start + Sfp004_Cycles(Sfp004_OpClocks(opmode, fmt));
	}

	/* The MC68882 takes the next instruction once this one has moved
	 * on to its arithmetic unit, the MC68881 once it is finished */
	sfp.accept_at = Sfp004_Is68882() ? start : sfp.apu_free;
	sfp.exc_vector = Sfp004_EnabledException();

	LOG_TRACE(TRACE_SFP004, "sfp004: cmd $%04x fpsr $%08x busy %d cycles\n",
	          sfp.cmd, sfp.fpsr, (int)(sfp.apu_free - start));
}

/**
 * Start converting a register for FMOVE FPn,<ea>
 */
static void Sfp004_StartMoveOut(int kfactor, uint64_t now)
{
	int fmt = (sfp.cmd >> 10) & 7;
	floatx80 src = sfp.fp[(sfp.cmd >> 7) & 7];
	bool ca = true;
	int clocks, vector;

	clocks = Sfp004_Is68882() ? MoveOutClocks68882[fmt] : MoveOutClocks68881[fmt];
	if (sfp.apu_free > now)
		now = sfp.apu_free;	/* wait for the calculation of the result */
	sfp.data_ready = sfp.apu_free = sfp.accept_at = now + Sfp004_Cycles(clocks);

	Sfp004_ClearStatus();
	sfp.len = Sfp004_Store(fmt == FMT_P_DYN ? FMT_P : fmt, src, kfactor, sfp.buf);
	sfp.pos = 0;
	Sfp004_MakeStatus();
	vector = Sfp004_EnabledException();

	/* The MC68882 lets the CPU go after the transfer of a single, double
	 * or extended result, unless it needs to report something */
	if (Sfp004_Is68882() && (fmt == FMT_S || fmt == FMT_D || fmt == FMT_X)
	    && !floatx80_is_any_nan(src) && !floatx80_is_unnormal(src)
	    && !floatx80_is_denormal(src)
	    && (fmt == FMT_X || (!(sfp.fpsr & (FPSR_OVFL | FPSR_UNFL))
	                         && !(sfp.fpcr & FPSR_INEX2))))
		ca = false;

	sfp.primitive = (ca ? PRIM_CA : 0) | PRIM_DR | PRIM_EVAL_EA | sfp.len
	                | (sfp.len <= 4 ? EA_DATA_ALTERABLE : EA_MEMORY_ALTERABLE);
	sfp.xfer = ca ? XFER_OUT_CA : XFER_OUT;
	sfp.exc_vector = vector;
	sfp.exc_mid = vector != 0;
	sfp.exc_prim = PRIM_MID_EXC | vector;
	sfp.phase = PHASE_CONVERT;
}

/**
 * Have the CPU take an exception, until it acknowledges it
 */
static uint16_t Sfp004_TakeException(uint16_t prim)
{
	sfp.phase = PHASE_EXCEPTION;
	sfp.exc_prim = prim;
	sfp.exc_mid = false;
	return prim;
}

/**
 * Set up the transfer of the registers in an FMOVEM list
 */
static uint16_t Sfp004_StartFmovem(uint8_t list)
{
	bool predec = !(sfp.cmd & 0x1000);
	int count = 0, i, reg;

	sfp.reglist = list;
	for (i = 7; i >= 0; i--)
	{
		if (!(list & (1 << i)))
			continue;
		if (sfp.cmd & 0x2000)	/* registers to memory */
		{
			reg = predec ? i : 7 - i;
			do_put_mem_long(sfp.buf + count * 12, (uint32_t)sfp.fp[reg].high << 16);
			Sfp004_Put64(sfp.buf + count * 12 + 4, sfp.fp[reg].low);
		}
		count++;
	}
	sfp.len = count * 12;
	sfp.pos = 0;
	if (sfp.cmd & 0x2000)
	{
		sfp.phase = PHASE_OUT;
		sfp.xfer = XFER_OUT_CA;
		return PRIM_CA | PRIM_DR | PRIM_MULTI | 12;
	}
	sfp.phase = PHASE_IN;
	sfp.xfer = XFER_FMOVEM;
	return PRIM_CA | PRIM_MULTI | 12;
}

/**
 * Load the registers transferred for FMOVEM <ea>,list
 */
static void Sfp004_FinishFmovem(void)
{
	bool predec = !(sfp.cmd & 0x1000);
	int count = 0, i, reg;

	for (i = 7; i >= 0; i--)
	{
		if (!(sfp.reglist & (1 << i)))
			continue;
		reg = predec ? i : 7 - i;
		sfp.fp[reg].high = do_get_mem_word(sfp.buf + count * 12);
		sfp.fp[reg].low = Sfp004_Get64(sfp.buf + count * 12 + 4);
		count++;
	}
}

/* Number of control registers selected by an FMOVE(M) FPcr command */
static int Sfp004_ControlCount(void)
{
	int bits = (sfp.cmd >> 10) & 7;

	if (bits == 0)
		bits = 1;	/* none selected means FPIAR */
	return (bits & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1);
}

/**
 * Start a general (cpGEN) instruction, return its first primitive
 */
static uint16_t Sfp004_StartGeneral(uint64_t now)
{
	int opclass = sfp.cmd >> 13;
	int fmt = (sfp.cmd >> 10) & 7;
	bool arith = opclass == 0 || opclass == 2 || opclass == 3;
	uint16_t pc = (arith && (sfp.fpcr & 0xff00)) ? PRIM_PC : 0;
	uint16_t ea;
	uint32_t *ctrl[3] = { &sfp.fpcr, &sfp.fpsr, &sfp.fpiar };
	int n, i;

	sfp.null_state = false;

	if (arith && sfp.exc_vector)
	{
		/* exception pending from a previous instruction */
		return Sfp004_TakeException(PRIM_PRE_EXC | sfp.exc_vector);
	}
	if (opclass == 1 || ((opclass == 0 || (opclass == 2 && fmt != 7))
	                     && (sfp.cmd & 0x40)))
	{
		/* not an FPU instruction, or an MC68040 only opmode */
		return Sfp004_TakeException(PRIM_PRE_EXC | VEC_FLINE);
	}

	switch (opclass)
	{
	 case 0:	/* FPm,FPn */
		Sfp004_ClearStatus();
		Sfp004_Execute(sfp.fp[fmt], -1, now);
		sfp.phase = PHASE_IDLE;
		return RESP_RELEASE | pc;

	 case 2:	/* <ea>,FPn */
		if (fmt == 7)	/* FMOVECR */
		{
			Sfp004_ClearStatus();
			Sfp004_Execute(Sfp004_RomConstant(sfp.cmd & 0x7f), -1, now);
			sfp.phase = PHASE_IDLE;
			return RESP_RELEASE | pc;
		}
		sfp.len = FormatSize[fmt];
		sfp.pos = 0;
		sfp.xfer = XFER_SOURCE;
		sfp.phase = PHASE_IN;
		ea = (fmt == FMT_D || fmt == FMT_X || fmt == FMT_P) ? EA_MEMORY : EA_DATA;
		/* The MC68882 needs no response read after a single,
		 * double or extended operand */
		if (Sfp004_Is68882() && (fmt == FMT_S || fmt == FMT_D || fmt == FMT_X))
			return pc | PRIM_EVAL_EA | ea | sfp.len;
		return PRIM_CA | pc | PRIM_EVAL_EA | ea | sfp.len;

	 case 3:	/* FPn,<ea> */
		if (fmt == FMT_P_DYN)
		{
			sfp.len = 4;
			sfp.pos = 0;
			sfp.xfer = XFER_KFACTOR;
			sfp.phase = PHASE_IN;
			return PRIM_CA | pc | PRIM_MAIN_REG | ((sfp.cmd >> 4) & 7);
		}
		n = sfp.cmd & 0x7f;
		if (n & 0x40)
			n |= ~0x3f;	/* sign extend the static k-factor */
		Sfp004_StartMoveOut(n, now);
		return RESP_BUSY | pc;

	 case 4:	/* <ea>,FPcr */
		n = Sfp004_ControlCount();
		sfp.len = n * 4;
		sfp.pos = 0;
		sfp.xfer = XFER_CONTROL;
		sfp.phase = PHASE_IN;
		if (n == 1)
			ea = ((sfp.cmd >> 10) & 7) <= 1 ? EA_ANY : EA_DATA;
		else
			ea = EA_MEMORY;
		return PRIM_CA | PRIM_EVAL_EA | ea | sfp.len;

	 case 5:	/* FPcr,<ea> */
		if (!(sfp.cmd & 0x1c00))
			sfp.cmd |= 0x0400;
		n = 0;
		for (i = 0; i < 3; i++)
		{
			if (sfp.cmd & (0x1000 >> i))
				do_put_mem_long(sfp.buf + 4 * n++, *ctrl[i]);
		}
		sfp.len = n * 4;
		sfp.pos = 0;
		sfp.xfer = XFER_OUT_CA;
		sfp.phase = PHASE_OUT;
		if (n == 1)
			ea = (sfp.cmd & 0x1c00) == 0x0400 ? EA_ALTERABLE : EA_DATA_ALTERABLE;
		else
			ea = EA_MEMORY_ALTERABLE;
		return PRIM_CA | PRIM_DR | PRIM_EVAL_EA | ea | sfp.len;

	 default:	/* FMOVEM */
		if (sfp.cmd & 0x0800)	/* dynamic list in a data register */
		{
			sfp.len = 4;
			sfp.pos = 0;
			sfp.xfer = XFER_DYNLIST;
			sfp.phase = PHASE_IN;
			return PRIM_CA | PRIM_MAIN_REG | ((sfp.cmd >> 4) & 7);
		}
		return Sfp004_StartFmovem(sfp.cmd & 0xff);
	}
}

/* MC68881/MC68882 conditional predicates by condition code (N Z I NaN),
 * from the CPU core's fpp.c */
static const bool ConditionTable[16][32] =
{
	{ 0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1 },
	{ 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1 },
	{ 0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1 },
	{ 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1 },
	{ 0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1 },
	{ 0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1,0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1 },
	{ 0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1 },
	{ 0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1,0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1 },
	{ 0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1 },
	{ 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1 },
	{ 0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1 },
	{ 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1 },
	{ 0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1 },
	{ 0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1,0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1 },
	{ 0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1 },
	{ 0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1,0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1 }
};

/**
 * Evaluate the predicate written to the condition CIR, return the primitive
 */
static uint16_t Sfp004_StartCondition(void)
{
	int pred = sfp.cmd & 0x3f;

	sfp.null_state = false;
	sfp.phase = PHASE_IDLE;

	if (sfp.exc_vector)
		return Sfp004_TakeException(PRIM_PRE_EXC | sfp.exc_vector);
	if (pred & 0x20)
		return Sfp004_TakeException(PRIM_PRE_EXC | VEC_FLINE);
	if ((pred & 0x10) && (sfp.fpsr & FPSR_CC_NAN))
	{
		/* IEEE non-aware predicate with an unordered result */
		sfp.fpsr |= FPSR_BSUN | FPSR_AE_IOP;
		if (sfp.fpcr & FPSR_BSUN)
			return Sfp004_TakeException(PRIM_PC | PRIM_PRE_EXC | VEC_BSUN);
	}
	return PRIM_NULL | ConditionTable[(sfp.fpsr >> 24) & 15][pred];
}


/*-----------------------------------------------------------------------*/
/* Coprocessor interface registers                                       */
/*-----------------------------------------------------------------------*/

static void Sfp004_SetWord(uint32_t addr, uint16_t val)
{
	IoMem_WriteWord(addr, val);
}

/**
 * Reset the FPU, as on a hardware reset or an FRESTORE of a null frame
 */
static void Sfp004_ResetFpu(uint64_t now)
{
	int i;

	for (i = 0; i < 8; i++)
	{
		sfp.fp[i].high = 0x7fff;
		sfp.fp[i].low = 0xffffffffffffffffULL;
	}
	sfp.fpcr = sfp.fpsr = sfp.fpiar = 0;
	sfp.null_state = true;
	sfp.phase = PHASE_IDLE;
	sfp.exc_vector = 0;
	sfp.exc_mid = false;
	sfp.len = sfp.pos = 0;
	sfp.accept_at = sfp.apu_free = sfp.data_ready = now;
	Sfp004_SetMode();
}

/**
 * Read the response CIR: this is where the FPU starts instructions and
 * hands out the next request of a dialog
 */
static void Sfp004_Response_ReadWord(void)
{
	uint64_t now;
	uint16_t resp;

	Sfp004_BusCycle();
	now = Sfp004_Now();

	switch (sfp.phase)
	{
	 case PHASE_COMMAND:
		if (sfp.cond || (sfp.cmd >> 13) >= 4)
		{
			/* needs the previous instruction to be finished */
			if (now < sfp.apu_free)
			{
				resp = RESP_BUSY;
				break;
			}
		}
		else if (now < sfp.accept_at)
		{
			resp = RESP_BUSY;
			break;
		}
		Sfp004_SetMode();
		resp = sfp.cond ? Sfp004_StartCondition() : Sfp004_StartGeneral(now);
		break;
	 case PHASE_CONVERT:
		if (now < sfp.data_ready)
		{
			resp = RESP_BUSY;
		}
		else if (sfp.exc_mid)
		{
			sfp.phase = PHASE_EXCEPTION;
			resp = sfp.exc_prim;
		}
		else
		{
			sfp.phase = PHASE_OUT;
			resp = sfp.primitive;
		}
		break;
	 case PHASE_PRIMITIVE:
		resp = sfp.primitive;
		sfp.phase = (sfp.xfer == XFER_OUT_CA) ? PHASE_OUT : PHASE_IN;
		break;
	 case PHASE_EXCEPTION:
		resp = sfp.exc_prim;
		break;
	 case PHASE_OUT:
		/* After a request to read a result that needs no response
		 * read afterwards, a real MC68882 is idle already */
		if (sfp.xfer == XFER_OUT)
		{
			resp = (now < sfp.apu_free) ? RESP_RELEASE : RESP_IDLE;
			break;
		}
		/* fall through */
	 case PHASE_IN:
		/* the request was already read, wait for the transfer */
		resp = RESP_BUSY;
		break;
	 case PHASE_RELEASE:
		sfp.phase = PHASE_IDLE;
		/* fall through */
	 default:
		resp = (now < sfp.apu_free) ? RESP_RELEASE : RESP_IDLE;
		break;
	}

	LOG_TRACE(TRACE_SFP004, "sfp004: response $%04x\n", resp);
	Sfp004_SetWord(SFP004_BASE + 0x00, resp);
}

/**
 * Write the control CIR: exception acknowledge (bit 1) or abort (bit 0)
 */
static void Sfp004_Control_WriteWord(void)
{
	uint16_t val = IoMem_ReadWord(SFP004_BASE + 0x02);

	Sfp004_BusCycle();
	LOG_TRACE(TRACE_SFP004, "sfp004: control $%04x\n", val);

	if (val & 1)
	{
		sfp.phase = PHASE_IDLE;
	}
	else if ((val & 2) && sfp.phase == PHASE_EXCEPTION)
	{
		/* a reported arithmetic exception is no longer pending */
		if ((sfp.exc_prim & 0xff) == sfp.exc_vector)
			sfp.exc_vector = 0;
		if (sfp.exc_mid)
		{
			/* the FMOVE out goes on with the transfer */
			sfp.exc_mid = false;
			sfp.phase = PHASE_CONVERT;
		}
		else
		{
			sfp.phase = PHASE_IDLE;
		}
	}
}

/**
 * Read the save CIR: start of FSAVE
 */
static void Sfp004_Save_ReadWord(void)
{
	uint16_t format;
	uint32_t biu;
	int size;

	Sfp004_BusCycle();

	if (Sfp004_Now() < sfp.apu_free)
	{
		format = SAVE_NOT_READY;
	}
	else if (sfp.null_state)
	{
		format = SAVE_NULL;
		sfp.phase = PHASE_IDLE;
	}
	else
	{
		/* idle frame: command word, internal state (MC68882), the
		 * exceptional operand, the operand register and BIU flags */
		size = Sfp004_Is68882() ? 0x38 : 0x18;
		format = (FPU_VERSION << 8) | size;
		memset(sfp.buf, 0, size);
		do_put_mem_long(sfp.buf, ((uint32_t)sfp.cmd << 16) | 0xffff);
		biu = 0x1c00ffff;
		if (sfp.exc_vector)
			biu &= ~0x08000000;	/* exception pending */
		do_put_mem_long(sfp.buf + size - 4, biu);
		sfp.exc_vector = 0;
		sfp.len = size;
		sfp.pos = 0;
		sfp.xfer = XFER_SAVE;
		sfp.phase = PHASE_OUT;
	}

	LOG_TRACE(TRACE_SFP004, "sfp004: save $%04x\n", format);
	Sfp004_SetWord(SFP004_BASE + 0x04, format);
}

/**
 * Read the restore CIR: the format word written, or invalid
 */
static void Sfp004_Restore_ReadWord(void)
{
	Sfp004_BusCycle();
	Sfp004_SetWord(SFP004_BASE + 0x06, sfp.restore);
}

/**
 * Write the restore CIR: start of FRESTORE
 */
static void Sfp004_Restore_WriteWord(void)
{
	uint16_t format = IoMem_ReadWord(SFP004_BASE + 0x06);
	int version = format >> 8, size = format & 0xff;

	Sfp004_BusCycle();
	LOG_TRACE(TRACE_SFP004, "sfp004: restore $%04x\n", format);

	sfp.restore = format;
	if (version == 0)
	{
		Sfp004_ResetFpu(Sfp004_Now());
	}
	else if (version == FPU_VERSION &&
	         (size == (Sfp004_Is68882() ? 0x38 : 0x18) ||
	          size == (Sfp004_Is68882() ? 0xd4 : 0xb4)))
	{
		sfp.null_state = false;
		sfp.len = size;
		sfp.pos = 0;
		sfp.xfer = XFER_RESTORE;
		sfp.phase = PHASE_IN;
	}
	else
	{
		sfp.restore = RESTORE_INVALID;
		sfp.phase = PHASE_IDLE;
	}
}

/**
 * Write the command CIR: start of a general instruction
 */
static void Sfp004_Command_WriteWord(void)
{
	Sfp004_BusCycle();
	sfp.cmd = IoMem_ReadWord(SFP004_BASE + 0x0a);
	sfp.cond = false;
	sfp.phase = PHASE_COMMAND;
	LOG_TRACE(TRACE_SFP004, "sfp004: command $%04x\n", sfp.cmd);
}

/**
 * Write the condition CIR: start of a conditional instruction
 */
static void Sfp004_Condition_WriteWord(void)
{
	Sfp004_BusCycle();
	sfp.cmd = IoMem_ReadWord(SFP004_BASE + 0x0e);
	sfp.cond = true;
	sfp.phase = PHASE_COMMAND;
	LOG_TRACE(TRACE_SFP004, "sfp004: condition $%04x\n", sfp.cmd);
}

/**
 * All operand bytes were written: carry on with the instruction
 */
static void Sfp004_FinishIn(uint64_t now)
{
	uint32_t *ctrl[3] = { &sfp.fpcr, &sfp.fpsr, &sfp.fpiar };
	static const uint32_t mask[3] = { FPCR_MASK, FPSR_MASK, 0xffffffff };
	int fmt = (sfp.cmd >> 10) & 7;
	int n, i, k;

	switch (sfp.xfer)
	{
	 case XFER_SOURCE:
		Sfp004_ClearStatus();
		Sfp004_Execute(Sfp004_Load(fmt, sfp.buf), fmt, now);
		if (Sfp004_Is68882() && (fmt == FMT_S || fmt == FMT_D || fmt == FMT_X))
			sfp.phase = PHASE_IDLE;
		else
			sfp.phase = PHASE_RELEASE;
		break;
	 case XFER_KFACTOR:
		k = sfp.buf[3] & 0x7f;
		if (k & 0x40)
			k |= ~0x3f;
		Sfp004_StartMoveOut(k, now);
		break;
	 case XFER_CONTROL:
		if (!(sfp.cmd & 0x1c00))
			sfp.cmd |= 0x0400;
		n = 0;
		for (i = 0; i < 3; i++)
		{
			if (sfp.cmd & (0x1000 >> i))
			{
				*ctrl[i] = do_get_mem_long(sfp.buf + 4 * n++) & mask[i];
			}
		}
		Sfp004_SetMode();
		sfp.phase = PHASE_RELEASE;
		break;
	 case XFER_DYNLIST:
		sfp.primitive = Sfp004_StartFmovem(sfp.buf[3]);
		sfp.phase = PHASE_PRIMITIVE;
		break;
	 case XFER_FMOVEM:
		Sfp004_FinishFmovem();
		sfp.phase = PHASE_RELEASE;
		break;
	 case XFER_RESTORE:
		/* BIU flags bit 27 clear: an exception was pending */
		if (!(do_get_mem_long(sfp.buf + sfp.len - 4) & 0x08000000))
			sfp.exc_vector = Sfp004_EnabledException();
		sfp.phase = PHASE_IDLE;
		break;
	}
}

/**
 * Write the operand CIR, in bytes, words or longs
 */
static void Sfp004_Operand_Write(void)
{
	uint32_t addr = IoAccessCurrentAddress;
	uint32_t end = IoAccessBaseAddress + nIoMemAccessSize;

	Sfp004_BusCycle();

	/* a program that didn't read the response gets its request anyway */
	if (sfp.phase == PHASE_COMMAND || sfp.phase == PHASE_PRIMITIVE)
	{
		uint64_t now = Sfp004_Now();
		Sfp004_SetMode();
		if (sfp.phase == PHASE_PRIMITIVE)
			sfp.phase = PHASE_IN;
		else if (sfp.cond)
			Sfp004_StartCondition();
		else
			Sfp004_StartGeneral(now > sfp.accept_at ? now : sfp.accept_at);
	}
	if (sfp.phase != PHASE_IN)
		return;

	for (; addr < end && addr < SFP004_BASE + 0x14; addr++)
	{
		sfp.buf[sfp.pos++] = IoMem[addr];
		if (sfp.pos == sfp.len)
		{
			Sfp004_FinishIn(Sfp004_Now());
			break;
		}
	}
}

/**
 * Read the operand CIR, in bytes, words or longs
 */
static void Sfp004_Operand_Read(void)
{
	uint32_t addr = IoAccessCurrentAddress;
	uint32_t end = IoAccessBaseAddress + nIoMemAccessSize;

	Sfp004_BusCycle();

	/* a program that didn't read the response gets its data anyway */
	if (sfp.phase == PHASE_COMMAND && !sfp.cond)
	{
		uint64_t now = Sfp004_Now();
		Sfp004_SetMode();
		Sfp004_StartGeneral(now > sfp.accept_at ? now : sfp.accept_at);
	}
	if ((sfp.phase == PHASE_CONVERT && !sfp.exc_mid) || sfp.phase == PHASE_PRIMITIVE)
		sfp.phase = PHASE_OUT;

	for (; addr < end && addr < SFP004_BASE + 0x14; addr++)
	{
		if (sfp.phase != PHASE_OUT)
		{
			IoMem[addr] = 0xff;
			continue;
		}
		IoMem[addr] = sfp.buf[sfp.pos++];
		if (sfp.pos == sfp.len)
		{
			if (sfp.xfer == XFER_OUT_CA)
				sfp.phase = PHASE_RELEASE;
			else
				sfp.phase = PHASE_IDLE;
		}
	}
}

/**
 * Read the register select CIR: the FMOVEM register list
 */
static void Sfp004_RegSelect_ReadWord(void)
{
	Sfp004_BusCycle();
	Sfp004_SetWord(SFP004_BASE + 0x14, sfp.reglist << 8);
}

static void Sfp004_OpWord_WriteWord(void)
{
	Sfp004_BusCycle();
	sfp.opword = IoMem_ReadWord(SFP004_BASE + 0x08);
}

/**
 * Write the instruction address CIR: the CPU passes the PC for FPIAR
 */
static void Sfp004_InstAddr_Write(void)
{
	Sfp004_BusCycle();
	sfp.fpiar = IoMem_ReadLong(SFP004_BASE + 0x18);
}

static void Sfp004_OpAddr_Read(void)
{
	Sfp004_BusCycle();
	IoMem_WriteLong(SFP004_BASE + 0x1c, sfp.opaddr);
}

static void Sfp004_OpAddr_Write(void)
{
	Sfp004_BusCycle();
	sfp.opaddr = IoMem_ReadLong(SFP004_BASE + 0x1c);
}

/* Write-only and reserved registers */
static void Sfp004_Void_Read(void)
{
	uint32_t addr;

	Sfp004_BusCycle();
	for (addr = IoAccessBaseAddress; addr < IoAccessBaseAddress + nIoMemAccessSize; addr++)
		IoMem[addr] = 0xff;
}

static void Sfp004_Void_Write(void)
{
	Sfp004_BusCycle();
}


/*-----------------------------------------------------------------------*/
/* Interface                                                             */
/*-----------------------------------------------------------------------*/

/**
 * A 68000 or 68010 has no coprocessor interface, so on an ST, Mega ST,
 * STE or Mega STE a 68881/68882 is memory-mapped, as a real one would be
 */
bool Sfp004_IsEnabled(void)
{
	return (ConfigureParams.System.n_FPUType == FPU_68881
	        || ConfigureParams.System.n_FPUType == FPU_68882)
	       && ConfigureParams.System.nCpuLevel < 2
	       && (Config_IsMachineST() || Config_IsMachineSTE());
}

/**
 * Hook the coprocessor interface registers into the IO memory map
 */
void Sfp004_InterceptIoMem(void)
{
	static const struct {
		int offset, size;
		void (*read)(void);
		void (*write)(void);
	} cir[] = {
		{ 0x00, 2, Sfp004_Response_ReadWord, Sfp004_Void_Write },
		{ 0x02, 2, Sfp004_Void_Read, Sfp004_Control_WriteWord },
		{ 0x04, 2, Sfp004_Save_ReadWord, Sfp004_Void_Write },
		{ 0x06, 2, Sfp004_Restore_ReadWord, Sfp004_Restore_WriteWord },
		{ 0x08, 2, Sfp004_Void_Read, Sfp004_OpWord_WriteWord },
		{ 0x0a, 2, Sfp004_Void_Read, Sfp004_Command_WriteWord },
		{ 0x0c, 2, Sfp004_Void_Read, Sfp004_Void_Write },
		{ 0x0e, 2, Sfp004_Void_Read, Sfp004_Condition_WriteWord },
		{ 0x10, 4, Sfp004_Operand_Read, Sfp004_Operand_Write },
		{ 0x14, 2, Sfp004_RegSelect_ReadWord, Sfp004_Void_Write },
		{ 0x16, 2, Sfp004_Void_Read, Sfp004_Void_Write },
		{ 0x18, 4, Sfp004_Void_Read, Sfp004_InstAddr_Write },
		{ 0x1c, 4, Sfp004_OpAddr_Read, Sfp004_OpAddr_Write },
	};
	unsigned int i;
	int b;

	if (!Sfp004_IsEnabled())
		return;

	for (i = 0; i < ARRAY_SIZE(cir); i++)
	{
		for (b = 0; b < cir[i].size; b++)
		{
			IoMem_Intercept(SFP004_BASE + cir[i].offset + b,
			                cir[i].read, cir[i].write);
		}
	}
}

/**
 * Reset the FPU (the system reset line resets it too)
 */
void Sfp004_Reset(void)
{
	set_special_flags(addsub_swap_inf, &fs);
	Sfp004_ResetFpu(Sfp004_Now());
	sfp.restore = 0;
	sfp.reglist = 0;
	sfp.opword = sfp.opaddr = 0;
}

/**
 * Save/Restore snapshot of local variables ('MemorySnapShot_Store' handles type)
 */
void Sfp004_MemorySnapShot_Capture(bool bSave)
{
	MemorySnapShot_Store(&sfp, sizeof(sfp));
	if (!bSave)
	{
		set_special_flags(addsub_swap_inf, &fs);
		Sfp004_SetMode();
	}
}
