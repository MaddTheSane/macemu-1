/*
 *  ppc-instructions.hpp - PowerPC instructions IDs
 *
 *  Kheperix (C) 2003 Gwenole Beauchesne
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef PPC_INSTRUCTIONS_H
#define PPC_INSTRUCTIONS_H

/**
 *		Define PowerPC instruction types
 **/

#define PPC_I(X) powerpc_instruction_##X

enum powerpc_instruction {
	PPC_I(INVALID),
	PPC_I(ADD),
	PPC_I(ADDC),
	PPC_I(ADDE),
	PPC_I(ADDI),
	PPC_I(ADDIC),
	PPC_I(ADDIC_),
	PPC_I(ADDIS),
	PPC_I(ADDME),
	PPC_I(ADDZE),
	PPC_I(AND),
	PPC_I(ANDC),
	PPC_I(ANDI),
	PPC_I(ANDIS),
	PPC_I(B),
	PPC_I(BC),
	PPC_I(BCCTR),
	PPC_I(BCLR),
	PPC_I(CMP),
	PPC_I(CMPI),
	PPC_I(CMPL),
	PPC_I(CMPLI),
	PPC_I(CNTLZW),
	PPC_I(CRAND),
	PPC_I(CRANDC),
	PPC_I(CREQV),
	PPC_I(CRNAND),
	PPC_I(CRNOR),
	PPC_I(CROR),
	PPC_I(CRORC),
	PPC_I(CRXOR),
	PPC_I(DCBA),
	PPC_I(DCBF),
	PPC_I(DCBI),
	PPC_I(DCBST),
	PPC_I(DCBT),
	PPC_I(DCBTST),
	PPC_I(DCBZ),
	PPC_I(DIVW),
	PPC_I(DIVWU),
	PPC_I(ECIWX),
	PPC_I(ECOWX),
	PPC_I(EIEIO),
	PPC_I(EQV),
	PPC_I(EXTSB),
	PPC_I(EXTSH),
	PPC_I(FABS),
	PPC_I(FADD),
	PPC_I(FADDS),
	PPC_I(FCMPO),
	PPC_I(FCMPU),
	PPC_I(FCTIW),
	PPC_I(FCTIWZ),
	PPC_I(FDIV),
	PPC_I(FDIVS),
	PPC_I(FMADD),
	PPC_I(FMADDS),
	PPC_I(FMR),
	PPC_I(FMSUB),
	PPC_I(FMSUBS),
	PPC_I(FMUL),
	PPC_I(FMULS),
	PPC_I(FNABS),
	PPC_I(FNEG),
	PPC_I(FNMADD),
	PPC_I(FNMADDS),
	PPC_I(FNMSUB),
	PPC_I(FNMSUBS),
	PPC_I(FRSP),
	PPC_I(FSUB),
	PPC_I(FSUBS),
	PPC_I(ICBI),
	PPC_I(ISYNC),
	PPC_I(LBZ),
	PPC_I(LBZU),
	PPC_I(LBZUX),
	PPC_I(LBZX),
	PPC_I(LFD),
	PPC_I(LFDU),
	PPC_I(LFDUX),
	PPC_I(LFDX),
	PPC_I(LFS),
	PPC_I(LFSU),
	PPC_I(LFSUX),
	PPC_I(LFSX),
	PPC_I(LHA),
	PPC_I(LHAU),
	PPC_I(LHAUX),
	PPC_I(LHAX),
	PPC_I(LHBRX),
	PPC_I(LHZ),
	PPC_I(LHZU),
	PPC_I(LHZUX),
	PPC_I(LHZX),
	PPC_I(LMW),
	PPC_I(LSWI),
	PPC_I(LSWX),
	PPC_I(LWARX),
	PPC_I(LWBRX),
	PPC_I(LWZ),
	PPC_I(LWZU),
	PPC_I(LWZUX),
	PPC_I(LWZX),
	PPC_I(MCRF),
	PPC_I(MCRFS),
	PPC_I(MFCR),
	PPC_I(MFFS),
	PPC_I(MFMSR),
	PPC_I(MFSPR),
	PPC_I(MFTB),
	PPC_I(MTCRF),
	PPC_I(MTFSB0),
	PPC_I(MTFSB1),
	PPC_I(MTFSF),
	PPC_I(MTFSFI),
	PPC_I(MTSPR),
	PPC_I(MULHW),
	PPC_I(MULHWU),
	PPC_I(MULLI),
	PPC_I(MULLW),
	PPC_I(NAND),
	PPC_I(NEG),
	PPC_I(NOR),
	PPC_I(OR),
	PPC_I(ORC),
	PPC_I(ORI),
	PPC_I(ORIS),
	PPC_I(RLWIMI),
	PPC_I(RLWINM),
	PPC_I(RLWNM),
	PPC_I(SC),
	PPC_I(SLW),
	PPC_I(SRAW),
	PPC_I(SRAWI),
	PPC_I(SRW),
	PPC_I(STB),
	PPC_I(STBU),
	PPC_I(STBUX),
	PPC_I(STBX),
	PPC_I(STFD),
	PPC_I(STFDU),
	PPC_I(STFDUX),
	PPC_I(STFDX),
	PPC_I(STFS),
	PPC_I(STFSU),
	PPC_I(STFSUX),
	PPC_I(STFSX),
	PPC_I(STH),
	PPC_I(STHBRX),
	PPC_I(STHU),
	PPC_I(STHUX),
	PPC_I(STHX),
	PPC_I(STMW),
	PPC_I(STSWI),
	PPC_I(STSWX),
	PPC_I(STW),
	PPC_I(STWBRX),
	PPC_I(STWCX),
	PPC_I(STWU),
	PPC_I(STWUX),
	PPC_I(STWX),
	PPC_I(SUBF),
	PPC_I(SUBFC),
	PPC_I(SUBFE),
	PPC_I(SUBFIC),
	PPC_I(SUBFME),
	PPC_I(SUBFZE),
	PPC_I(SYNC),
	PPC_I(XOR),
	PPC_I(XORI),
	PPC_I(XORIS),
	PPC_I(MAX) // Total number of instruction types
};

#endif /* PPC_INSTRUCTIONS_H */