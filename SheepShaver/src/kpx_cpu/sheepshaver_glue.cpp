/*
 *  sheepshaver_glue.cpp - Glue Kheperix CPU to SheepShaver CPU engine interface
 *
 *  SheepShaver (C) 1997-2004 Christian Bauer and Marc Hellwig
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

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "xlowmem.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "macos_util.h"
#include "block-alloc.hpp"
#include "sigsegv.h"
#include "cpu/ppc/ppc-cpu.hpp"
#include "cpu/ppc/ppc-operations.hpp"
#include "cpu/ppc/ppc-instructions.hpp"
#include "thunks.h"

// Used for NativeOp trampolines
#include "video.h"
#include "name_registry.h"
#include "serial.h"
#include "ether.h"

#include <stdio.h>

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

// Emulation time statistics
#define EMUL_TIME_STATS 1

#if EMUL_TIME_STATS
static clock_t emul_start_time;
static uint32 interrupt_count = 0;
static clock_t interrupt_time = 0;
static uint32 exec68k_count = 0;
static clock_t exec68k_time = 0;
static uint32 native_exec_count = 0;
static clock_t native_exec_time = 0;
static uint32 macos_exec_count = 0;
static clock_t macos_exec_time = 0;
#endif

static void enter_mon(void)
{
	// Start up mon in real-mode
#if ENABLE_MON
	char *arg[4] = {"mon", "-m", "-r", NULL};
	mon(3, arg);
#endif
}

// From main_*.cpp
extern uintptr SignalStackBase();

// From rsrc_patches.cpp
extern "C" void check_load_invoc(uint32 type, int16 id, uint32 h);

// PowerPC EmulOp to exit from emulation looop
const uint32 POWERPC_EXEC_RETURN = POWERPC_EMUL_OP | 1;

// Enable multicore (main/interrupts) cpu emulation?
#define MULTICORE_CPU (ASYNC_IRQ ? 1 : 0)

// Enable Execute68k() safety checks?
#define SAFE_EXEC_68K 1

// Save FP state in Execute68k()?
#define SAVE_FP_EXEC_68K 1

// Interrupts in EMUL_OP mode?
#define INTERRUPTS_IN_EMUL_OP_MODE 1

// Interrupts in native mode?
#define INTERRUPTS_IN_NATIVE_MODE 1

// Pointer to Kernel Data
static KernelData * const kernel_data = (KernelData *)KERNEL_DATA_BASE;

// SIGSEGV handler
static sigsegv_return_t sigsegv_handler(sigsegv_address_t, sigsegv_address_t);

// JIT Compiler enabled?
static inline bool enable_jit_p()
{
	return PrefsFindBool("jit");
}


/**
 *		PowerPC emulator glue with special 'sheep' opcodes
 **/

enum {
	PPC_I(SHEEP) = PPC_I(MAX),
	PPC_I(SHEEP_MAX)
};

class sheepshaver_cpu
	: public powerpc_cpu
{
	void init_decoder();
	void execute_sheep(uint32 opcode);

public:

	// Constructor
	sheepshaver_cpu();

	// CR & XER accessors
	uint32 get_cr() const		{ return cr().get(); }
	void set_cr(uint32 v)		{ cr().set(v); }
	uint32 get_xer() const		{ return xer().get(); }
	void set_xer(uint32 v)		{ xer().set(v); }

	// Execute EMUL_OP routine
	void execute_emul_op(uint32 emul_op);

	// Execute 68k routine
	void execute_68k(uint32 entry, M68kRegisters *r);

	// Execute ppc routine
	void execute_ppc(uint32 entry);

	// Execute MacOS/PPC code
	uint32 execute_macos_code(uint32 tvect, int nargs, uint32 const *args);

	// Compile one instruction
	virtual bool compile1(codegen_context_t & cg_context);

	// Resource manager thunk
	void get_resource(uint32 old_get_resource);

	// Handle MacOS interrupt
	void interrupt(uint32 entry);
	void handle_interrupt();

	// Lazy memory allocator (one item at a time)
	void *operator new(size_t size)
		{ return allocator_helper< sheepshaver_cpu, lazy_allocator >::allocate(); }
	void operator delete(void *p)
		{ allocator_helper< sheepshaver_cpu, lazy_allocator >::deallocate(p); }
	// FIXME: really make surre array allocation fail at link time?
	void *operator new[](size_t);
	void operator delete[](void *p);

	// Make sure the SIGSEGV handler can access CPU registers
	friend sigsegv_return_t sigsegv_handler(sigsegv_address_t, sigsegv_address_t);
};

