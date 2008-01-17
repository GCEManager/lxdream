/**
 * $Id$
 * 
 * SH4 => x86 translation. This version does no real optimization, it just
 * outputs straight-line x86 code - it mainly exists to provide a baseline
 * to test the optimizing versions against.
 *
 * Copyright (c) 2007 Nathan Keynes.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <assert.h>
#include <math.h>

#ifndef NDEBUG
#define DEBUG_JUMPS 1
#endif

#include "sh4/xltcache.h"
#include "sh4/sh4core.h"
#include "sh4/sh4trans.h"
#include "sh4/sh4mmio.h"
#include "sh4/x86op.h"
#include "clock.h"

#define DEFAULT_BACKPATCH_SIZE 4096

struct backpatch_record {
    uint32_t *fixup_addr;
    uint32_t fixup_icount;
    uint32_t exc_code;
};

#define MAX_RECOVERY_SIZE 2048

#define DELAY_NONE 0
#define DELAY_PC 1
#define DELAY_PC_PR 2

/** 
 * Struct to manage internal translation state. This state is not saved -
 * it is only valid between calls to sh4_translate_begin_block() and
 * sh4_translate_end_block()
 */
struct sh4_x86_state {
    int in_delay_slot;
    gboolean priv_checked; /* true if we've already checked the cpu mode. */
    gboolean fpuen_checked; /* true if we've already checked fpu enabled. */
    gboolean branch_taken; /* true if we branched unconditionally */
    uint32_t block_start_pc;
    uint32_t stack_posn;   /* Trace stack height for alignment purposes */
    int tstate;

    /* mode flags */
    gboolean tlb_on; /* True if tlb translation is active */

    /* Allocated memory for the (block-wide) back-patch list */
    struct backpatch_record *backpatch_list;
    uint32_t backpatch_posn;
    uint32_t backpatch_size;
    struct xlat_recovery_record recovery_list[MAX_RECOVERY_SIZE];
    uint32_t recovery_posn;
};

#define TSTATE_NONE -1
#define TSTATE_O    0
#define TSTATE_C    2
#define TSTATE_E    4
#define TSTATE_NE   5
#define TSTATE_G    0xF
#define TSTATE_GE   0xD
#define TSTATE_A    7
#define TSTATE_AE   3

/** Branch if T is set (either in the current cflags, or in sh4r.t) */
#define JT_rel8(rel8,label) if( sh4_x86.tstate == TSTATE_NONE ) { \
	CMP_imm8s_sh4r( 1, R_T ); sh4_x86.tstate = TSTATE_E; } \
    OP(0x70+sh4_x86.tstate); OP(rel8); \
    MARK_JMP(rel8,label)
/** Branch if T is clear (either in the current cflags or in sh4r.t) */
#define JF_rel8(rel8,label) if( sh4_x86.tstate == TSTATE_NONE ) { \
	CMP_imm8s_sh4r( 1, R_T ); sh4_x86.tstate = TSTATE_E; } \
    OP(0x70+ (sh4_x86.tstate^1)); OP(rel8); \
    MARK_JMP(rel8, label)

static struct sh4_x86_state sh4_x86;

static uint32_t max_int = 0x7FFFFFFF;
static uint32_t min_int = 0x80000000;
static uint32_t save_fcw; /* save value for fpu control word */
static uint32_t trunc_fcw = 0x0F7F; /* fcw value for truncation mode */

void sh4_x86_init()
{
    sh4_x86.backpatch_list = malloc(DEFAULT_BACKPATCH_SIZE);
    sh4_x86.backpatch_size = DEFAULT_BACKPATCH_SIZE / sizeof(struct backpatch_record);
}


static void sh4_x86_add_backpatch( uint8_t *fixup_addr, uint32_t fixup_pc, uint32_t exc_code )
{
    if( sh4_x86.backpatch_posn == sh4_x86.backpatch_size ) {
	sh4_x86.backpatch_size <<= 1;
	sh4_x86.backpatch_list = realloc( sh4_x86.backpatch_list, 
					  sh4_x86.backpatch_size * sizeof(struct backpatch_record));
	assert( sh4_x86.backpatch_list != NULL );
    }
    if( sh4_x86.in_delay_slot ) {
	fixup_pc -= 2;
    }
    sh4_x86.backpatch_list[sh4_x86.backpatch_posn].fixup_addr = (uint32_t *)fixup_addr;
    sh4_x86.backpatch_list[sh4_x86.backpatch_posn].fixup_icount = (fixup_pc - sh4_x86.block_start_pc)>>1;
    sh4_x86.backpatch_list[sh4_x86.backpatch_posn].exc_code = exc_code;
    sh4_x86.backpatch_posn++;
}

void sh4_x86_add_recovery( uint32_t pc )
{
    xlat_recovery[xlat_recovery_posn].xlat_pc = (uintptr_t)xlat_output;
    xlat_recovery[xlat_recovery_posn].sh4_icount = (pc - sh4_x86.block_start_pc)>>1;
    xlat_recovery_posn++;
}

/**
 * Emit an instruction to load an SH4 reg into a real register
 */
static inline void load_reg( int x86reg, int sh4reg ) 
{
    /* mov [bp+n], reg */
    OP(0x8B);
    OP(0x45 + (x86reg<<3));
    OP(REG_OFFSET(r[sh4reg]));
}

static inline void load_reg16s( int x86reg, int sh4reg )
{
    OP(0x0F);
    OP(0xBF);
    MODRM_r32_sh4r(x86reg, REG_OFFSET(r[sh4reg]));
}

static inline void load_reg16u( int x86reg, int sh4reg )
{
    OP(0x0F);
    OP(0xB7);
    MODRM_r32_sh4r(x86reg, REG_OFFSET(r[sh4reg]));

}

#define load_spreg( x86reg, regoff ) MOV_sh4r_r32( regoff, x86reg )
#define store_spreg( x86reg, regoff ) MOV_r32_sh4r( x86reg, regoff )
/**
 * Emit an instruction to load an immediate value into a register
 */
static inline void load_imm32( int x86reg, uint32_t value ) {
    /* mov #value, reg */
    OP(0xB8 + x86reg);
    OP32(value);
}

/**
 * Load an immediate 64-bit quantity (note: x86-64 only)
 */
static inline void load_imm64( int x86reg, uint32_t value ) {
    /* mov #value, reg */
    REXW();
    OP(0xB8 + x86reg);
    OP64(value);
}


/**
 * Emit an instruction to store an SH4 reg (RN)
 */
void static inline store_reg( int x86reg, int sh4reg ) {
    /* mov reg, [bp+n] */
    OP(0x89);
    OP(0x45 + (x86reg<<3));
    OP(REG_OFFSET(r[sh4reg]));
}

#define load_fr_bank(bankreg) load_spreg( bankreg, REG_OFFSET(fr_bank))

/**
 * Load an FR register (single-precision floating point) into an integer x86
 * register (eg for register-to-register moves)
 */
void static inline load_fr( int bankreg, int x86reg, int frm )
{
    OP(0x8B); OP(0x40+bankreg+(x86reg<<3)); OP((frm^1)<<2);
}

/**
 * Store an FR register (single-precision floating point) into an integer x86
 * register (eg for register-to-register moves)
 */
void static inline store_fr( int bankreg, int x86reg, int frn )
{
    OP(0x89);  OP(0x40+bankreg+(x86reg<<3)); OP((frn^1)<<2);
}


/**
 * Load a pointer to the back fp back into the specified x86 register. The
 * bankreg must have been previously loaded with FPSCR.
 * NB: 12 bytes
 */
static inline void load_xf_bank( int bankreg )
{
    NOT_r32( bankreg );
    SHR_imm8_r32( (21 - 6), bankreg ); // Extract bit 21 then *64 for bank size
    AND_imm8s_r32( 0x40, bankreg );    // Complete extraction
    OP(0x8D); OP(0x44+(bankreg<<3)); OP(0x28+bankreg); OP(REG_OFFSET(fr)); // LEA [ebp+bankreg+disp], bankreg
}

/**
 * Update the fr_bank pointer based on the current fpscr value.
 */
static inline void update_fr_bank( int fpscrreg )
{
    SHR_imm8_r32( (21 - 6), fpscrreg ); // Extract bit 21 then *64 for bank size
    AND_imm8s_r32( 0x40, fpscrreg );    // Complete extraction
    OP(0x8D); OP(0x44+(fpscrreg<<3)); OP(0x28+fpscrreg); OP(REG_OFFSET(fr)); // LEA [ebp+fpscrreg+disp], fpscrreg
    store_spreg( fpscrreg, REG_OFFSET(fr_bank) );
}
/**
 * Push FPUL (as a 32-bit float) onto the FPU stack
 */
static inline void push_fpul( )
{
    OP(0xD9); OP(0x45); OP(R_FPUL);
}

/**
 * Pop FPUL (as a 32-bit float) from the FPU stack
 */
static inline void pop_fpul( )
{
    OP(0xD9); OP(0x5D); OP(R_FPUL);
}

/**
 * Push a 32-bit float onto the FPU stack, with bankreg previously loaded
 * with the location of the current fp bank.
 */
static inline void push_fr( int bankreg, int frm ) 
{
    OP(0xD9); OP(0x40 + bankreg); OP((frm^1)<<2);  // FLD.S [bankreg + frm^1*4]
}

/**
 * Pop a 32-bit float from the FPU stack and store it back into the fp bank, 
 * with bankreg previously loaded with the location of the current fp bank.
 */
static inline void pop_fr( int bankreg, int frm )
{
    OP(0xD9); OP(0x58 + bankreg); OP((frm^1)<<2); // FST.S [bankreg + frm^1*4]
}

/**
 * Push a 64-bit double onto the FPU stack, with bankreg previously loaded
 * with the location of the current fp bank.
 */
static inline void push_dr( int bankreg, int frm )
{
    OP(0xDD); OP(0x40 + bankreg); OP(frm<<2); // FLD.D [bankreg + frm*4]
}

static inline void pop_dr( int bankreg, int frm )
{
    OP(0xDD); OP(0x58 + bankreg); OP(frm<<2); // FST.D [bankreg + frm*4]
}

/* Exception checks - Note that all exception checks will clobber EAX */

#define check_priv( ) \
    if( !sh4_x86.priv_checked ) { \
	sh4_x86.priv_checked = TRUE;\
	load_spreg( R_EAX, R_SR );\
	AND_imm32_r32( SR_MD, R_EAX );\
	if( sh4_x86.in_delay_slot ) {\
	    JE_exc( EXC_SLOT_ILLEGAL );\
	} else {\
	    JE_exc( EXC_ILLEGAL );\
	}\
    }\

#define check_fpuen( ) \
    if( !sh4_x86.fpuen_checked ) {\
	sh4_x86.fpuen_checked = TRUE;\
	load_spreg( R_EAX, R_SR );\
	AND_imm32_r32( SR_FD, R_EAX );\
	if( sh4_x86.in_delay_slot ) {\
	    JNE_exc(EXC_SLOT_FPU_DISABLED);\
	} else {\
	    JNE_exc(EXC_FPU_DISABLED);\
	}\
    }

#define check_ralign16( x86reg ) \
    TEST_imm32_r32( 0x00000001, x86reg ); \
    JNE_exc(EXC_DATA_ADDR_READ)

#define check_walign16( x86reg ) \
    TEST_imm32_r32( 0x00000001, x86reg ); \
    JNE_exc(EXC_DATA_ADDR_WRITE);

#define check_ralign32( x86reg ) \
    TEST_imm32_r32( 0x00000003, x86reg ); \
    JNE_exc(EXC_DATA_ADDR_READ)

#define check_walign32( x86reg ) \
    TEST_imm32_r32( 0x00000003, x86reg ); \
    JNE_exc(EXC_DATA_ADDR_WRITE);