lazy_allocator< sheepshaver_cpu > allocator_helper< sheepshaver_cpu, lazy_allocator >::allocator;

sheepshaver_cpu::sheepshaver_cpu()
	: powerpc_cpu(enable_jit_p())
{
	init_decoder();
}

void sheepshaver_cpu::init_decoder()
{
#ifndef PPC_NO_STATIC_II_INDEX_TABLE
	static bool initialized = false;
	if (initialized)
		return;
	initialized = true;
#endif

	static const instr_info_t sheep_ii_table[] = {
		{ "sheep",
		  (execute_pmf)&sheepshaver_cpu::execute_sheep,
		  NULL,
		  PPC_I(SHEEP),
		  D_form, 6, 0, CFLOW_JUMP | CFLOW_TRAP
		}
	};

	const int ii_count = sizeof(sheep_ii_table)/sizeof(sheep_ii_table[0]);
	D(bug("SheepShaver extra decode table has %d entries\n", ii_count));

	for (int i = 0; i < ii_count; i++) {
		const instr_info_t * ii = &sheep_ii_table[i];
		init_decoder_entry(ii);
	}
}

// Forward declaration for native opcode handler
static void NativeOp(int selector);

/*		NativeOp instruction format:
		+------------+--------------------------+--+----------+------------+
		|      6     |                          |FN|    OP    |      2     |
		+------------+--------------------------+--+----------+------------+
		 0         5 |6                       19 20 21      25 26        31
*/

typedef bit_field< 20, 20 > FN_field;
typedef bit_field< 21, 25 > NATIVE_OP_field;
typedef bit_field< 26, 31 > EMUL_OP_field;

// Execute EMUL_OP routine
void sheepshaver_cpu::execute_emul_op(uint32 emul_op)
{
	M68kRegisters r68;
	WriteMacInt32(XLM_68K_R25, gpr(25));
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);
	for (int i = 0; i < 8; i++)
		r68.d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)
		r68.a[i] = gpr(16 + i);
	r68.a[7] = gpr(1);
	uint32 saved_cr = get_cr() & CR_field<2>::mask();
	uint32 saved_xer = get_xer();
	EmulOp(&r68, gpr(24), emul_op);
	set_cr(saved_cr);
	set_xer(saved_xer);
	for (int i = 0; i < 8; i++)
		gpr(8 + i) = r68.d[i];
	for (int i = 0; i < 7; i++)
		gpr(16 + i) = r68.a[i];
	gpr(1) = r68.a[7];
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);
}

// Execute SheepShaver instruction
void sheepshaver_cpu::execute_sheep(uint32 opcode)
{
//	D(bug("Extended opcode %08x at %08x (68k pc %08x)\n", opcode, pc(), gpr(24)));
	assert((((opcode >> 26) & 0x3f) == 6) && OP_MAX <= 64 + 3);

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		QuitEmulator();
		break;

	case 1:		// EXEC_RETURN
		spcflags().set(SPCFLAG_CPU_EXEC_RETURN);
		break;

	case 2:		// EXEC_NATIVE
		NativeOp(NATIVE_OP_field::extract(opcode));
		if (FN_field::test(opcode))
			pc() = lr();
		else
			pc() += 4;
		break;

	default:	// EMUL_OP
		execute_emul_op(EMUL_OP_field::extract(opcode) - 3);
		pc() += 4;
		break;
	}
}

// Compile one instruction
bool sheepshaver_cpu::compile1(codegen_context_t & cg_context)
{
#if PPC_ENABLE_JIT
	const instr_info_t *ii = cg_context.instr_info;
	if (ii->mnemo != PPC_I(SHEEP))
		return false;

	bool compiled = false;
	powerpc_dyngen & dg = cg_context.codegen;
	uint32 opcode = cg_context.opcode;

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		dg.gen_invoke(QuitEmulator);
		compiled = true;
		break;

	case 1:		// EXEC_RETURN
		dg.gen_spcflags_set(SPCFLAG_CPU_EXEC_RETURN);
		compiled = true;
		break;

	case 2: {	// EXEC_NATIVE
		uint32 selector = NATIVE_OP_field::extract(opcode);
		switch (selector) {
		case NATIVE_PATCH_NAME_REGISTRY:
			dg.gen_invoke(DoPatchNameRegistry);
			compiled = true;
			break;
		case NATIVE_VIDEO_INSTALL_ACCEL:
			dg.gen_invoke(VideoInstallAccel);
			compiled = true;
			break;
		case NATIVE_VIDEO_VBL:
			dg.gen_invoke(VideoVBL);
			compiled = true;
			break;
		case NATIVE_GET_RESOURCE:
		case NATIVE_GET_1_RESOURCE:
		case NATIVE_GET_IND_RESOURCE:
		case NATIVE_GET_1_IND_RESOURCE:
		case NATIVE_R_GET_RESOURCE: {
			static const uint32 get_resource_ptr[] = {
				XLM_GET_RESOURCE,
				XLM_GET_1_RESOURCE,
				XLM_GET_IND_RESOURCE,
				XLM_GET_1_IND_RESOURCE,
				XLM_R_GET_RESOURCE
			};
			uint32 old_get_resource = ReadMacInt32(get_resource_ptr[selector - NATIVE_GET_RESOURCE]);
			typedef void (*func_t)(dyngen_cpu_base, uint32);
			func_t func = (func_t)nv_mem_fun(&sheepshaver_cpu::get_resource).ptr();
			dg.gen_invoke_CPU_im(func, old_get_resource);
			compiled = true;
			break;
		}
		case NATIVE_DISABLE_INTERRUPT:
			dg.gen_invoke(DisableInterrupt);
			compiled = true;
			break;
		case NATIVE_ENABLE_INTERRUPT:
			dg.gen_invoke(EnableInterrupt);
			compiled = true;
			break;
		case NATIVE_CHECK_LOAD_INVOC:
			dg.gen_load_T0_GPR(3);
			dg.gen_load_T1_GPR(4);
			dg.gen_se_16_32_T1();
			dg.gen_load_T2_GPR(5);
			dg.gen_invoke_T0_T1_T2((void (*)(uint32, uint32, uint32))check_load_invoc);
			compiled = true;
			break;
		}
		if (FN_field::test(opcode)) {
			if (compiled) {
				dg.gen_load_A0_LR();
				dg.gen_set_PC_A0();
			}
			cg_context.done_compile = true;
		}
		else
			cg_context.done_compile = false;
		break;
	}

	default: {	// EMUL_OP
		typedef void (*func_t)(dyngen_cpu_base, uint32);
		func_t func = (func_t)nv_mem_fun(&sheepshaver_cpu::execute_emul_op).ptr();
		dg.gen_invoke_CPU_im(func, EMUL_OP_field::extract(opcode) - 3);
		cg_context.done_compile = false;
		compiled = true;
		break;
	}
	}
	return compiled;
#endif
	return false;
}

// Handle MacOS interrupt
void sheepshaver_cpu::interrupt(uint32 entry)
{
#if EMUL_TIME_STATS
	interrupt_count++;
	const clock_t interrupt_start = clock();
#endif

#if !MULTICORE_CPU
	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_sp = gpr(1);
#endif

	// Initialize stack pointer to SheepShaver alternate stack base
	gpr(1) = SignalStackBase() - 64;

	// Build trampoline to return from interrupt
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;

	// Prepare registers for nanokernel interrupt routine
	kernel_data->v[0x004 >> 2] = htonl(gpr(1));
	kernel_data->v[0x018 >> 2] = htonl(gpr(6));

	gpr(6) = ntohl(kernel_data->v[0x65c >> 2]);
	assert(gpr(6) != 0);
	WriteMacInt32(gpr(6) + 0x13c, gpr(7));
	WriteMacInt32(gpr(6) + 0x144, gpr(8));
	WriteMacInt32(gpr(6) + 0x14c, gpr(9));
	WriteMacInt32(gpr(6) + 0x154, gpr(10));
	WriteMacInt32(gpr(6) + 0x15c, gpr(11));
	WriteMacInt32(gpr(6) + 0x164, gpr(12));
	WriteMacInt32(gpr(6) + 0x16c, gpr(13));

	gpr(1)  = KernelDataAddr;
	gpr(7)  = ntohl(kernel_data->v[0x660 >> 2]);
	gpr(8)  = 0;
	gpr(10) = trampoline.addr();
	gpr(12) = trampoline.addr();
	gpr(13) = get_cr();

	// rlwimi. r7,r7,8,0,0
	uint32 result = op_ppc_rlwimi::apply(gpr(7), 8, 0x80000000, gpr(7));
	record_cr0(result);
	gpr(7) = result;

	gpr(11) = 0xf072; // MSR (SRR1)
	cr().set((gpr(11) & 0x0fff0000) | (get_cr() & ~0x0fff0000));

	// Enter nanokernel
	execute(entry);

#if !MULTICORE_CPU
	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	gpr(1) = saved_sp;
#endif

#if EMUL_TIME_STATS
	interrupt_time += (clock() - interrupt_start);
#endif
}