#define UNDEF()
#define MEM_RESULT(value_reg) if(value_reg != R_EAX) { MOV_r32_r32(R_EAX,value_reg); }
#define MEM_READ_BYTE( addr_reg, value_reg ) call_func1(sh4_read_byte, addr_reg ); MEM_RESULT(value_reg)
#define MEM_READ_WORD( addr_reg, value_reg ) call_func1(sh4_read_word, addr_reg ); MEM_RESULT(value_reg)
#define MEM_READ_LONG( addr_reg, value_reg ) call_func1(sh4_read_long, addr_reg ); MEM_RESULT(value_reg)
#define MEM_WRITE_BYTE( addr_reg, value_reg ) call_func2(sh4_write_byte, addr_reg, value_reg)
#define MEM_WRITE_WORD( addr_reg, value_reg ) call_func2(sh4_write_word, addr_reg, value_reg)
#define MEM_WRITE_LONG( addr_reg, value_reg ) call_func2(sh4_write_long, addr_reg, value_reg)

/**
 * Perform MMU translation on the address in addr_reg for a read operation, iff the TLB is turned 
 * on, otherwise do nothing. Clobbers EAX, ECX and EDX. May raise a TLB exception or address error.
 */
#define MMU_TRANSLATE_READ( addr_reg ) if( sh4_x86.tlb_on ) { call_func1(mmu_vma_to_phys_read, addr_reg); CMP_imm32_r32(MMU_VMA_ERROR, R_EAX); JE_exc(-1); MEM_RESULT(addr_reg); }
/**
 * Perform MMU translation on the address in addr_reg for a write operation, iff the TLB is turned 
 * on, otherwise do nothing. Clobbers EAX, ECX and EDX. May raise a TLB exception or address error.
 */
#define MMU_TRANSLATE_WRITE( addr_reg ) if( sh4_x86.tlb_on ) { call_func1(mmu_vma_to_phys_write, addr_reg); CMP_imm32_r32(MMU_VMA_ERROR, R_EAX); JE_exc(-1); MEM_RESULT(addr_reg); }

#define MEM_READ_SIZE (CALL_FUNC1_SIZE)
#define MEM_WRITE_SIZE (CALL_FUNC2_SIZE)
#define MMU_TRANSLATE_SIZE (sh4_x86.tlb_on ? (CALL_FUNC1_SIZE + 12) : 0 )

#define SLOTILLEGAL() JMP_exc(EXC_SLOT_ILLEGAL); sh4_x86.in_delay_slot = DELAY_NONE; return 1;

/****** Import appropriate calling conventions ******/
#if SH4_TRANSLATOR == TARGET_X86_64
#include "sh4/ia64abi.h"
#else /* SH4_TRANSLATOR == TARGET_X86 */
#ifdef APPLE_BUILD
#include "sh4/ia32mac.h"
#else
#include "sh4/ia32abi.h"
#endif
#endif

/**
 * Embed a breakpoint into the generated code
 */
void sh4_translate_emit_breakpoint( sh4vma_t pc )
{
    load_imm32( R_EAX, pc );
    call_func1( sh4_translate_breakpoint_hit, R_EAX );
}

/**
 * Embed a call to sh4_execute_instruction for situations that we
 * can't translate (mainly page-crossing delay slots at the moment).
 * Caller is responsible for setting new_pc.
 */
void sh4_emulator_exit( sh4vma_t endpc )
{
    load_imm32( R_ECX, endpc - sh4_x86.block_start_pc );   // 5
    ADD_r32_sh4r( R_ECX, R_PC );
    
    load_imm32( R_ECX, ((endpc - sh4_x86.block_start_pc)>>1)*sh4_cpu_period ); // 5
    ADD_r32_sh4r( R_ECX, REG_OFFSET(slice_cycle) );     // 6
    load_imm32( R_ECX, sh4_x86.in_delay_slot ? 1 : 0 );
    store_spreg( R_ECX, REG_OFFSET(in_delay_slot) );

    call_func0( sh4_execute_instruction );    
    load_imm32( R_EAX, R_PC );
    if( sh4_x86.tlb_on ) {
	call_func1(xlat_get_code_by_vma,R_EAX);
    } else {
	call_func1(xlat_get_code,R_EAX);
    }
    AND_imm8s_r32( 0xFC, R_EAX ); // 3
    POP_r32(R_EBP);
    RET();
} 

/**
 * Translate a single instruction. Delayed branches are handled specially
 * by translating both branch and delayed instruction as a single unit (as
 * 
 * The instruction MUST be in the icache (assert check)
 *
 * @return true if the instruction marks the end of a basic block
 * (eg a branch or 
 */
uint32_t sh4_translate_instruction( sh4vma_t pc )
{
    uint32_t ir;
    /* Read instruction from icache */
    assert( IS_IN_ICACHE(pc) );
    ir = *(uint16_t *)GET_ICACHE_PTR(pc);
    
	/* PC is not in the current icache - this usually means we're running
	 * with MMU on, and we've gone past the end of the page. And since 
	 * sh4_translate_block is pretty careful about this, it means we're
	 * almost certainly in a delay slot.
	 *
	 * Since we can't assume the page is present (and we can't fault it in
	 * at this point, inline a call to sh4_execute_instruction (with a few
	 * small repairs to cope with the different environment).
	 */

    if( !sh4_x86.in_delay_slot ) {
	sh4_x86_add_recovery(pc);
    }
        switch( (ir&0xF000) >> 12 ) {
            case 0x0:
                switch( ir&0xF ) {
                    case 0x2:
                        switch( (ir&0x80) >> 7 ) {
                            case 0x0:
                                switch( (ir&0x70) >> 4 ) {
                                    case 0x0:
                                        { /* STC SR, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        call_func0(sh4_read_sr);
                                        store_reg( R_EAX, Rn );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x1:
                                        { /* STC GBR, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        load_spreg( R_EAX, R_GBR );
                                        store_reg( R_EAX, Rn );
                                        }
                                        break;
                                    case 0x2:
                                        { /* STC VBR, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_spreg( R_EAX, R_VBR );
                                        store_reg( R_EAX, Rn );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x3:
                                        { /* STC SSR, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_spreg( R_EAX, R_SSR );
                                        store_reg( R_EAX, Rn );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x4:
                                        { /* STC SPC, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_spreg( R_EAX, R_SPC );
                                        store_reg( R_EAX, Rn );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    default:
                                        UNDEF();
                                        break;
                                }
                                break;
                            case 0x1:
                                { /* STC Rm_BANK, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm_BANK = ((ir>>4)&0x7); 
                                check_priv();
                                load_spreg( R_EAX, REG_OFFSET(r_bank[Rm_BANK]) );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                        }
                        break;
                    case 0x3:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* BSRF Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                if( sh4_x86.in_delay_slot ) {
                            	SLOTILLEGAL();
                                } else {
                            	load_spreg( R_EAX, R_PC );
                            	ADD_imm32_r32( pc + 4 - sh4_x86.block_start_pc, R_EAX );
                            	store_spreg( R_EAX, R_PR );
                            	ADD_sh4r_r32( REG_OFFSET(r[Rn]), R_EAX );
                            	store_spreg( R_EAX, R_NEW_PC );
                            
                            	sh4_x86.tstate = TSTATE_NONE;
                            	sh4_translate_instruction( pc + 2 );
                            	exit_block_newpcset(pc+2);
                            	sh4_x86.branch_taken = TRUE;
                            	return 4;
                                }
                                }
                                break;
                            case 0x2:
                                { /* BRAF Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                if( sh4_x86.in_delay_slot ) {
                            	SLOTILLEGAL();
                                } else {
                            	load_spreg( R_EAX, R_PC );
                            	ADD_imm32_r32( pc + 4 - sh4_x86.block_start_pc, R_EAX );
                            	ADD_sh4r_r32( REG_OFFSET(r[Rn]), R_EAX );
                            	store_spreg( R_EAX, R_NEW_PC );
                            	sh4_x86.in_delay_slot = DELAY_PC;
                            	sh4_x86.tstate = TSTATE_NONE;
                            	sh4_translate_instruction( pc + 2 );
                            	exit_block_newpcset(pc+2);
                            	sh4_x86.branch_taken = TRUE;
                            	return 4;
                                }
                                }
                                break;
                            case 0x8:
                                { /* PREF @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                MOV_r32_r32( R_EAX, R_ECX );
                                AND_imm32_r32( 0xFC000000, R_EAX );
                                CMP_imm32_r32( 0xE0000000, R_EAX );
                                JNE_rel8(8+CALL_FUNC1_SIZE, end);
                                call_func1( sh4_flush_store_queue, R_ECX );
                                TEST_r32_r32( R_EAX, R_EAX );
                                JE_exc(-1);
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x9:
                                { /* OCBI @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                }
                                break;
                            case 0xA:
                                { /* OCBP @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                }
                                break;
                            case 0xB:
                                { /* OCBWB @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                }
                                break;
                            case 0xC:
                                { /* MOVCA.L R0, @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_reg( R_EDX, 0 );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x4:
                        { /* MOV.B Rm, @(R0, Rn) */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, 0 );
                        load_reg( R_ECX, Rn );
                        ADD_r32_r32( R_ECX, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        MEM_WRITE_BYTE( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x5:
                        { /* MOV.W Rm, @(R0, Rn) */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, 0 );
                        load_reg( R_ECX, Rn );
                        ADD_r32_r32( R_ECX, R_EAX );
                        check_walign16( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        MEM_WRITE_WORD( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x6:
                        { /* MOV.L Rm, @(R0, Rn) */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, 0 );
                        load_reg( R_ECX, Rn );
                        ADD_r32_r32( R_ECX, R_EAX );
                        check_walign32( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        MEM_WRITE_LONG( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x7:
                        { /* MUL.L Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        MUL_r32( R_ECX );
                        store_spreg( R_EAX, R_MACL );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x8:
                        switch( (ir&0xFF0) >> 4 ) {
                            case 0x0:
                                { /* CLRT */
                                CLC();
                                SETC_t();
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            case 0x1:
                                { /* SETT */
                                STC();
                                SETC_t();
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            case 0x2:
                                { /* CLRMAC */
                                XOR_r32_r32(R_EAX, R_EAX);
                                store_spreg( R_EAX, R_MACL );
                                store_spreg( R_EAX, R_MACH );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x3:
                                { /* LDTLB */
                                call_func0( MMU_ldtlb );
                                }
                                break;
                            case 0x4:
                                { /* CLRS */
                                CLC();
                                SETC_sh4r(R_S);
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            case 0x5:
                                { /* SETS */
                                STC();
                                SETC_sh4r(R_S);
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x9:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* NOP */
                                /* Do nothing. Well, we could emit an 0x90, but what would really be the point? */
                                }
                                break;
                            case 0x1:
                                { /* DIV0U */
                                XOR_r32_r32( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_Q );
                                store_spreg( R_EAX, R_M );
                                store_spreg( R_EAX, R_T );
                                sh4_x86.tstate = TSTATE_C; // works for DIV1
                                }
                                break;
                            case 0x2:
                                { /* MOVT Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_spreg( R_EAX, R_T );
                                store_reg( R_EAX, Rn );
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0xA:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* STS MACH, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_spreg( R_EAX, R_MACH );
                                store_reg( R_EAX, Rn );
                                }
                                break;
                            case 0x1:
                                { /* STS MACL, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_spreg( R_EAX, R_MACL );
                                store_reg( R_EAX, Rn );
                                }
                                break;
                            case 0x2:
                                { /* STS PR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_spreg( R_EAX, R_PR );
                                store_reg( R_EAX, Rn );
                                }
                                break;
                            case 0x3:
                                { /* STC SGR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                check_priv();
                                load_spreg( R_EAX, R_SGR );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x5:
                                { /* STS FPUL, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_spreg( R_EAX, R_FPUL );
                                store_reg( R_EAX, Rn );
                                }
                                break;
                            case 0x6:
                                { /* STS FPSCR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_spreg( R_EAX, R_FPSCR );
                                store_reg( R_EAX, Rn );
                                }
                                break;
                            case 0xF:
                                { /* STC DBR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                check_priv();
                                load_spreg( R_EAX, R_DBR );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0xB:
                        switch( (ir&0xFF0) >> 4 ) {
                            case 0x0:
                                { /* RTS */
                                if( sh4_x86.in_delay_slot ) {
                            	SLOTILLEGAL();
                                } else {
                            	load_spreg( R_ECX, R_PR );
                            	store_spreg( R_ECX, R_NEW_PC );
                            	sh4_x86.in_delay_slot = DELAY_PC;
                            	sh4_translate_instruction(pc+2);
                            	exit_block_newpcset(pc+2);
                            	sh4_x86.branch_taken = TRUE;
                            	return 4;
                                }
                                }
                                break;
                            case 0x1:
                                { /* SLEEP */
                                check_priv();
                                call_func0( sh4_sleep );
                                sh4_x86.tstate = TSTATE_NONE;
                                sh4_x86.in_delay_slot = DELAY_NONE;
                                return 2;
                                }
                                break;
                            case 0x2:
                                { /* RTE */
                                if( sh4_x86.in_delay_slot ) {
                            	SLOTILLEGAL();
                                } else {
                            	check_priv();
                            	load_spreg( R_ECX, R_SPC );
                            	store_spreg( R_ECX, R_NEW_PC );
                            	load_spreg( R_EAX, R_SSR );
                            	call_func1( sh4_write_sr, R_EAX );
                            	sh4_x86.in_delay_slot = DELAY_PC;
                            	sh4_x86.priv_checked = FALSE;
                            	sh4_x86.fpuen_checked = FALSE;
                            	sh4_x86.tstate = TSTATE_NONE;
                            	sh4_translate_instruction(pc+2);
                            	exit_block_newpcset(pc+2);
                            	sh4_x86.branch_taken = TRUE;
                            	return 4;
                                }
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0xC:
                        { /* MOV.B @(R0, Rm), Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, 0 );
                        load_reg( R_ECX, Rm );
                        ADD_r32_r32( R_ECX, R_EAX );
                        MMU_TRANSLATE_READ( R_EAX )
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xD:
                        { /* MOV.W @(R0, Rm), Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, 0 );
                        load_reg( R_ECX, Rm );
                        ADD_r32_r32( R_ECX, R_EAX );
                        check_ralign16( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_WORD( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xE:
                        { /* MOV.L @(R0, Rm), Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, 0 );
                        load_reg( R_ECX, Rm );
                        ADD_r32_r32( R_ECX, R_EAX );
                        check_ralign32( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_LONG( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xF:
                        { /* MAC.L @Rm+, @Rn+ */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        if( Rm == Rn ) {
                    	load_reg( R_EAX, Rm );
                    	check_ralign32( R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	PUSH_realigned_r32( R_EAX );
                    	load_reg( R_EAX, Rn );
                    	ADD_imm8s_r32( 4, R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	ADD_imm8s_sh4r( 8, REG_OFFSET(r[Rn]) );
                    	// Note translate twice in case of page boundaries. Maybe worth
                    	// adding a page-boundary check to skip the second translation
                        } else {
                    	load_reg( R_EAX, Rm );
                    	check_ralign32( R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	PUSH_realigned_r32( R_EAX );
                    	load_reg( R_EAX, Rn );
                    	check_ralign32( R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rn]) );
                    	ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                        }
                        MEM_READ_LONG( R_EAX, R_EAX );
                        POP_r32( R_ECX );
                        PUSH_r32( R_EAX );
                        MEM_READ_LONG( R_ECX, R_EAX );
                        POP_realigned_r32( R_ECX );
                    
                        IMUL_r32( R_ECX );
                        ADD_r32_sh4r( R_EAX, R_MACL );
                        ADC_r32_sh4r( R_EDX, R_MACH );
                    
                        load_spreg( R_ECX, R_S );
                        TEST_r32_r32(R_ECX, R_ECX);
                        JE_rel8( CALL_FUNC0_SIZE, nosat );
                        call_func0( signsat48 );
                        JMP_TARGET( nosat );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    default:
                        UNDEF();
                        break;
                }
                break;
            case 0x1:
                { /* MOV.L Rm, @(disp, Rn) */
                uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); uint32_t disp = (ir&0xF)<<2; 
                load_reg( R_EAX, Rn );
                ADD_imm32_r32( disp, R_EAX );
                check_walign32( R_EAX );
                MMU_TRANSLATE_WRITE( R_EAX );
                load_reg( R_EDX, Rm );
                MEM_WRITE_LONG( R_EAX, R_EDX );
                sh4_x86.tstate = TSTATE_NONE;
                }
                break;
            case 0x2:
                switch( ir&0xF ) {
                    case 0x0:
                        { /* MOV.B Rm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rn );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        MEM_WRITE_BYTE( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x1:
                        { /* MOV.W Rm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rn );
                        check_walign16( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX )
                        load_reg( R_EDX, Rm );
                        MEM_WRITE_WORD( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x2:
                        { /* MOV.L Rm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rn );
                        check_walign32(R_EAX);
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        MEM_WRITE_LONG( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x4:
                        { /* MOV.B Rm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rn );
                        ADD_imm8s_r32( -1, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        ADD_imm8s_sh4r( -1, REG_OFFSET(r[Rn]) );
                        MEM_WRITE_BYTE( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x5:
                        { /* MOV.W Rm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rn );
                        ADD_imm8s_r32( -2, R_EAX );
                        check_walign16( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        ADD_imm8s_sh4r( -2, REG_OFFSET(r[Rn]) );
                        MEM_WRITE_WORD( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x6:
                        { /* MOV.L Rm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rn );
                        ADD_imm8s_r32( -4, R_EAX );
                        check_walign32( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, Rm );
                        ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                        MEM_WRITE_LONG( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x7:
                        { /* DIV0S Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        SHR_imm8_r32( 31, R_EAX );
                        SHR_imm8_r32( 31, R_ECX );
                        store_spreg( R_EAX, R_M );
                        store_spreg( R_ECX, R_Q );
                        CMP_r32_r32( R_EAX, R_ECX );
                        SETNE_t();
                        sh4_x86.tstate = TSTATE_NE;
                        }
                        break;
                    case 0x8:
                        { /* TST Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        TEST_r32_r32( R_EAX, R_ECX );
                        SETE_t();
                        sh4_x86.tstate = TSTATE_E;
                        }
                        break;
                    case 0x9:
                        { /* AND Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        AND_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xA:
                        { /* XOR Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        XOR_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xB:
                        { /* OR Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        OR_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xC:
                        { /* CMP/STR Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        XOR_r32_r32( R_ECX, R_EAX );
                        TEST_r8_r8( R_AL, R_AL );
                        JE_rel8(13, target1);
                        TEST_r8_r8( R_AH, R_AH ); // 2
                        JE_rel8(9, target2);
                        SHR_imm8_r32( 16, R_EAX ); // 3
                        TEST_r8_r8( R_AL, R_AL ); // 2
                        JE_rel8(2, target3);
                        TEST_r8_r8( R_AH, R_AH ); // 2
                        JMP_TARGET(target1);
                        JMP_TARGET(target2);
                        JMP_TARGET(target3);
                        SETE_t();
                        sh4_x86.tstate = TSTATE_E;
                        }
                        break;
                    case 0xD:
                        { /* XTRCT Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        SHL_imm8_r32( 16, R_EAX );
                        SHR_imm8_r32( 16, R_ECX );
                        OR_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xE:
                        { /* MULU.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg16u( R_EAX, Rm );
                        load_reg16u( R_ECX, Rn );
                        MUL_r32( R_ECX );
                        store_spreg( R_EAX, R_MACL );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xF:
                        { /* MULS.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg16s( R_EAX, Rm );
                        load_reg16s( R_ECX, Rn );
                        MUL_r32( R_ECX );
                        store_spreg( R_EAX, R_MACL );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    default:
                        UNDEF();
                        break;
                }
                break;
            case 0x3:
                switch( ir&0xF ) {
                    case 0x0:
                        { /* CMP/EQ Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        CMP_r32_r32( R_EAX, R_ECX );
                        SETE_t();
                        sh4_x86.tstate = TSTATE_E;
                        }
                        break;
                    case 0x2:
                        { /* CMP/HS Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        CMP_r32_r32( R_EAX, R_ECX );
                        SETAE_t();
                        sh4_x86.tstate = TSTATE_AE;
                        }
                        break;
                    case 0x3:
                        { /* CMP/GE Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        CMP_r32_r32( R_EAX, R_ECX );
                        SETGE_t();
                        sh4_x86.tstate = TSTATE_GE;
                        }
                        break;
                    case 0x4:
                        { /* DIV1 Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_spreg( R_ECX, R_M );
                        load_reg( R_EAX, Rn );
                        if( sh4_x86.tstate != TSTATE_C ) {
                    	LDC_t();
                        }
                        RCL1_r32( R_EAX );
                        SETC_r8( R_DL ); // Q'
                        CMP_sh4r_r32( R_Q, R_ECX );
                        JE_rel8(5, mqequal);
                        ADD_sh4r_r32( REG_OFFSET(r[Rm]), R_EAX );
                        JMP_rel8(3, end);
                        JMP_TARGET(mqequal);
                        SUB_sh4r_r32( REG_OFFSET(r[Rm]), R_EAX );
                        JMP_TARGET(end);
                        store_reg( R_EAX, Rn ); // Done with Rn now
                        SETC_r8(R_AL); // tmp1
                        XOR_r8_r8( R_DL, R_AL ); // Q' = Q ^ tmp1
                        XOR_r8_r8( R_AL, R_CL ); // Q'' = Q' ^ M
                        store_spreg( R_ECX, R_Q );
                        XOR_imm8s_r32( 1, R_AL );   // T = !Q'
                        MOVZX_r8_r32( R_AL, R_EAX );
                        store_spreg( R_EAX, R_T );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x5:
                        { /* DMULU.L Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        MUL_r32(R_ECX);
                        store_spreg( R_EDX, R_MACH );
                        store_spreg( R_EAX, R_MACL );    
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x6:
                        { /* CMP/HI Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        CMP_r32_r32( R_EAX, R_ECX );
                        SETA_t();
                        sh4_x86.tstate = TSTATE_A;
                        }
                        break;
                    case 0x7:
                        { /* CMP/GT Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        CMP_r32_r32( R_EAX, R_ECX );
                        SETG_t();
                        sh4_x86.tstate = TSTATE_G;
                        }
                        break;
                    case 0x8:
                        { /* SUB Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        SUB_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xA:
                        { /* SUBC Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        if( sh4_x86.tstate != TSTATE_C ) {
                    	LDC_t();
                        }
                        SBB_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        SETC_t();
                        sh4_x86.tstate = TSTATE_C;
                        }
                        break;
                    case 0xB:
                        { /* SUBV Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        SUB_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        SETO_t();
                        sh4_x86.tstate = TSTATE_O;
                        }
                        break;
                    case 0xC:
                        { /* ADD Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        ADD_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xD:
                        { /* DMULS.L Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        IMUL_r32(R_ECX);
                        store_spreg( R_EDX, R_MACH );
                        store_spreg( R_EAX, R_MACL );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xE:
                        { /* ADDC Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        if( sh4_x86.tstate != TSTATE_C ) {
                    	LDC_t();
                        }
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        ADC_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        SETC_t();
                        sh4_x86.tstate = TSTATE_C;
                        }
                        break;
                    case 0xF:
                        { /* ADDV Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        load_reg( R_ECX, Rn );
                        ADD_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        SETO_t();
                        sh4_x86.tstate = TSTATE_O;
                        }
                        break;
                    default:
                        UNDEF();
                        break;
                }
                break;
            case 0x4:
                switch( ir&0xF ) {
                    case 0x0:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* SHLL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHL1_r32( R_EAX );
                                SETC_t();
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            case 0x1:
                                { /* DT Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                ADD_imm8s_r32( -1, R_EAX );
                                store_reg( R_EAX, Rn );
                                SETE_t();
                                sh4_x86.tstate = TSTATE_E;
                                }
                                break;
                            case 0x2:
                                { /* SHAL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHL1_r32( R_EAX );
                                SETC_t();
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x1:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* SHLR Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHR1_r32( R_EAX );
                                SETC_t();
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            case 0x1:
                                { /* CMP/PZ Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                CMP_imm8s_r32( 0, R_EAX );
                                SETGE_t();
                                sh4_x86.tstate = TSTATE_GE;
                                }
                                break;
                            case 0x2:
                                { /* SHAR Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SAR1_r32( R_EAX );
                                SETC_t();
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x2:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* STS.L MACH, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, R_MACH );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x1:
                                { /* STS.L MACL, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, R_MACL );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x2:
                                { /* STS.L PR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, R_PR );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x3:
                                { /* STC.L SGR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                check_priv();
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, R_SGR );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x5:
                                { /* STS.L FPUL, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, R_FPUL );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x6:
                                { /* STS.L FPSCR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, R_FPSCR );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0xF:
                                { /* STC.L DBR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                check_priv();
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, R_DBR );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x3:
                        switch( (ir&0x80) >> 7 ) {
                            case 0x0:
                                switch( (ir&0x70) >> 4 ) {
                                    case 0x0:
                                        { /* STC.L SR, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rn );
                                        check_walign32( R_EAX );
                                        ADD_imm8s_r32( -4, R_EAX );
                                        MMU_TRANSLATE_WRITE( R_EAX );
                                        PUSH_realigned_r32( R_EAX );
                                        call_func0( sh4_read_sr );
                                        POP_realigned_r32( R_ECX );
                                        ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                        MEM_WRITE_LONG( R_ECX, R_EAX );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x1:
                                        { /* STC.L GBR, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        load_reg( R_EAX, Rn );
                                        check_walign32( R_EAX );
                                        ADD_imm8s_r32( -4, R_EAX );
                                        MMU_TRANSLATE_WRITE( R_EAX );
                                        load_spreg( R_EDX, R_GBR );
                                        ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                        MEM_WRITE_LONG( R_EAX, R_EDX );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x2:
                                        { /* STC.L VBR, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rn );
                                        check_walign32( R_EAX );
                                        ADD_imm8s_r32( -4, R_EAX );
                                        MMU_TRANSLATE_WRITE( R_EAX );
                                        load_spreg( R_EDX, R_VBR );
                                        ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                        MEM_WRITE_LONG( R_EAX, R_EDX );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x3:
                                        { /* STC.L SSR, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rn );
                                        check_walign32( R_EAX );
                                        ADD_imm8s_r32( -4, R_EAX );
                                        MMU_TRANSLATE_WRITE( R_EAX );
                                        load_spreg( R_EDX, R_SSR );
                                        ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                        MEM_WRITE_LONG( R_EAX, R_EDX );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x4:
                                        { /* STC.L SPC, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rn );
                                        check_walign32( R_EAX );
                                        ADD_imm8s_r32( -4, R_EAX );
                                        MMU_TRANSLATE_WRITE( R_EAX );
                                        load_spreg( R_EDX, R_SPC );
                                        ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                        MEM_WRITE_LONG( R_EAX, R_EDX );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    default:
                                        UNDEF();
                                        break;
                                }
                                break;
                            case 0x1:
                                { /* STC.L Rm_BANK, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm_BANK = ((ir>>4)&0x7); 
                                check_priv();
                                load_reg( R_EAX, Rn );
                                check_walign32( R_EAX );
                                ADD_imm8s_r32( -4, R_EAX );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                load_spreg( R_EDX, REG_OFFSET(r_bank[Rm_BANK]) );
                                ADD_imm8s_sh4r( -4, REG_OFFSET(r[Rn]) );
                                MEM_WRITE_LONG( R_EAX, R_EDX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                        }
                        break;
                    case 0x4:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* ROTL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                ROL1_r32( R_EAX );
                                store_reg( R_EAX, Rn );
                                SETC_t();
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            case 0x2:
                                { /* ROTCL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                if( sh4_x86.tstate != TSTATE_C ) {
                            	LDC_t();
                                }
                                RCL1_r32( R_EAX );
                                store_reg( R_EAX, Rn );
                                SETC_t();
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x5:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* ROTR Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                ROR1_r32( R_EAX );
                                store_reg( R_EAX, Rn );
                                SETC_t();
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            case 0x1:
                                { /* CMP/PL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                CMP_imm8s_r32( 0, R_EAX );
                                SETG_t();
                                sh4_x86.tstate = TSTATE_G;
                                }
                                break;
                            case 0x2:
                                { /* ROTCR Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                if( sh4_x86.tstate != TSTATE_C ) {
                            	LDC_t();
                                }
                                RCR1_r32( R_EAX );
                                store_reg( R_EAX, Rn );
                                SETC_t();
                                sh4_x86.tstate = TSTATE_C;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x6:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* LDS.L @Rm+, MACH */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_MACH );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x1:
                                { /* LDS.L @Rm+, MACL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_MACL );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x2:
                                { /* LDS.L @Rm+, PR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_PR );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x3:
                                { /* LDC.L @Rm+, SGR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                check_priv();
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_SGR );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x5:
                                { /* LDS.L @Rm+, FPUL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_FPUL );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x6:
                                { /* LDS.L @Rm+, FPSCR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_FPSCR );
                                update_fr_bank( R_EAX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0xF:
                                { /* LDC.L @Rm+, DBR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                check_priv();
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, R_DBR );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x7:
                        switch( (ir&0x80) >> 7 ) {
                            case 0x0:
                                switch( (ir&0x70) >> 4 ) {
                                    case 0x0:
                                        { /* LDC.L @Rm+, SR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        if( sh4_x86.in_delay_slot ) {
                                    	SLOTILLEGAL();
                                        } else {
                                    	check_priv();
                                    	load_reg( R_EAX, Rm );
                                    	check_ralign32( R_EAX );
                                    	MMU_TRANSLATE_READ( R_EAX );
                                    	ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                    	MEM_READ_LONG( R_EAX, R_EAX );
                                    	call_func1( sh4_write_sr, R_EAX );
                                    	sh4_x86.priv_checked = FALSE;
                                    	sh4_x86.fpuen_checked = FALSE;
                                    	sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        }
                                        break;
                                    case 0x1:
                                        { /* LDC.L @Rm+, GBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        load_reg( R_EAX, Rm );
                                        check_ralign32( R_EAX );
                                        MMU_TRANSLATE_READ( R_EAX );
                                        ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                        MEM_READ_LONG( R_EAX, R_EAX );
                                        store_spreg( R_EAX, R_GBR );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x2:
                                        { /* LDC.L @Rm+, VBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rm );
                                        check_ralign32( R_EAX );
                                        MMU_TRANSLATE_READ( R_EAX );
                                        ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                        MEM_READ_LONG( R_EAX, R_EAX );
                                        store_spreg( R_EAX, R_VBR );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x3:
                                        { /* LDC.L @Rm+, SSR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rm );
                                        check_ralign32( R_EAX );
                                        MMU_TRANSLATE_READ( R_EAX );
                                        ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                        MEM_READ_LONG( R_EAX, R_EAX );
                                        store_spreg( R_EAX, R_SSR );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x4:
                                        { /* LDC.L @Rm+, SPC */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rm );
                                        check_ralign32( R_EAX );
                                        MMU_TRANSLATE_READ( R_EAX );
                                        ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                        MEM_READ_LONG( R_EAX, R_EAX );
                                        store_spreg( R_EAX, R_SPC );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    default:
                                        UNDEF();
                                        break;
                                }
                                break;
                            case 0x1:
                                { /* LDC.L @Rm+, Rn_BANK */
                                uint32_t Rm = ((ir>>8)&0xF); uint32_t Rn_BANK = ((ir>>4)&0x7); 
                                check_priv();
                                load_reg( R_EAX, Rm );
                                check_ralign32( R_EAX );
                                MMU_TRANSLATE_READ( R_EAX );
                                ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                                MEM_READ_LONG( R_EAX, R_EAX );
                                store_spreg( R_EAX, REG_OFFSET(r_bank[Rn_BANK]) );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                        }
                        break;
                    case 0x8:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* SHLL2 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHL_imm8_r32( 2, R_EAX );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x1:
                                { /* SHLL8 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHL_imm8_r32( 8, R_EAX );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x2:
                                { /* SHLL16 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHL_imm8_r32( 16, R_EAX );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0x9:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* SHLR2 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHR_imm8_r32( 2, R_EAX );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x1:
                                { /* SHLR8 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHR_imm8_r32( 8, R_EAX );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x2:
                                { /* SHLR16 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                SHR_imm8_r32( 16, R_EAX );
                                store_reg( R_EAX, Rn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0xA:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* LDS Rm, MACH */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, R_MACH );
                                }
                                break;
                            case 0x1:
                                { /* LDS Rm, MACL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, R_MACL );
                                }
                                break;
                            case 0x2:
                                { /* LDS Rm, PR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, R_PR );
                                }
                                break;
                            case 0x3:
                                { /* LDC Rm, SGR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                check_priv();
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, R_SGR );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x5:
                                { /* LDS Rm, FPUL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, R_FPUL );
                                }
                                break;
                            case 0x6:
                                { /* LDS Rm, FPSCR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, R_FPSCR );
                                update_fr_bank( R_EAX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0xF:
                                { /* LDC Rm, DBR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                check_priv();
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, R_DBR );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0xB:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* JSR @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                if( sh4_x86.in_delay_slot ) {
                            	SLOTILLEGAL();
                                } else {
                            	load_spreg( R_EAX, R_PC );
                            	ADD_imm32_r32( pc + 4 - sh4_x86.block_start_pc, R_EAX );
                            	store_spreg( R_EAX, R_PR );
                            	load_reg( R_ECX, Rn );
                            	store_spreg( R_ECX, R_NEW_PC );
                            	sh4_translate_instruction(pc+2);
                            	exit_block_newpcset(pc+2);
                            	sh4_x86.branch_taken = TRUE;
                            	return 4;
                                }
                                }
                                break;
                            case 0x1:
                                { /* TAS.B @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                load_reg( R_EAX, Rn );
                                MMU_TRANSLATE_WRITE( R_EAX );
                                PUSH_realigned_r32( R_EAX );
                                MEM_READ_BYTE( R_EAX, R_EAX );
                                TEST_r8_r8( R_AL, R_AL );
                                SETE_t();
                                OR_imm8_r8( 0x80, R_AL );
                                POP_realigned_r32( R_ECX );
                                MEM_WRITE_BYTE( R_ECX, R_EAX );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x2:
                                { /* JMP @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                if( sh4_x86.in_delay_slot ) {
                            	SLOTILLEGAL();
                                } else {
                            	load_reg( R_ECX, Rn );
                            	store_spreg( R_ECX, R_NEW_PC );
                            	sh4_x86.in_delay_slot = DELAY_PC;
                            	sh4_translate_instruction(pc+2);
                            	exit_block_newpcset(pc+2);
                            	sh4_x86.branch_taken = TRUE;
                            	return 4;
                                }
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0xC:
                        { /* SHAD Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        /* Annoyingly enough, not directly convertible */
                        load_reg( R_EAX, Rn );
                        load_reg( R_ECX, Rm );
                        CMP_imm32_r32( 0, R_ECX );
                        JGE_rel8(16, doshl);
                                        
                        NEG_r32( R_ECX );      // 2
                        AND_imm8_r8( 0x1F, R_CL ); // 3
                        JE_rel8( 4, emptysar);     // 2
                        SAR_r32_CL( R_EAX );       // 2
                        JMP_rel8(10, end);          // 2
                    
                        JMP_TARGET(emptysar);
                        SAR_imm8_r32(31, R_EAX );  // 3
                        JMP_rel8(5, end2);
                    
                        JMP_TARGET(doshl);
                        AND_imm8_r8( 0x1F, R_CL ); // 3
                        SHL_r32_CL( R_EAX );       // 2
                        JMP_TARGET(end);
                        JMP_TARGET(end2);
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xD:
                        { /* SHLD Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rn );
                        load_reg( R_ECX, Rm );
                        CMP_imm32_r32( 0, R_ECX );
                        JGE_rel8(15, doshl);
                    
                        NEG_r32( R_ECX );      // 2
                        AND_imm8_r8( 0x1F, R_CL ); // 3
                        JE_rel8( 4, emptyshr );
                        SHR_r32_CL( R_EAX );       // 2
                        JMP_rel8(9, end);          // 2
                    
                        JMP_TARGET(emptyshr);
                        XOR_r32_r32( R_EAX, R_EAX );
                        JMP_rel8(5, end2);
                    
                        JMP_TARGET(doshl);
                        AND_imm8_r8( 0x1F, R_CL ); // 3
                        SHL_r32_CL( R_EAX );       // 2
                        JMP_TARGET(end);
                        JMP_TARGET(end2);
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xE:
                        switch( (ir&0x80) >> 7 ) {
                            case 0x0:
                                switch( (ir&0x70) >> 4 ) {
                                    case 0x0:
                                        { /* LDC Rm, SR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        if( sh4_x86.in_delay_slot ) {
                                    	SLOTILLEGAL();
                                        } else {
                                    	check_priv();
                                    	load_reg( R_EAX, Rm );
                                    	call_func1( sh4_write_sr, R_EAX );
                                    	sh4_x86.priv_checked = FALSE;
                                    	sh4_x86.fpuen_checked = FALSE;
                                    	sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        }
                                        break;
                                    case 0x1:
                                        { /* LDC Rm, GBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        load_reg( R_EAX, Rm );
                                        store_spreg( R_EAX, R_GBR );
                                        }
                                        break;
                                    case 0x2:
                                        { /* LDC Rm, VBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rm );
                                        store_spreg( R_EAX, R_VBR );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x3:
                                        { /* LDC Rm, SSR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rm );
                                        store_spreg( R_EAX, R_SSR );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x4:
                                        { /* LDC Rm, SPC */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        check_priv();
                                        load_reg( R_EAX, Rm );
                                        store_spreg( R_EAX, R_SPC );
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    default:
                                        UNDEF();
                                        break;
                                }
                                break;
                            case 0x1:
                                { /* LDC Rm, Rn_BANK */
                                uint32_t Rm = ((ir>>8)&0xF); uint32_t Rn_BANK = ((ir>>4)&0x7); 
                                check_priv();
                                load_reg( R_EAX, Rm );
                                store_spreg( R_EAX, REG_OFFSET(r_bank[Rn_BANK]) );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                        }
                        break;
                    case 0xF:
                        { /* MAC.W @Rm+, @Rn+ */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        if( Rm == Rn ) {
                    	load_reg( R_EAX, Rm );
                    	check_ralign16( R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	PUSH_realigned_r32( R_EAX );
                    	load_reg( R_EAX, Rn );
                    	ADD_imm8s_r32( 2, R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rn]) );
                    	// Note translate twice in case of page boundaries. Maybe worth
                    	// adding a page-boundary check to skip the second translation
                        } else {
                    	load_reg( R_EAX, Rm );
                    	check_ralign16( R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	PUSH_realigned_r32( R_EAX );
                    	load_reg( R_EAX, Rn );
                    	check_ralign16( R_EAX );
                    	MMU_TRANSLATE_READ( R_EAX );
                    	ADD_imm8s_sh4r( 2, REG_OFFSET(r[Rn]) );
                    	ADD_imm8s_sh4r( 2, REG_OFFSET(r[Rm]) );
                        }
                        MEM_READ_WORD( R_EAX, R_EAX );
                        POP_r32( R_ECX );
                        PUSH_r32( R_EAX );
                        MEM_READ_WORD( R_ECX, R_EAX );
                        POP_realigned_r32( R_ECX );
                        IMUL_r32( R_ECX );
                    
                        load_spreg( R_ECX, R_S );
                        TEST_r32_r32( R_ECX, R_ECX );
                        JE_rel8( 47, nosat );
                    
                        ADD_r32_sh4r( R_EAX, R_MACL );  // 6
                        JNO_rel8( 51, end );            // 2
                        load_imm32( R_EDX, 1 );         // 5
                        store_spreg( R_EDX, R_MACH );   // 6
                        JS_rel8( 13, positive );        // 2
                        load_imm32( R_EAX, 0x80000000 );// 5
                        store_spreg( R_EAX, R_MACL );   // 6
                        JMP_rel8( 25, end2 );           // 2
                    
                        JMP_TARGET(positive);
                        load_imm32( R_EAX, 0x7FFFFFFF );// 5
                        store_spreg( R_EAX, R_MACL );   // 6
                        JMP_rel8( 12, end3);            // 2
                    
                        JMP_TARGET(nosat);
                        ADD_r32_sh4r( R_EAX, R_MACL );  // 6
                        ADC_r32_sh4r( R_EDX, R_MACH );  // 6
                        JMP_TARGET(end);
                        JMP_TARGET(end2);
                        JMP_TARGET(end3);
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                }
                break;
            case 0x5:
                { /* MOV.L @(disp, Rm), Rn */
                uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); uint32_t disp = (ir&0xF)<<2; 
                load_reg( R_EAX, Rm );
                ADD_imm8s_r32( disp, R_EAX );
                check_ralign32( R_EAX );
                MMU_TRANSLATE_READ( R_EAX );
                MEM_READ_LONG( R_EAX, R_EAX );
                store_reg( R_EAX, Rn );
                sh4_x86.tstate = TSTATE_NONE;
                }
                break;
            case 0x6:
                switch( ir&0xF ) {
                    case 0x0:
                        { /* MOV.B @Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x1:
                        { /* MOV.W @Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        check_ralign16( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_WORD( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x2:
                        { /* MOV.L @Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        check_ralign32( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_LONG( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x3:
                        { /* MOV Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        store_reg( R_EAX, Rn );
                        }
                        break;
                    case 0x4:
                        { /* MOV.B @Rm+, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        MMU_TRANSLATE_READ( R_EAX );
                        ADD_imm8s_sh4r( 1, REG_OFFSET(r[Rm]) );
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x5:
                        { /* MOV.W @Rm+, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        check_ralign16( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        ADD_imm8s_sh4r( 2, REG_OFFSET(r[Rm]) );
                        MEM_READ_WORD( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x6:
                        { /* MOV.L @Rm+, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        check_ralign32( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                        MEM_READ_LONG( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x7:
                        { /* NOT Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        NOT_r32( R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x8:
                        { /* SWAP.B Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        XCHG_r8_r8( R_AL, R_AH );
                        store_reg( R_EAX, Rn );
                        }
                        break;
                    case 0x9:
                        { /* SWAP.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        MOV_r32_r32( R_EAX, R_ECX );
                        SHL_imm8_r32( 16, R_ECX );
                        SHR_imm8_r32( 16, R_EAX );
                        OR_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xA:
                        { /* NEGC Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        XOR_r32_r32( R_ECX, R_ECX );
                        LDC_t();
                        SBB_r32_r32( R_EAX, R_ECX );
                        store_reg( R_ECX, Rn );
                        SETC_t();
                        sh4_x86.tstate = TSTATE_C;
                        }
                        break;
                    case 0xB:
                        { /* NEG Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        NEG_r32( R_EAX );
                        store_reg( R_EAX, Rn );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xC:
                        { /* EXTU.B Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        MOVZX_r8_r32( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        }
                        break;
                    case 0xD:
                        { /* EXTU.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        MOVZX_r16_r32( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        }
                        break;
                    case 0xE:
                        { /* EXTS.B Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        MOVSX_r8_r32( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        }
                        break;
                    case 0xF:
                        { /* EXTS.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        load_reg( R_EAX, Rm );
                        MOVSX_r16_r32( R_EAX, R_EAX );
                        store_reg( R_EAX, Rn );
                        }
                        break;
                }
                break;
            case 0x7:
                { /* ADD #imm, Rn */
                uint32_t Rn = ((ir>>8)&0xF); int32_t imm = SIGNEXT8(ir&0xFF); 
                load_reg( R_EAX, Rn );
                ADD_imm8s_r32( imm, R_EAX );
                store_reg( R_EAX, Rn );
                sh4_x86.tstate = TSTATE_NONE;
                }
                break;
            case 0x8:
                switch( (ir&0xF00) >> 8 ) {
                    case 0x0:
                        { /* MOV.B R0, @(disp, Rn) */
                        uint32_t Rn = ((ir>>4)&0xF); uint32_t disp = (ir&0xF); 
                        load_reg( R_EAX, Rn );
                        ADD_imm32_r32( disp, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, 0 );
                        MEM_WRITE_BYTE( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x1:
                        { /* MOV.W R0, @(disp, Rn) */
                        uint32_t Rn = ((ir>>4)&0xF); uint32_t disp = (ir&0xF)<<1; 
                        load_reg( R_EAX, Rn );
                        ADD_imm32_r32( disp, R_EAX );
                        check_walign16( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, 0 );
                        MEM_WRITE_WORD( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x4:
                        { /* MOV.B @(disp, Rm), R0 */
                        uint32_t Rm = ((ir>>4)&0xF); uint32_t disp = (ir&0xF); 
                        load_reg( R_EAX, Rm );
                        ADD_imm32_r32( disp, R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x5:
                        { /* MOV.W @(disp, Rm), R0 */
                        uint32_t Rm = ((ir>>4)&0xF); uint32_t disp = (ir&0xF)<<1; 
                        load_reg( R_EAX, Rm );
                        ADD_imm32_r32( disp, R_EAX );
                        check_ralign16( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_WORD( R_EAX, R_EAX );
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x8:
                        { /* CMP/EQ #imm, R0 */
                        int32_t imm = SIGNEXT8(ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        CMP_imm8s_r32(imm, R_EAX);
                        SETE_t();
                        sh4_x86.tstate = TSTATE_E;
                        }
                        break;
                    case 0x9:
                        { /* BT disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        if( sh4_x86.in_delay_slot ) {
                    	SLOTILLEGAL();
                        } else {
                    	sh4vma_t target = disp + pc + 4;
                    	JF_rel8( EXIT_BLOCK_REL_SIZE(target), nottaken );
                    	exit_block_rel(target, pc+2 );
                    	JMP_TARGET(nottaken);
                    	return 2;
                        }
                        }
                        break;
                    case 0xB:
                        { /* BF disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        if( sh4_x86.in_delay_slot ) {
                    	SLOTILLEGAL();
                        } else {
                    	sh4vma_t target = disp + pc + 4;
                    	JT_rel8( EXIT_BLOCK_REL_SIZE(target), nottaken );
                    	exit_block_rel(target, pc+2 );
                    	JMP_TARGET(nottaken);
                    	return 2;
                        }
                        }
                        break;
                    case 0xD:
                        { /* BT/S disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        if( sh4_x86.in_delay_slot ) {
                    	SLOTILLEGAL();
                        } else {
                    	sh4_x86.in_delay_slot = DELAY_PC;
                    	if( sh4_x86.tstate == TSTATE_NONE ) {
                    	    CMP_imm8s_sh4r( 1, R_T );
                    	    sh4_x86.tstate = TSTATE_E;
                    	}
                    	OP(0x0F); OP(0x80+(sh4_x86.tstate^1)); uint32_t *patch = (uint32_t *)xlat_output; OP32(0); // JE rel32
                    	sh4_translate_instruction(pc+2);
                    	exit_block_rel( disp + pc + 4, pc+4 );
                    	// not taken
                    	*patch = (xlat_output - ((uint8_t *)patch)) - 4;
                    	sh4_translate_instruction(pc+2);
                    	return 4;
                        }
                        }
                        break;
                    case 0xF:
                        { /* BF/S disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        if( sh4_x86.in_delay_slot ) {
                    	SLOTILLEGAL();
                        } else {
                    	sh4vma_t target = disp + pc + 4;
                    	sh4_x86.in_delay_slot = DELAY_PC;
                    	if( sh4_x86.tstate == TSTATE_NONE ) {
                    	    CMP_imm8s_sh4r( 1, R_T );
                    	    sh4_x86.tstate = TSTATE_E;
                    	}
                    	OP(0x0F); OP(0x80+sh4_x86.tstate); uint32_t *patch = (uint32_t *)xlat_output; OP32(0); // JNE rel32
                    	sh4_translate_instruction(pc+2);
                    	exit_block_rel( target, pc+4 );
                    	// not taken
                    	*patch = (xlat_output - ((uint8_t *)patch)) - 4;
                    	sh4_translate_instruction(pc+2);
                    	return 4;
                        }
                        }
                        break;
                    default:
                        UNDEF();
                        break;
                }
                break;
            case 0x9:
                { /* MOV.W @(disp, PC), Rn */
                uint32_t Rn = ((ir>>8)&0xF); uint32_t disp = (ir&0xFF)<<1; 
                if( sh4_x86.in_delay_slot ) {
            	SLOTILLEGAL();
                } else {
            	// See comments for MOV.L @(disp, PC), Rn
            	uint32_t target = pc + disp + 4;
            	if( IS_IN_ICACHE(target) ) {
            	    sh4ptr_t ptr = GET_ICACHE_PTR(target);
            	    MOV_moff32_EAX( ptr );
            	    MOVSX_r16_r32( R_EAX, R_EAX );
            	} else {
            	    load_imm32( R_EAX, (pc - sh4_x86.block_start_pc) + disp + 4 );
            	    ADD_sh4r_r32( R_PC, R_EAX );
            	    MMU_TRANSLATE_READ( R_EAX );
            	    MEM_READ_WORD( R_EAX, R_EAX );
            	    sh4_x86.tstate = TSTATE_NONE;
            	}
            	store_reg( R_EAX, Rn );
                }
                }
                break;
            case 0xA:
                { /* BRA disp */
                int32_t disp = SIGNEXT12(ir&0xFFF)<<1; 
                if( sh4_x86.in_delay_slot ) {
            	SLOTILLEGAL();
                } else {
            	sh4_x86.in_delay_slot = DELAY_PC;
            	sh4_translate_instruction( pc + 2 );
            	exit_block_rel( disp + pc + 4, pc+4 );
            	sh4_x86.branch_taken = TRUE;
            	return 4;
                }
                }
                break;
            case 0xB:
                { /* BSR disp */
                int32_t disp = SIGNEXT12(ir&0xFFF)<<1; 
                if( sh4_x86.in_delay_slot ) {
            	SLOTILLEGAL();
                } else {
            	load_spreg( R_EAX, R_PC );
            	ADD_imm32_r32( pc + 4 - sh4_x86.block_start_pc, R_EAX );
            	store_spreg( R_EAX, R_PR );
            	sh4_x86.in_delay_slot = DELAY_PC;
            	sh4_translate_instruction( pc + 2 );
            	exit_block_rel( disp + pc + 4, pc+4 );
            	sh4_x86.branch_taken = TRUE;
            	return 4;
                }
                }
                break;
            case 0xC:
                switch( (ir&0xF00) >> 8 ) {
                    case 0x0:
                        { /* MOV.B R0, @(disp, GBR) */
                        uint32_t disp = (ir&0xFF); 
                        load_spreg( R_EAX, R_GBR );
                        ADD_imm32_r32( disp, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, 0 );
                        MEM_WRITE_BYTE( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x1:
                        { /* MOV.W R0, @(disp, GBR) */
                        uint32_t disp = (ir&0xFF)<<1; 
                        load_spreg( R_EAX, R_GBR );
                        ADD_imm32_r32( disp, R_EAX );
                        check_walign16( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, 0 );
                        MEM_WRITE_WORD( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x2:
                        { /* MOV.L R0, @(disp, GBR) */
                        uint32_t disp = (ir&0xFF)<<2; 
                        load_spreg( R_EAX, R_GBR );
                        ADD_imm32_r32( disp, R_EAX );
                        check_walign32( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_reg( R_EDX, 0 );
                        MEM_WRITE_LONG( R_EAX, R_EDX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x3:
                        { /* TRAPA #imm */
                        uint32_t imm = (ir&0xFF); 
                        if( sh4_x86.in_delay_slot ) {
                    	SLOTILLEGAL();
                        } else {
                    	load_imm32( R_ECX, pc+2 - sh4_x86.block_start_pc );   // 5
                    	ADD_r32_sh4r( R_ECX, R_PC );
                    	load_imm32( R_EAX, imm );
                    	call_func1( sh4_raise_trap, R_EAX );
                    	sh4_x86.tstate = TSTATE_NONE;
                    	exit_block_pcset(pc);
                    	sh4_x86.branch_taken = TRUE;
                    	return 2;
                        }
                        }
                        break;
                    case 0x4:
                        { /* MOV.B @(disp, GBR), R0 */
                        uint32_t disp = (ir&0xFF); 
                        load_spreg( R_EAX, R_GBR );
                        ADD_imm32_r32( disp, R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x5:
                        { /* MOV.W @(disp, GBR), R0 */
                        uint32_t disp = (ir&0xFF)<<1; 
                        load_spreg( R_EAX, R_GBR );
                        ADD_imm32_r32( disp, R_EAX );
                        check_ralign16( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_WORD( R_EAX, R_EAX );
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x6:
                        { /* MOV.L @(disp, GBR), R0 */
                        uint32_t disp = (ir&0xFF)<<2; 
                        load_spreg( R_EAX, R_GBR );
                        ADD_imm32_r32( disp, R_EAX );
                        check_ralign32( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_LONG( R_EAX, R_EAX );
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x7:
                        { /* MOVA @(disp, PC), R0 */
                        uint32_t disp = (ir&0xFF)<<2; 
                        if( sh4_x86.in_delay_slot ) {
                    	SLOTILLEGAL();
                        } else {
                    	load_imm32( R_ECX, (pc - sh4_x86.block_start_pc) + disp + 4 - (pc&0x03) );
                    	ADD_sh4r_r32( R_PC, R_ECX );
                    	store_reg( R_ECX, 0 );
                    	sh4_x86.tstate = TSTATE_NONE;
                        }
                        }
                        break;
                    case 0x8:
                        { /* TST #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        TEST_imm32_r32( imm, R_EAX );
                        SETE_t();
                        sh4_x86.tstate = TSTATE_E;
                        }
                        break;
                    case 0x9:
                        { /* AND #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        AND_imm32_r32(imm, R_EAX); 
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xA:
                        { /* XOR #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        XOR_imm32_r32( imm, R_EAX );
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xB:
                        { /* OR #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        OR_imm32_r32(imm, R_EAX);
                        store_reg( R_EAX, 0 );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xC:
                        { /* TST.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0);
                        load_reg( R_ECX, R_GBR);
                        ADD_r32_r32( R_ECX, R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        TEST_imm8_r8( imm, R_AL );
                        SETE_t();
                        sh4_x86.tstate = TSTATE_E;
                        }
                        break;
                    case 0xD:
                        { /* AND.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        load_spreg( R_ECX, R_GBR );
                        ADD_r32_r32( R_ECX, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        PUSH_realigned_r32(R_EAX);
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        POP_realigned_r32(R_ECX);
                        AND_imm32_r32(imm, R_EAX );
                        MEM_WRITE_BYTE( R_ECX, R_EAX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xE:
                        { /* XOR.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        load_spreg( R_ECX, R_GBR );
                        ADD_r32_r32( R_ECX, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        PUSH_realigned_r32(R_EAX);
                        MEM_READ_BYTE(R_EAX, R_EAX);
                        POP_realigned_r32(R_ECX);
                        XOR_imm32_r32( imm, R_EAX );
                        MEM_WRITE_BYTE( R_ECX, R_EAX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xF:
                        { /* OR.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        load_reg( R_EAX, 0 );
                        load_spreg( R_ECX, R_GBR );
                        ADD_r32_r32( R_ECX, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        PUSH_realigned_r32(R_EAX);
                        MEM_READ_BYTE( R_EAX, R_EAX );
                        POP_realigned_r32(R_ECX);
                        OR_imm32_r32(imm, R_EAX );
                        MEM_WRITE_BYTE( R_ECX, R_EAX );
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                }
                break;
            case 0xD:
                { /* MOV.L @(disp, PC), Rn */
                uint32_t Rn = ((ir>>8)&0xF); uint32_t disp = (ir&0xFF)<<2; 
                if( sh4_x86.in_delay_slot ) {
            	SLOTILLEGAL();
                } else {
            	uint32_t target = (pc & 0xFFFFFFFC) + disp + 4;
            	if( IS_IN_ICACHE(target) ) {
            	    // If the target address is in the same page as the code, it's
            	    // pretty safe to just ref it directly and circumvent the whole
            	    // memory subsystem. (this is a big performance win)
            
            	    // FIXME: There's a corner-case that's not handled here when
            	    // the current code-page is in the ITLB but not in the UTLB.
            	    // (should generate a TLB miss although need to test SH4 
            	    // behaviour to confirm) Unlikely to be anyone depending on this
            	    // behaviour though.
            	    sh4ptr_t ptr = GET_ICACHE_PTR(target);
            	    MOV_moff32_EAX( ptr );
            	} else {
            	    // Note: we use sh4r.pc for the calc as we could be running at a
            	    // different virtual address than the translation was done with,
            	    // but we can safely assume that the low bits are the same.
            	    load_imm32( R_EAX, (pc-sh4_x86.block_start_pc) + disp + 4 - (pc&0x03) );
            	    ADD_sh4r_r32( R_PC, R_EAX );
            	    MMU_TRANSLATE_READ( R_EAX );
            	    MEM_READ_LONG( R_EAX, R_EAX );
            	    sh4_x86.tstate = TSTATE_NONE;
            	}
            	store_reg( R_EAX, Rn );
                }
                }
                break;
            case 0xE:
                { /* MOV #imm, Rn */
                uint32_t Rn = ((ir>>8)&0xF); int32_t imm = SIGNEXT8(ir&0xFF); 
                load_imm32( R_EAX, imm );
                store_reg( R_EAX, Rn );
                }
                break;
            case 0xF:
                switch( ir&0xF ) {
                    case 0x0:
                        { /* FADD FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                        load_fr_bank( R_EDX );
                        JNE_rel8(13,doubleprec);
                        push_fr(R_EDX, FRm);
                        push_fr(R_EDX, FRn);
                        FADDP_st(1);
                        pop_fr(R_EDX, FRn);
                        JMP_rel8(11,end);
                        JMP_TARGET(doubleprec);
                        push_dr(R_EDX, FRm);
                        push_dr(R_EDX, FRn);
                        FADDP_st(1);
                        pop_dr(R_EDX, FRn);
                        JMP_TARGET(end);
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x1:
                        { /* FSUB FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                        load_fr_bank( R_EDX );
                        JNE_rel8(13, doubleprec);
                        push_fr(R_EDX, FRn);
                        push_fr(R_EDX, FRm);
                        FSUBP_st(1);
                        pop_fr(R_EDX, FRn);
                        JMP_rel8(11, end);
                        JMP_TARGET(doubleprec);
                        push_dr(R_EDX, FRn);
                        push_dr(R_EDX, FRm);
                        FSUBP_st(1);
                        pop_dr(R_EDX, FRn);
                        JMP_TARGET(end);
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x2:
                        { /* FMUL FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                        load_fr_bank( R_EDX );
                        JNE_rel8(13, doubleprec);
                        push_fr(R_EDX, FRm);
                        push_fr(R_EDX, FRn);
                        FMULP_st(1);
                        pop_fr(R_EDX, FRn);
                        JMP_rel8(11, end);
                        JMP_TARGET(doubleprec);
                        push_dr(R_EDX, FRm);
                        push_dr(R_EDX, FRn);
                        FMULP_st(1);
                        pop_dr(R_EDX, FRn);
                        JMP_TARGET(end);
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x3:
                        { /* FDIV FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                        load_fr_bank( R_EDX );
                        JNE_rel8(13, doubleprec);
                        push_fr(R_EDX, FRn);
                        push_fr(R_EDX, FRm);
                        FDIVP_st(1);
                        pop_fr(R_EDX, FRn);
                        JMP_rel8(11, end);
                        JMP_TARGET(doubleprec);
                        push_dr(R_EDX, FRn);
                        push_dr(R_EDX, FRm);
                        FDIVP_st(1);
                        pop_dr(R_EDX, FRn);
                        JMP_TARGET(end);
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x4:
                        { /* FCMP/EQ FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                        load_fr_bank( R_EDX );
                        JNE_rel8(8, doubleprec);
                        push_fr(R_EDX, FRm);
                        push_fr(R_EDX, FRn);
                        JMP_rel8(6, end);
                        JMP_TARGET(doubleprec);
                        push_dr(R_EDX, FRm);
                        push_dr(R_EDX, FRn);
                        JMP_TARGET(end);
                        FCOMIP_st(1);
                        SETE_t();
                        FPOP_st();
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x5:
                        { /* FCMP/GT FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                        load_fr_bank( R_EDX );
                        JNE_rel8(8, doubleprec);
                        push_fr(R_EDX, FRm);
                        push_fr(R_EDX, FRn);
                        JMP_rel8(6, end);
                        JMP_TARGET(doubleprec);
                        push_dr(R_EDX, FRm);
                        push_dr(R_EDX, FRn);
                        JMP_TARGET(end);
                        FCOMIP_st(1);
                        SETA_t();
                        FPOP_st();
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x6:
                        { /* FMOV @(R0, Rm), FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_reg( R_EAX, Rm );
                        ADD_sh4r_r32( REG_OFFSET(r[0]), R_EAX );
                        check_ralign32( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        load_spreg( R_EDX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_SZ, R_EDX );
                        JNE_rel8(8 + MEM_READ_SIZE, doublesize);
                        MEM_READ_LONG( R_EAX, R_EAX );
                        load_fr_bank( R_EDX );
                        store_fr( R_EDX, R_EAX, FRn );
                        if( FRn&1 ) {
                    	JMP_rel8(21 + MEM_READ_DOUBLE_SIZE, end);
                    	JMP_TARGET(doublesize);
                    	MEM_READ_DOUBLE( R_EAX, R_ECX, R_EAX );
                    	load_spreg( R_EDX, R_FPSCR ); // assume read_long clobbered it
                    	load_xf_bank( R_EDX );
                    	store_fr( R_EDX, R_ECX, FRn&0x0E );
                    	store_fr( R_EDX, R_EAX, FRn|0x01 );
                    	JMP_TARGET(end);
                        } else {
                    	JMP_rel8(9 + MEM_READ_DOUBLE_SIZE, end);
                    	JMP_TARGET(doublesize);
                    	MEM_READ_DOUBLE( R_EAX, R_ECX, R_EAX );
                    	load_fr_bank( R_EDX );
                    	store_fr( R_EDX, R_ECX, FRn&0x0E );
                    	store_fr( R_EDX, R_EAX, FRn|0x01 );
                    	JMP_TARGET(end);
                        }
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x7:
                        { /* FMOV FRm, @(R0, Rn) */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_reg( R_EAX, Rn );
                        ADD_sh4r_r32( REG_OFFSET(r[0]), R_EAX );
                        check_walign32( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_spreg( R_EDX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_SZ, R_EDX );
                        JNE_rel8(8 + MEM_WRITE_SIZE, doublesize);
                        load_fr_bank( R_EDX );
                        load_fr( R_EDX, R_ECX, FRm );
                        MEM_WRITE_LONG( R_EAX, R_ECX ); // 12
                        if( FRm&1 ) {
                    	JMP_rel8( 18 + MEM_WRITE_DOUBLE_SIZE, end );
                    	JMP_TARGET(doublesize);
                    	load_xf_bank( R_EDX );
                    	load_fr( R_EDX, R_ECX, FRm&0x0E );
                    	load_fr( R_EDX, R_EDX, FRm|0x01 );
                    	MEM_WRITE_DOUBLE( R_EAX, R_ECX, R_EDX );
                    	JMP_TARGET(end);
                        } else {
                    	JMP_rel8( 9 + MEM_WRITE_DOUBLE_SIZE, end );
                    	JMP_TARGET(doublesize);
                    	load_fr_bank( R_EDX );
                    	load_fr( R_EDX, R_ECX, FRm&0x0E );
                    	load_fr( R_EDX, R_EDX, FRm|0x01 );
                    	MEM_WRITE_DOUBLE( R_EAX, R_ECX, R_EDX );
                    	JMP_TARGET(end);
                        }
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x8:
                        { /* FMOV @Rm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_reg( R_EAX, Rm );
                        check_ralign32( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        load_spreg( R_EDX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_SZ, R_EDX );
                        JNE_rel8(8 + MEM_READ_SIZE, doublesize);
                        MEM_READ_LONG( R_EAX, R_EAX );
                        load_fr_bank( R_EDX );
                        store_fr( R_EDX, R_EAX, FRn );
                        if( FRn&1 ) {
                    	JMP_rel8(21 + MEM_READ_DOUBLE_SIZE, end);
                    	JMP_TARGET(doublesize);
                    	MEM_READ_DOUBLE( R_EAX, R_ECX, R_EAX );
                    	load_spreg( R_EDX, R_FPSCR ); // assume read_long clobbered it
                    	load_xf_bank( R_EDX );
                    	store_fr( R_EDX, R_ECX, FRn&0x0E );
                    	store_fr( R_EDX, R_EAX, FRn|0x01 );
                    	JMP_TARGET(end);
                        } else {
                    	JMP_rel8(9 + MEM_READ_DOUBLE_SIZE, end);
                    	JMP_TARGET(doublesize);
                    	MEM_READ_DOUBLE( R_EAX, R_ECX, R_EAX );
                    	load_fr_bank( R_EDX );
                    	store_fr( R_EDX, R_ECX, FRn&0x0E );
                    	store_fr( R_EDX, R_EAX, FRn|0x01 );
                    	JMP_TARGET(end);
                        }
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0x9:
                        { /* FMOV @Rm+, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_reg( R_EAX, Rm );
                        check_ralign32( R_EAX );
                        MMU_TRANSLATE_READ( R_EAX );
                        load_spreg( R_EDX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_SZ, R_EDX );
                        JNE_rel8(12 + MEM_READ_SIZE, doublesize);
                        ADD_imm8s_sh4r( 4, REG_OFFSET(r[Rm]) );
                        MEM_READ_LONG( R_EAX, R_EAX );
                        load_fr_bank( R_EDX );
                        store_fr( R_EDX, R_EAX, FRn );
                        if( FRn&1 ) {
                    	JMP_rel8(25 + MEM_READ_DOUBLE_SIZE, end);
                    	JMP_TARGET(doublesize);
                    	ADD_imm8s_sh4r( 8, REG_OFFSET(r[Rm]) );
                    	MEM_READ_DOUBLE( R_EAX, R_ECX, R_EAX );
                    	load_spreg( R_EDX, R_FPSCR ); // assume read_long clobbered it
                    	load_xf_bank( R_EDX );
                    	store_fr( R_EDX, R_ECX, FRn&0x0E );
                    	store_fr( R_EDX, R_EAX, FRn|0x01 );
                    	JMP_TARGET(end);
                        } else {
                    	JMP_rel8(13 + MEM_READ_DOUBLE_SIZE, end);
                    	ADD_imm8s_sh4r( 8, REG_OFFSET(r[Rm]) );
                    	MEM_READ_DOUBLE( R_EAX, R_ECX, R_EAX );
                    	load_fr_bank( R_EDX );
                    	store_fr( R_EDX, R_ECX, FRn&0x0E );
                    	store_fr( R_EDX, R_EAX, FRn|0x01 );
                    	JMP_TARGET(end);
                        }
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xA:
                        { /* FMOV FRm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_reg( R_EAX, Rn );
                        check_walign32( R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_spreg( R_EDX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_SZ, R_EDX );
                        JNE_rel8(8 + MEM_WRITE_SIZE, doublesize);
                        load_fr_bank( R_EDX );
                        load_fr( R_EDX, R_ECX, FRm );
                        MEM_WRITE_LONG( R_EAX, R_ECX ); // 12
                        if( FRm&1 ) {
                    	JMP_rel8( 18 + MEM_WRITE_DOUBLE_SIZE, end );
                    	JMP_TARGET(doublesize);
                    	load_xf_bank( R_EDX );
                    	load_fr( R_EDX, R_ECX, FRm&0x0E );
                    	load_fr( R_EDX, R_EDX, FRm|0x01 );
                    	MEM_WRITE_DOUBLE( R_EAX, R_ECX, R_EDX );
                    	JMP_TARGET(end);
                        } else {
                    	JMP_rel8( 9 + MEM_WRITE_DOUBLE_SIZE, end );
                    	JMP_TARGET(doublesize);
                    	load_fr_bank( R_EDX );
                    	load_fr( R_EDX, R_ECX, FRm&0x0E );
                    	load_fr( R_EDX, R_EDX, FRm|0x01 );
                    	MEM_WRITE_DOUBLE( R_EAX, R_ECX, R_EDX );
                    	JMP_TARGET(end);
                        }
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xB:
                        { /* FMOV FRm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_reg( R_EAX, Rn );
                        check_walign32( R_EAX );
                        load_spreg( R_EDX, R_FPSCR );
                        TEST_imm32_r32( FPSCR_SZ, R_EDX );
                        JNE_rel8(15 + MEM_WRITE_SIZE + MMU_TRANSLATE_SIZE, doublesize);
                        ADD_imm8s_r32( -4, R_EAX );
                        MMU_TRANSLATE_WRITE( R_EAX );
                        load_fr_bank( R_EDX );
                        load_fr( R_EDX, R_ECX, FRm );
                        ADD_imm8s_sh4r(-4,REG_OFFSET(r[Rn]));
                        MEM_WRITE_LONG( R_EAX, R_ECX ); // 12
                        if( FRm&1 ) {
                    	JMP_rel8( 25 + MEM_WRITE_DOUBLE_SIZE + MMU_TRANSLATE_SIZE, end );
                    	JMP_TARGET(doublesize);
                    	ADD_imm8s_r32(-8,R_EAX);
                    	MMU_TRANSLATE_WRITE( R_EAX );
                    	load_xf_bank( R_EDX );
                    	load_fr( R_EDX, R_ECX, FRm&0x0E );
                    	load_fr( R_EDX, R_EDX, FRm|0x01 );
                    	ADD_imm8s_sh4r(-8,REG_OFFSET(r[Rn]));
                    	MEM_WRITE_DOUBLE( R_EAX, R_ECX, R_EDX );
                    	JMP_TARGET(end);
                        } else {
                    	JMP_rel8( 16 + MEM_WRITE_DOUBLE_SIZE + MMU_TRANSLATE_SIZE, end );
                    	JMP_TARGET(doublesize);
                    	ADD_imm8s_r32(-8,R_EAX);
                    	MMU_TRANSLATE_WRITE( R_EAX );
                    	load_fr_bank( R_EDX );
                    	load_fr( R_EDX, R_ECX, FRm&0x0E );
                    	load_fr( R_EDX, R_EDX, FRm|0x01 );
                    	ADD_imm8s_sh4r(-8,REG_OFFSET(r[Rn]));
                    	MEM_WRITE_DOUBLE( R_EAX, R_ECX, R_EDX );
                    	JMP_TARGET(end);
                        }
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xC:
                        { /* FMOV FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        /* As horrible as this looks, it's actually covering 5 separate cases:
                         * 1. 32-bit fr-to-fr (PR=0)
                         * 2. 64-bit dr-to-dr (PR=1, FRm&1 == 0, FRn&1 == 0 )
                         * 3. 64-bit dr-to-xd (PR=1, FRm&1 == 0, FRn&1 == 1 )
                         * 4. 64-bit xd-to-dr (PR=1, FRm&1 == 1, FRn&1 == 0 )
                         * 5. 64-bit xd-to-xd (PR=1, FRm&1 == 1, FRn&1 == 1 )
                         */
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        load_fr_bank( R_EDX );
                        TEST_imm32_r32( FPSCR_SZ, R_ECX );
                        JNE_rel8(8, doublesize);
                        load_fr( R_EDX, R_EAX, FRm ); // PR=0 branch
                        store_fr( R_EDX, R_EAX, FRn );
                        if( FRm&1 ) {
                    	JMP_rel8(24, end);
                    	JMP_TARGET(doublesize);
                    	load_xf_bank( R_ECX ); 
                    	load_fr( R_ECX, R_EAX, FRm-1 );
                    	if( FRn&1 ) {
                    	    load_fr( R_ECX, R_EDX, FRm );
                    	    store_fr( R_ECX, R_EAX, FRn-1 );
                    	    store_fr( R_ECX, R_EDX, FRn );
                    	} else /* FRn&1 == 0 */ {
                    	    load_fr( R_ECX, R_ECX, FRm );
                    	    store_fr( R_EDX, R_EAX, FRn );
                    	    store_fr( R_EDX, R_ECX, FRn+1 );
                    	}
                    	JMP_TARGET(end);
                        } else /* FRm&1 == 0 */ {
                    	if( FRn&1 ) {
                    	    JMP_rel8(24, end);
                    	    load_xf_bank( R_ECX );
                    	    load_fr( R_EDX, R_EAX, FRm );
                    	    load_fr( R_EDX, R_EDX, FRm+1 );
                    	    store_fr( R_ECX, R_EAX, FRn-1 );
                    	    store_fr( R_ECX, R_EDX, FRn );
                    	    JMP_TARGET(end);
                    	} else /* FRn&1 == 0 */ {
                    	    JMP_rel8(12, end);
                    	    load_fr( R_EDX, R_EAX, FRm );
                    	    load_fr( R_EDX, R_ECX, FRm+1 );
                    	    store_fr( R_EDX, R_EAX, FRn );
                    	    store_fr( R_EDX, R_ECX, FRn+1 );
                    	    JMP_TARGET(end);
                    	}
                        }
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    case 0xD:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* FSTS FPUL, FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_fr_bank( R_ECX );
                                load_spreg( R_EAX, R_FPUL );
                                store_fr( R_ECX, R_EAX, FRn );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x1:
                                { /* FLDS FRm, FPUL */
                                uint32_t FRm = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_fr_bank( R_ECX );
                                load_fr( R_ECX, R_EAX, FRm );
                                store_spreg( R_EAX, R_FPUL );
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x2:
                                { /* FLOAT FPUL, FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                load_spreg(R_EDX, REG_OFFSET(fr_bank));
                                FILD_sh4r(R_FPUL);
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                JNE_rel8(5, doubleprec);
                                pop_fr( R_EDX, FRn );
                                JMP_rel8(3, end);
                                JMP_TARGET(doubleprec);
                                pop_dr( R_EDX, FRn );
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x3:
                                { /* FTRC FRm, FPUL */
                                uint32_t FRm = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                load_fr_bank( R_EDX );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                JNE_rel8(5, doubleprec);
                                push_fr( R_EDX, FRm );
                                JMP_rel8(3, doop);
                                JMP_TARGET(doubleprec);
                                push_dr( R_EDX, FRm );
                                JMP_TARGET( doop );
                                load_imm32( R_ECX, (uint32_t)&max_int );
                                FILD_r32ind( R_ECX );
                                FCOMIP_st(1);
                                JNA_rel8( 32, sat );
                                load_imm32( R_ECX, (uint32_t)&min_int );  // 5
                                FILD_r32ind( R_ECX );           // 2
                                FCOMIP_st(1);                   // 2
                                JAE_rel8( 21, sat2 );            // 2
                                load_imm32( R_EAX, (uint32_t)&save_fcw );
                                FNSTCW_r32ind( R_EAX );
                                load_imm32( R_EDX, (uint32_t)&trunc_fcw );
                                FLDCW_r32ind( R_EDX );
                                FISTP_sh4r(R_FPUL);             // 3
                                FLDCW_r32ind( R_EAX );
                                JMP_rel8( 9, end );             // 2
                            
                                JMP_TARGET(sat);
                                JMP_TARGET(sat2);
                                MOV_r32ind_r32( R_ECX, R_ECX ); // 2
                                store_spreg( R_ECX, R_FPUL );
                                FPOP_st();
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x4:
                                { /* FNEG FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                load_fr_bank( R_EDX );
                                JNE_rel8(10, doubleprec);
                                push_fr(R_EDX, FRn);
                                FCHS_st0();
                                pop_fr(R_EDX, FRn);
                                JMP_rel8(8, end);
                                JMP_TARGET(doubleprec);
                                push_dr(R_EDX, FRn);
                                FCHS_st0();
                                pop_dr(R_EDX, FRn);
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x5:
                                { /* FABS FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                load_fr_bank( R_EDX );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                JNE_rel8(10, doubleprec);
                                push_fr(R_EDX, FRn); // 3
                                FABS_st0(); // 2
                                pop_fr( R_EDX, FRn); //3
                                JMP_rel8(8,end); // 2
                                JMP_TARGET(doubleprec);
                                push_dr(R_EDX, FRn);
                                FABS_st0();
                                pop_dr(R_EDX, FRn);
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x6:
                                { /* FSQRT FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                load_fr_bank( R_EDX );
                                JNE_rel8(10, doubleprec);
                                push_fr(R_EDX, FRn);
                                FSQRT_st0();
                                pop_fr(R_EDX, FRn);
                                JMP_rel8(8, end);
                                JMP_TARGET(doubleprec);
                                push_dr(R_EDX, FRn);
                                FSQRT_st0();
                                pop_dr(R_EDX, FRn);
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x7:
                                { /* FSRRA FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                load_fr_bank( R_EDX );
                                JNE_rel8(12, end); // PR=0 only
                                FLD1_st0();
                                push_fr(R_EDX, FRn);
                                FSQRT_st0();
                                FDIVP_st(1);
                                pop_fr(R_EDX, FRn);
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x8:
                                { /* FLDI0 FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                /* IFF PR=0 */
                                  check_fpuen();
                                  load_spreg( R_ECX, R_FPSCR );
                                  TEST_imm32_r32( FPSCR_PR, R_ECX );
                                  JNE_rel8(8, end);
                                  XOR_r32_r32( R_EAX, R_EAX );
                                  load_spreg( R_ECX, REG_OFFSET(fr_bank) );
                                  store_fr( R_ECX, R_EAX, FRn );
                                  JMP_TARGET(end);
                                  sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0x9:
                                { /* FLDI1 FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                /* IFF PR=0 */
                                  check_fpuen();
                                  load_spreg( R_ECX, R_FPSCR );
                                  TEST_imm32_r32( FPSCR_PR, R_ECX );
                                  JNE_rel8(11, end);
                                  load_imm32(R_EAX, 0x3F800000);
                                  load_spreg( R_ECX, REG_OFFSET(fr_bank) );
                                  store_fr( R_ECX, R_EAX, FRn );
                                  JMP_TARGET(end);
                                  sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0xA:
                                { /* FCNVSD FPUL, FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                JE_rel8(9, end); // only when PR=1
                                load_fr_bank( R_ECX );
                                push_fpul();
                                pop_dr( R_ECX, FRn );
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0xB:
                                { /* FCNVDS FRm, FPUL */
                                uint32_t FRm = ((ir>>8)&0xF); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                JE_rel8(9, end); // only when PR=1
                                load_fr_bank( R_ECX );
                                push_dr( R_ECX, FRm );
                                pop_fpul();
                                JMP_TARGET(end);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0xE:
                                { /* FIPR FVm, FVn */
                                uint32_t FVn = ((ir>>10)&0x3); uint32_t FVm = ((ir>>8)&0x3); 
                                check_fpuen();
                                load_spreg( R_ECX, R_FPSCR );
                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                JNE_rel8(44, doubleprec);
                                
                                load_fr_bank( R_ECX );
                                push_fr( R_ECX, FVm<<2 );
                                push_fr( R_ECX, FVn<<2 );
                                FMULP_st(1);
                                push_fr( R_ECX, (FVm<<2)+1);
                                push_fr( R_ECX, (FVn<<2)+1);
                                FMULP_st(1);
                                FADDP_st(1);
                                push_fr( R_ECX, (FVm<<2)+2);
                                push_fr( R_ECX, (FVn<<2)+2);
                                FMULP_st(1);
                                FADDP_st(1);
                                push_fr( R_ECX, (FVm<<2)+3);
                                push_fr( R_ECX, (FVn<<2)+3);
                                FMULP_st(1);
                                FADDP_st(1);
                                pop_fr( R_ECX, (FVn<<2)+3);
                                JMP_TARGET(doubleprec);
                                sh4_x86.tstate = TSTATE_NONE;
                                }
                                break;
                            case 0xF:
                                switch( (ir&0x100) >> 8 ) {
                                    case 0x0:
                                        { /* FSCA FPUL, FRn */
                                        uint32_t FRn = ((ir>>9)&0x7)<<1; 
                                        check_fpuen();
                                        load_spreg( R_ECX, R_FPSCR );
                                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                                        JNE_rel8( CALL_FUNC2_SIZE + 9, doubleprec );
                                        load_fr_bank( R_ECX );
                                        ADD_imm8s_r32( (FRn&0x0E)<<2, R_ECX );
                                        load_spreg( R_EDX, R_FPUL );
                                        call_func2( sh4_fsca, R_EDX, R_ECX );
                                        JMP_TARGET(doubleprec);
                                        sh4_x86.tstate = TSTATE_NONE;
                                        }
                                        break;
                                    case 0x1:
                                        switch( (ir&0x200) >> 9 ) {
                                            case 0x0:
                                                { /* FTRV XMTRX, FVn */
                                                uint32_t FVn = ((ir>>10)&0x3); 
                                                check_fpuen();
                                                load_spreg( R_ECX, R_FPSCR );
                                                TEST_imm32_r32( FPSCR_PR, R_ECX );
                                                JNE_rel8( 18 + CALL_FUNC2_SIZE, doubleprec );
                                                load_fr_bank( R_EDX );                 // 3
                                                ADD_imm8s_r32( FVn<<4, R_EDX );        // 3
                                                load_xf_bank( R_ECX );                 // 12
                                                call_func2( sh4_ftrv, R_EDX, R_ECX );  // 12
                                                JMP_TARGET(doubleprec);
                                                sh4_x86.tstate = TSTATE_NONE;
                                                }
                                                break;
                                            case 0x1:
                                                switch( (ir&0xC00) >> 10 ) {
                                                    case 0x0:
                                                        { /* FSCHG */
                                                        check_fpuen();
                                                        load_spreg( R_ECX, R_FPSCR );
                                                        XOR_imm32_r32( FPSCR_SZ, R_ECX );
                                                        store_spreg( R_ECX, R_FPSCR );
                                                        sh4_x86.tstate = TSTATE_NONE;
                                                        }
                                                        break;
                                                    case 0x2:
                                                        { /* FRCHG */
                                                        check_fpuen();
                                                        load_spreg( R_ECX, R_FPSCR );
                                                        XOR_imm32_r32( FPSCR_FR, R_ECX );
                                                        store_spreg( R_ECX, R_FPSCR );
                                                        update_fr_bank( R_ECX );
                                                        sh4_x86.tstate = TSTATE_NONE;
                                                        }
                                                        break;
                                                    case 0x3:
                                                        { /* UNDEF */
                                                        if( sh4_x86.in_delay_slot ) {
                                                    	SLOTILLEGAL();
                                                        } else {
                                                    	JMP_exc(EXC_ILLEGAL);
                                                    	return 2;
                                                        }
                                                        }
                                                        break;
                                                    default:
                                                        UNDEF();
                                                        break;
                                                }
                                                break;
                                        }
                                        break;
                                }
                                break;
                            default:
                                UNDEF();
                                break;
                        }
                        break;
                    case 0xE:
                        { /* FMAC FR0, FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        check_fpuen();
                        load_spreg( R_ECX, R_FPSCR );
                        load_spreg( R_EDX, REG_OFFSET(fr_bank));
                        TEST_imm32_r32( FPSCR_PR, R_ECX );
                        JNE_rel8(18, doubleprec);
                        push_fr( R_EDX, 0 );
                        push_fr( R_EDX, FRm );
                        FMULP_st(1);
                        push_fr( R_EDX, FRn );
                        FADDP_st(1);
                        pop_fr( R_EDX, FRn );
                        JMP_rel8(16, end);
                        JMP_TARGET(doubleprec);
                        push_dr( R_EDX, 0 );
                        push_dr( R_EDX, FRm );
                        FMULP_st(1);
                        push_dr( R_EDX, FRn );
                        FADDP_st(1);
                        pop_dr( R_EDX, FRn );
                        JMP_TARGET(end);
                        sh4_x86.tstate = TSTATE_NONE;
                        }
                        break;
                    default:
                        UNDEF();
                        break;
                }
                break;
        }

    sh4_x86.in_delay_slot = DELAY_NONE;
    return 0;
}