// Execute 68k routine
void sheepshaver_cpu::execute_68k(uint32 entry, M68kRegisters *r)
{
#if EMUL_TIME_STATS
	exec68k_count++;
	const clock_t exec68k_start = clock();
#endif

#if SAFE_EXEC_68K
	if (ReadMacInt32(XLM_RUN_MODE) != MODE_EMUL_OP)
		printf("FATAL: Execute68k() not called from EMUL_OP mode\n");
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_cr = get_cr();

	// Create MacOS stack frame
	// FIXME: make sure MacOS doesn't expect PPC registers to live on top
	uint32 sp = gpr(1);
	gpr(1) -= 56;
	WriteMacInt32(gpr(1), sp);

	// Save PowerPC registers
	uint32 saved_GPRs[19];
	memcpy(&saved_GPRs[0], &gpr(13), sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	double saved_FPRs[18];
	memcpy(&saved_FPRs[0], &fpr(14), sizeof(double)*(32-14));
#endif

	// Setup registers for 68k emulator
	cr().set(CR_SO_field<2>::mask());			// Supervisor mode
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  gpr(8 + i) = r->d[i];
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  gpr(16 + i) = r->a[i];
	gpr(23) = 0;
	gpr(24) = entry;
	gpr(25) = ReadMacInt32(XLM_68K_R25);		// MSB of SR
	gpr(26) = 0;
	gpr(28) = 0;								// VBR
	gpr(29) = ntohl(kernel_data->ed.v[0x74 >> 2]);		// Pointer to opcode table
	gpr(30) = ntohl(kernel_data->ed.v[0x78 >> 2]);		// Address of emulator
	gpr(31) = KernelDataAddr + 0x1000;

	// Push return address (points to EXEC_RETURN opcode) on stack
	gpr(1) -= 4;
	WriteMacInt32(gpr(1), XLM_EXEC_RETURN_OPCODE);
	
	// Rentering 68k emulator
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

	// Set r0 to 0 for 68k emulator
	gpr(0) = 0;

	// Execute 68k opcode
	uint32 opcode = ReadMacInt16(gpr(24));
	gpr(27) = (int32)(int16)ReadMacInt16(gpr(24) += 2);
	gpr(29) += opcode * 8;
	execute(gpr(29));

	// Save r25 (contains current 68k interrupt level)
	WriteMacInt32(XLM_68K_R25, gpr(25));

	// Reentering EMUL_OP mode
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);

	// Save 68k registers
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  r->d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  r->a[i] = gpr(16 + i);

	// Restore PowerPC registers
	memcpy(&gpr(13), &saved_GPRs[0], sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	memcpy(&fpr(14), &saved_FPRs[0], sizeof(double)*(32-14));
#endif

	// Cleanup stack
	gpr(1) += 56;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	set_cr(saved_cr);

#if EMUL_TIME_STATS
	exec68k_time += (clock() - exec68k_start);
#endif
}

// Call MacOS PPC code
uint32 sheepshaver_cpu::execute_macos_code(uint32 tvect, int nargs, uint32 const *args)
{
#if EMUL_TIME_STATS
	macos_exec_count++;
	const clock_t macos_exec_start = clock();
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();

	// Build trampoline with EXEC_RETURN
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	lr() = trampoline.addr();

	gpr(1) -= 64;								// Create stack frame
	uint32 proc = ReadMacInt32(tvect);			// Get routine address
	uint32 toc = ReadMacInt32(tvect + 4);		// Get TOC pointer

	// Save PowerPC registers
	uint32 regs[8];
	regs[0] = gpr(2);
	for (int i = 0; i < nargs; i++)
		regs[i + 1] = gpr(i + 3);

	// Prepare and call MacOS routine
	gpr(2) = toc;
	for (int i = 0; i < nargs; i++)
		gpr(i + 3) = args[i];
	execute(proc);
	uint32 retval = gpr(3);

	// Restore PowerPC registers
	for (int i = 0; i <= nargs; i++)
		gpr(i + 2) = regs[i];

	// Cleanup stack
	gpr(1) += 64;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;

#if EMUL_TIME_STATS
	macos_exec_time += (clock() - macos_exec_start);
#endif

	return retval;
}

// Execute ppc routine
inline void sheepshaver_cpu::execute_ppc(uint32 entry)
{
	// Save branch registers
	uint32 saved_lr = lr();

	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	WriteMacInt32(trampoline.addr(), POWERPC_EXEC_RETURN);
	lr() = trampoline.addr();

	execute(entry);

	// Restore branch registers
	lr() = saved_lr;
}

// Resource Manager thunk
inline void sheepshaver_cpu::get_resource(uint32 old_get_resource)
{
	uint32 type = gpr(3);
	int16 id = gpr(4);

	// Create stack frame
	gpr(1) -= 56;

	// Call old routine
	execute_ppc(old_get_resource);

	// Call CheckLoad()
	uint32 handle = gpr(3);
	check_load_invoc(type, id, handle);
	gpr(3) = handle;

	// Cleanup stack
	gpr(1) += 56;
}


/**
 *		SheepShaver CPU engine interface
 **/

static sheepshaver_cpu *main_cpu = NULL;		// CPU emulator to handle usual control flow
static sheepshaver_cpu *interrupt_cpu = NULL;	// CPU emulator to handle interrupts
static sheepshaver_cpu *current_cpu = NULL;		// Current CPU emulator context

void FlushCodeCache(uintptr start, uintptr end)
{
	D(bug("FlushCodeCache(%08x, %08x)\n", start, end));
	main_cpu->invalidate_cache_range(start, end);
#if MULTICORE_CPU
	interrupt_cpu->invalidate_cache_range(start, end);
#endif
}

static inline void cpu_push(sheepshaver_cpu *new_cpu)
{
#if MULTICORE_CPU
	current_cpu = new_cpu;
#endif
}

static inline void cpu_pop()
{
#if MULTICORE_CPU
	current_cpu = main_cpu;
#endif
}

// Dump PPC registers
static void dump_registers(void)
{
	current_cpu->dump_registers();
}

// Dump log
static void dump_log(void)
{
	current_cpu->dump_log();
}

/*
 *  Initialize CPU emulation
 */

static sigsegv_return_t sigsegv_handler(sigsegv_address_t fault_address, sigsegv_address_t fault_instruction)
{
#if ENABLE_VOSF
	// Handle screen fault
	extern bool Screen_fault_handler(sigsegv_address_t, sigsegv_address_t);
	if (Screen_fault_handler(fault_address, fault_instruction))
		return SIGSEGV_RETURN_SUCCESS;
#endif

	const uintptr addr = (uintptr)fault_address;
#if HAVE_SIGSEGV_SKIP_INSTRUCTION
	// Ignore writes to ROM
	if ((addr - ROM_BASE) < ROM_SIZE)
		return SIGSEGV_RETURN_SKIP_INSTRUCTION;

	// Get program counter of target CPU
	sheepshaver_cpu * const cpu = current_cpu;
	const uint32 pc = cpu->pc();
	
	// Fault in Mac ROM or RAM?
	bool mac_fault = (pc >= ROM_BASE) && (pc < (ROM_BASE + ROM_AREA_SIZE)) || (pc >= RAMBase) && (pc < (RAMBase + RAMSize));
	if (mac_fault) {

		// "VM settings" during MacOS 8 installation
		if (pc == ROM_BASE + 0x488160 && cpu->gpr(20) == 0xf8000000)
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
	
		// MacOS 8.5 installation
		else if (pc == ROM_BASE + 0x488140 && cpu->gpr(16) == 0xf8000000)
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
	
		// MacOS 8 serial drivers on startup
		else if (pc == ROM_BASE + 0x48e080 && (cpu->gpr(8) == 0xf3012002 || cpu->gpr(8) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
	
		// MacOS 8.1 serial drivers on startup
		else if (pc == ROM_BASE + 0x48c5e0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		else if (pc == ROM_BASE + 0x4a10a0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// Ignore all other faults, if requested
		if (PrefsFindBool("ignoresegv"))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
	}
#else
#error "FIXME: You don't have the capability to skip instruction within signal handlers"
#endif

	printf("SIGSEGV\n");
	printf("  pc %p\n", fault_instruction);
	printf("  ea %p\n", fault_address);
	printf(" cpu %s\n", current_cpu == main_cpu ? "main" : "interrupts");
	dump_registers();
	current_cpu->dump_log();
	enter_mon();
	QuitEmulator();

	return SIGSEGV_RETURN_FAILURE;
}

void init_emul_ppc(void)
{
	// Initialize main CPU emulator
	main_cpu = new sheepshaver_cpu();
	main_cpu->set_register(powerpc_registers::GPR(3), any_register((uint32)ROM_BASE + 0x30d000));
	main_cpu->set_register(powerpc_registers::GPR(4), any_register(KernelDataAddr + 0x1000));
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

#if MULTICORE_CPU
	// Initialize alternate CPU emulator to handle interrupts
	interrupt_cpu = new sheepshaver_cpu();
#endif

	// Install the handler for SIGSEGV
	sigsegv_install_handler(sigsegv_handler);

#if ENABLE_MON
	// Install "regs" command in cxmon
	mon_add_command("regs", dump_registers, "regs                     Dump PowerPC registers\n");
	mon_add_command("log", dump_log, "log                      Dump PowerPC emulation log\n");
#endif

#if EMUL_TIME_STATS
	emul_start_time = clock();
#endif
}

/*
 *  Deinitialize emulation
 */

void exit_emul_ppc(void)
{
#if EMUL_TIME_STATS
	clock_t emul_end_time = clock();

	printf("### Statistics for SheepShaver emulation parts\n");
	const clock_t emul_time = emul_end_time - emul_start_time;
	printf("Total emulation time : %.1f sec\n", double(emul_time) / double(CLOCKS_PER_SEC));
	printf("Total interrupt count: %d (%2.1f Hz)\n", interrupt_count,
		   (double(interrupt_count) * CLOCKS_PER_SEC) / double(emul_time));

#define PRINT_STATS(LABEL, VAR_PREFIX) do {								\
		printf("Total " LABEL " count : %d\n", VAR_PREFIX##_count);		\
		printf("Total " LABEL " time  : %.1f sec (%.1f%%)\n",			\
			   double(VAR_PREFIX##_time) / double(CLOCKS_PER_SEC),		\
			   100.0 * double(VAR_PREFIX##_time) / double(emul_time));	\
	} while (0)

	PRINT_STATS("Execute68k[Trap] execution", exec68k);
	PRINT_STATS("NativeOp execution", native_exec);
	PRINT_STATS("MacOS routine execution", macos_exec);

#undef PRINT_STATS
	printf("\n");
#endif

	delete main_cpu;
#if MULTICORE_CPU
	delete interrupt_cpu;
#endif
}

/*
 *  Emulation loop
 */

void emul_ppc(uint32 entry)
{
	current_cpu = main_cpu;
#if 0
	current_cpu->start_log();
#endif
	// start emulation loop and enable code translation or caching
	current_cpu->execute(entry);
}

/*
 *  Handle PowerPC interrupt
 */

#if ASYNC_IRQ
void HandleInterrupt(void)
{
	main_cpu->handle_interrupt();
}
#else
void TriggerInterrupt(void)
{
#if 0
  WriteMacInt32(0x16a, ReadMacInt32(0x16a) + 1);
#else
  // Trigger interrupt to main cpu only
  if (main_cpu)
	  main_cpu->trigger_interrupt();
#endif
}
#endif

void sheepshaver_cpu::handle_interrupt(void)
{
	// Do nothing if interrupts are disabled
	if (*(int32 *)XLM_IRQ_NEST > 0)
		return;

	// Do nothing if there is no interrupt pending
	if (InterruptFlags == 0)
		return;

	// Disable MacOS stack sniffer
	WriteMacInt32(0x110, 0);

	// Interrupt action depends on current run mode
	switch (ReadMacInt32(XLM_RUN_MODE)) {
	case MODE_68K:
		// 68k emulator active, trigger 68k interrupt level 1
		assert(current_cpu == main_cpu);
		WriteMacInt16(tswap32(kernel_data->v[0x67c >> 2]), 1);
		set_cr(get_cr() | tswap32(kernel_data->v[0x674 >> 2]));
		break;
    
#if INTERRUPTS_IN_NATIVE_MODE
	case MODE_NATIVE:
		// 68k emulator inactive, in nanokernel?
		assert(current_cpu == main_cpu);
		if (gpr(1) != KernelDataAddr) {
			// Prepare for 68k interrupt level 1
			WriteMacInt16(tswap32(kernel_data->v[0x67c >> 2]), 1);
			WriteMacInt32(tswap32(kernel_data->v[0x658 >> 2]) + 0xdc,
						  ReadMacInt32(tswap32(kernel_data->v[0x658 >> 2]) + 0xdc)
						  | tswap32(kernel_data->v[0x674 >> 2]));
      
			// Execute nanokernel interrupt routine (this will activate the 68k emulator)
			DisableInterrupt();
			cpu_push(interrupt_cpu);
			if (ROMType == ROMTYPE_NEWWORLD)
				current_cpu->interrupt(ROM_BASE + 0x312b1c);
			else
				current_cpu->interrupt(ROM_BASE + 0x312a3c);
			cpu_pop();
		}
		break;
#endif
    
#if INTERRUPTS_IN_EMUL_OP_MODE
	case MODE_EMUL_OP:
		// 68k emulator active, within EMUL_OP routine, execute 68k interrupt routine directly when interrupt level is 0
		if ((ReadMacInt32(XLM_68K_R25) & 7) == 0) {
#if 1
			// Execute full 68k interrupt routine
			M68kRegisters r;
			uint32 old_r25 = ReadMacInt32(XLM_68K_R25);	// Save interrupt level
			WriteMacInt32(XLM_68K_R25, 0x21);			// Execute with interrupt level 1
			static const uint8 proc[] = {
				0x3f, 0x3c, 0x00, 0x00,			// move.w	#$0000,-(sp)	(fake format word)
				0x48, 0x7a, 0x00, 0x0a,			// pea		@1(pc)			(return address)
				0x40, 0xe7,						// move		sr,-(sp)		(saved SR)
				0x20, 0x78, 0x00, 0x064,		// move.l	$64,a0
				0x4e, 0xd0,						// jmp		(a0)
				M68K_RTS >> 8, M68K_RTS & 0xff	// @1
			};
			Execute68k((uint32)proc, &r);
			WriteMacInt32(XLM_68K_R25, old_r25);		// Restore interrupt level
#else
			// Only update cursor
			if (HasMacStarted()) {
				if (InterruptFlags & INTFLAG_VIA) {
					ClearInterruptFlag(INTFLAG_VIA);
					ADBInterrupt();
					ExecuteNative(NATIVE_VIDEO_VBL);
				}
			}
#endif
		}
		break;
#endif
	}
}

static void get_resource(void);
static void get_1_resource(void);
static void get_ind_resource(void);
static void get_1_ind_resource(void);
static void r_get_resource(void);

#define GPR(REG) current_cpu->gpr(REG)

static void NativeOp(int selector)
{
#if EMUL_TIME_STATS
	native_exec_count++;
	const clock_t native_exec_start = clock();
#endif

	switch (selector) {
	case NATIVE_PATCH_NAME_REGISTRY:
		DoPatchNameRegistry();
		break;
	case NATIVE_VIDEO_INSTALL_ACCEL:
		VideoInstallAccel();
		break;
	case NATIVE_VIDEO_VBL:
		VideoVBL();
		break;
	case NATIVE_VIDEO_DO_DRIVER_IO:
		GPR(3) = (int32)(int16)VideoDoDriverIO((void *)GPR(3), (void *)GPR(4),
											   (void *)GPR(5), GPR(6), GPR(7));
		break;
#ifdef WORDS_BIGENDIAN
	case NATIVE_ETHER_IRQ:
		EtherIRQ();
		break;
	case NATIVE_ETHER_INIT:
		GPR(3) = InitStreamModule((void *)GPR(3));
		break;
	case NATIVE_ETHER_TERM:
		TerminateStreamModule();
		break;
	case NATIVE_ETHER_OPEN:
		GPR(3) = ether_open((queue_t *)GPR(3), (void *)GPR(4), GPR(5), GPR(6), (void*)GPR(7));
		break;
	case NATIVE_ETHER_CLOSE:
		GPR(3) = ether_close((queue_t *)GPR(3), GPR(4), (void *)GPR(5));
		break;
	case NATIVE_ETHER_WPUT:
		GPR(3) = ether_wput((queue_t *)GPR(3), (mblk_t *)GPR(4));
		break;
	case NATIVE_ETHER_RSRV:
		GPR(3) = ether_rsrv((queue_t *)GPR(3));
		break;
#else
	case NATIVE_ETHER_INIT:
		// FIXME: needs more complicated thunks
		GPR(3) = false;
		break;
#endif
	case NATIVE_SERIAL_NOTHING:
	case NATIVE_SERIAL_OPEN:
	case NATIVE_SERIAL_PRIME_IN:
	case NATIVE_SERIAL_PRIME_OUT:
	case NATIVE_SERIAL_CONTROL:
	case NATIVE_SERIAL_STATUS:
	case NATIVE_SERIAL_CLOSE: {
		typedef int16 (*SerialCallback)(uint32, uint32);
		static const SerialCallback serial_callbacks[] = {
			SerialNothing,
			SerialOpen,
			SerialPrimeIn,
			SerialPrimeOut,
			SerialControl,
			SerialStatus,
			SerialClose
		};
		GPR(3) = serial_callbacks[selector - NATIVE_SERIAL_NOTHING](GPR(3), GPR(4));
		break;
	}
	case NATIVE_GET_RESOURCE:
	case NATIVE_GET_1_RESOURCE:
	case NATIVE_GET_IND_RESOURCE:
	case NATIVE_GET_1_IND_RESOURCE:
	case NATIVE_R_GET_RESOURCE: {
		typedef void (*GetResourceCallback)(void);
		static const GetResourceCallback get_resource_callbacks[] = {
			get_resource,
			get_1_resource,
			get_ind_resource,
			get_1_ind_resource,
			r_get_resource
		};
		get_resource_callbacks[selector - NATIVE_GET_RESOURCE]();
		break;
	}
	case NATIVE_DISABLE_INTERRUPT:
		DisableInterrupt();
		break;
	case NATIVE_ENABLE_INTERRUPT:
		EnableInterrupt();
		break;
	case NATIVE_MAKE_EXECUTABLE:
		MakeExecutable(0, (void *)GPR(4), GPR(5));
		break;
	case NATIVE_CHECK_LOAD_INVOC:
		check_load_invoc(GPR(3), GPR(4), GPR(5));
		break;
	default:
		printf("FATAL: NATIVE_OP called with bogus selector %d\n", selector);
		QuitEmulator();
		break;
	}

#if EMUL_TIME_STATS
	native_exec_time += (clock() - native_exec_start);
#endif
}

/*
 *  Execute 68k subroutine (must be ended with EXEC_RETURN)
 *  This must only be called by the emul_thread when in EMUL_OP mode
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68k(uint32 pc, M68kRegisters *r)
{
	current_cpu->execute_68k(pc, r);
}

/*
 *  Execute 68k A-Trap from EMUL_OP routine
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68kTrap(uint16 trap, M68kRegisters *r)
{
	SheepVar proc_var(4);
	uint32 proc = proc_var.addr();
	WriteMacInt16(proc, trap);
	WriteMacInt16(proc + 2, M68K_RTS);
	Execute68k(proc, r);
}

/*
 *  Call MacOS PPC code
 */

uint32 call_macos(uint32 tvect)
{
	return current_cpu->execute_macos_code(tvect, 0, NULL);
}

uint32 call_macos1(uint32 tvect, uint32 arg1)
{
	const uint32 args[] = { arg1 };
	return current_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos2(uint32 tvect, uint32 arg1, uint32 arg2)
{
	const uint32 args[] = { arg1, arg2 };
	return current_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos3(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3)
{
	const uint32 args[] = { arg1, arg2, arg3 };
	return current_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos4(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4 };
	return current_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos5(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5 };
	return current_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos6(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6 };
	return current_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos7(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6, arg7 };
	return current_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

/*
 *  Resource Manager thunks
 */

void get_resource(void)
{
	current_cpu->get_resource(ReadMacInt32(XLM_GET_RESOURCE));
}

void get_1_resource(void)
{
	current_cpu->get_resource(ReadMacInt32(XLM_GET_1_RESOURCE));
}

void get_ind_resource(void)
{
	current_cpu->get_resource(ReadMacInt32(XLM_GET_IND_RESOURCE));
}

void get_1_ind_resource(void)
{
	current_cpu->get_resource(ReadMacInt32(XLM_GET_1_IND_RESOURCE));
}

void r_get_resource(void)
{
	current_cpu->get_resource(ReadMacInt32(XLM_R_GET_RESOURCE));
}