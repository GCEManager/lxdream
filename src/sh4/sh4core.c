/**
 * $Id: sh4core.c,v 1.41 2007-08-23 12:33:27 nkeynes Exp $
 * 
 * SH4 emulation core, and parent module for all the SH4 peripheral
 * modules.
 *
 * Copyright (c) 2005 Nathan Keynes.
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

#define MODULE sh4_module
#include <math.h>
#include "dream.h"
#include "sh4/sh4core.h"
#include "sh4/sh4mmio.h"
#include "sh4/intc.h"
#include "mem.h"
#include "clock.h"
#include "syscall.h"

#define SH4_CALLTRACE 1

#define MAX_INT 0x7FFFFFFF
#define MIN_INT 0x80000000
#define MAX_INTF 2147483647.0
#define MIN_INTF -2147483648.0

/* CPU-generated exception code/vector pairs */
#define EXC_POWER_RESET  0x000 /* vector special */
#define EXC_MANUAL_RESET 0x020
#define EXC_READ_ADDR_ERR 0x0E0
#define EXC_WRITE_ADDR_ERR 0x100
#define EXC_SLOT_ILLEGAL 0x1A0
#define EXC_ILLEGAL      0x180
#define EXC_TRAP         0x160
#define EXC_FPDISABLE    0x800
#define EXC_SLOT_FPDISABLE 0x820

#define EXV_EXCEPTION    0x100  /* General exception vector */
#define EXV_TLBMISS      0x400  /* TLB-miss exception vector */
#define EXV_INTERRUPT    0x600  /* External interrupt vector */

/********************** SH4 Module Definition ****************************/

void sh4_init( void );
void sh4_reset( void );
uint32_t sh4_run_slice( uint32_t );
void sh4_start( void );
void sh4_stop( void );
void sh4_save_state( FILE *f );
int sh4_load_state( FILE *f );
void sh4_accept_interrupt( void );

struct dreamcast_module sh4_module = { "SH4", sh4_init, sh4_reset, 
				       NULL, sh4_run_slice, sh4_stop,
				       sh4_save_state, sh4_load_state };

struct sh4_registers sh4r;

void sh4_init(void)
{
    register_io_regions( mmio_list_sh4mmio );
    MMU_init();
    sh4_reset();
}

void sh4_reset(void)
{
    /* zero everything out, for the sake of having a consistent state. */
    memset( &sh4r, 0, sizeof(sh4r) );

    /* Resume running if we were halted */
    sh4r.sh4_state = SH4_STATE_RUNNING;

    sh4r.pc    = 0xA0000000;
    sh4r.new_pc= 0xA0000002;
    sh4r.vbr   = 0x00000000;
    sh4r.fpscr = 0x00040001;
    sh4r.sr    = 0x700000F0;

    /* Mem reset will do this, but if we want to reset _just_ the SH4... */
    MMIO_WRITE( MMU, EXPEVT, EXC_POWER_RESET );

    /* Peripheral modules */
    CPG_reset();
    INTC_reset();
    MMU_reset();
    TMU_reset();
    SCIF_reset();
}

static struct breakpoint_struct sh4_breakpoints[MAX_BREAKPOINTS];
static int sh4_breakpoint_count = 0;
static uint16_t *sh4_icache = NULL;
static uint32_t sh4_icache_addr = 0;

void sh4_set_breakpoint( uint32_t pc, int type )
{
    sh4_breakpoints[sh4_breakpoint_count].address = pc;
    sh4_breakpoints[sh4_breakpoint_count].type = type;
    sh4_breakpoint_count++;
}

gboolean sh4_clear_breakpoint( uint32_t pc, int type )
{
    int i;

    for( i=0; i<sh4_breakpoint_count; i++ ) {
	if( sh4_breakpoints[i].address == pc && 
	    sh4_breakpoints[i].type == type ) {
	    while( ++i < sh4_breakpoint_count ) {
		sh4_breakpoints[i-1].address = sh4_breakpoints[i].address;
		sh4_breakpoints[i-1].type = sh4_breakpoints[i].type;
	    }
	    sh4_breakpoint_count--;
	    return TRUE;
	}
    }
    return FALSE;
}

int sh4_get_breakpoint( uint32_t pc )
{
    int i;
    for( i=0; i<sh4_breakpoint_count; i++ ) {
	if( sh4_breakpoints[i].address == pc )
	    return sh4_breakpoints[i].type;
    }
    return 0;
}

uint32_t sh4_run_slice( uint32_t nanosecs ) 
{
    int i;
    sh4r.slice_cycle = 0;

    if( sh4r.sh4_state != SH4_STATE_RUNNING ) {
	if( sh4r.event_pending < nanosecs ) {
	    sh4r.sh4_state = SH4_STATE_RUNNING;
	    sh4r.slice_cycle = sh4r.event_pending;
	}
    }

    if( sh4_breakpoint_count == 0 ) {
	for( ; sh4r.slice_cycle < nanosecs; sh4r.slice_cycle += sh4_cpu_period ) {
	    if( SH4_EVENT_PENDING() ) {
		if( sh4r.event_types & PENDING_EVENT ) {
		    event_execute();
		}
		/* Eventq execute may (quite likely) deliver an immediate IRQ */
		if( sh4r.event_types & PENDING_IRQ ) {
		    sh4_accept_interrupt();
		}
	    }
	    if( !sh4_execute_instruction() ) {
		break;
	    }
	}
    } else {
	for( ;sh4r.slice_cycle < nanosecs; sh4r.slice_cycle += sh4_cpu_period ) {
	    if( SH4_EVENT_PENDING() ) {
		if( sh4r.event_types & PENDING_EVENT ) {
		    event_execute();
		}
		/* Eventq execute may (quite likely) deliver an immediate IRQ */
		if( sh4r.event_types & PENDING_IRQ ) {
		    sh4_accept_interrupt();
		}
	    }
                 
	    if( !sh4_execute_instruction() )
		break;
#ifdef ENABLE_DEBUG_MODE
	    for( i=0; i<sh4_breakpoint_count; i++ ) {
		if( sh4_breakpoints[i].address == sh4r.pc ) {
		    break;
		}
	    }
	    if( i != sh4_breakpoint_count ) {
		dreamcast_stop();
		if( sh4_breakpoints[i].type == BREAK_ONESHOT )
		    sh4_clear_breakpoint( sh4r.pc, BREAK_ONESHOT );
		break;
	    }
#endif	
	}
    }

    /* If we aborted early, but the cpu is still technically running,
     * we're doing a hard abort - cut the timeslice back to what we
     * actually executed
     */
    if( sh4r.slice_cycle != nanosecs && sh4r.sh4_state == SH4_STATE_RUNNING ) {
	nanosecs = sh4r.slice_cycle;
    }
    if( sh4r.sh4_state != SH4_STATE_STANDBY ) {
	TMU_run_slice( nanosecs );
	SCIF_run_slice( nanosecs );
    }
    return nanosecs;
}

void sh4_stop(void)
{

}

void sh4_save_state( FILE *f )
{
    fwrite( &sh4r, sizeof(sh4r), 1, f );
    MMU_save_state( f );
    INTC_save_state( f );
    TMU_save_state( f );
    SCIF_save_state( f );
}

int sh4_load_state( FILE * f )
{
    fread( &sh4r, sizeof(sh4r), 1, f );
    MMU_load_state( f );
    INTC_load_state( f );
    TMU_load_state( f );
    return SCIF_load_state( f );
}

/********************** SH4 emulation core  ****************************/

void sh4_set_pc( int pc )
{
    sh4r.pc = pc;
    sh4r.new_pc = pc+2;
}

#define UNDEF(ir) return sh4_raise_slot_exception(EXC_ILLEGAL, EXC_SLOT_ILLEGAL)
#define UNIMP(ir) do{ ERROR( "Halted on unimplemented instruction at %08x, opcode = %04x", sh4r.pc, ir ); dreamcast_stop(); return FALSE; }while(0)

#if(SH4_CALLTRACE == 1)
#define MAX_CALLSTACK 32
static struct call_stack {
    sh4addr_t call_addr;
    sh4addr_t target_addr;
    sh4addr_t stack_pointer;
} call_stack[MAX_CALLSTACK];

static int call_stack_depth = 0;
int sh4_call_trace_on = 0;

static inline trace_call( sh4addr_t source, sh4addr_t dest ) 
{
    if( call_stack_depth < MAX_CALLSTACK ) {
	call_stack[call_stack_depth].call_addr = source;
	call_stack[call_stack_depth].target_addr = dest;
	call_stack[call_stack_depth].stack_pointer = sh4r.r[15];
    }
    call_stack_depth++;
}

static inline trace_return( sh4addr_t source, sh4addr_t dest )
{
    if( call_stack_depth > 0 ) {
	call_stack_depth--;
    }
}

void fprint_stack_trace( FILE *f )
{
    int i = call_stack_depth -1;
    if( i >= MAX_CALLSTACK )
	i = MAX_CALLSTACK - 1;
    for( ; i >= 0; i-- ) {
	fprintf( f, "%d. Call from %08X => %08X, SP=%08X\n", 
		 (call_stack_depth - i), call_stack[i].call_addr,
		 call_stack[i].target_addr, call_stack[i].stack_pointer );
    }
}

#define TRACE_CALL( source, dest ) trace_call(source, dest)
#define TRACE_RETURN( source, dest ) trace_return(source, dest)
#else
#define TRACE_CALL( dest, rts ) 
#define TRACE_RETURN( source, dest )
#endif

#define RAISE( x, v ) do{			\
    if( sh4r.vbr == 0 ) { \
        ERROR( "%08X: VBR not initialized while raising exception %03X, halting", sh4r.pc, x ); \
        dreamcast_stop(); return FALSE;	\
    } else { \
        sh4r.spc = sh4r.pc;	\
        sh4r.ssr = sh4_read_sr(); \
        sh4r.sgr = sh4r.r[15]; \
        MMIO_WRITE(MMU,EXPEVT,x); \
        sh4r.pc = sh4r.vbr + v; \
        sh4r.new_pc = sh4r.pc + 2; \
        sh4_load_sr( sh4r.ssr |SR_MD|SR_BL|SR_RB ); \
	if( sh4r.in_delay_slot ) { \
	    sh4r.in_delay_slot = 0; \
	    sh4r.spc -= 2; \
	} \
    } \
    return TRUE; } while(0)

#define MEM_READ_BYTE( addr ) sh4_read_byte(addr)
#define MEM_READ_WORD( addr ) sh4_read_word(addr)
#define MEM_READ_LONG( addr ) sh4_read_long(addr)
#define MEM_WRITE_BYTE( addr, val ) sh4_write_byte(addr, val)
#define MEM_WRITE_WORD( addr, val ) sh4_write_word(addr, val)
#define MEM_WRITE_LONG( addr, val ) sh4_write_long(addr, val)

#define FP_WIDTH (IS_FPU_DOUBLESIZE() ? 8 : 4)

#define MEM_FP_READ( addr, reg ) sh4_read_float( addr, reg );
#define MEM_FP_WRITE( addr, reg ) sh4_write_float( addr, reg );

#define CHECKPRIV() if( !IS_SH4_PRIVMODE() ) return sh4_raise_slot_exception( EXC_ILLEGAL, EXC_SLOT_ILLEGAL )
#define CHECKRALIGN16(addr) if( (addr)&0x01 ) return sh4_raise_exception( EXC_READ_ADDR_ERR )
#define CHECKRALIGN32(addr) if( (addr)&0x03 ) return sh4_raise_exception( EXC_READ_ADDR_ERR )
#define CHECKWALIGN16(addr) if( (addr)&0x01 ) return sh4_raise_exception( EXC_WRITE_ADDR_ERR )
#define CHECKWALIGN32(addr) if( (addr)&0x03 ) return sh4_raise_exception( EXC_WRITE_ADDR_ERR )

#define CHECKFPUEN() if( !IS_FPU_ENABLED() ) { if( ir == 0xFFFD ) { UNDEF(ir); } else { return sh4_raise_slot_exception( EXC_FPDISABLE, EXC_SLOT_FPDISABLE ); } }
#define CHECKDEST(p) if( (p) == 0 ) { ERROR( "%08X: Branch/jump to NULL, CPU halted", sh4r.pc ); dreamcast_stop(); return FALSE; }
#define CHECKSLOTILLEGAL() if(sh4r.in_delay_slot) return sh4_raise_exception(EXC_SLOT_ILLEGAL)

static void sh4_switch_banks( )
{
    uint32_t tmp[8];

    memcpy( tmp, sh4r.r, sizeof(uint32_t)*8 );
    memcpy( sh4r.r, sh4r.r_bank, sizeof(uint32_t)*8 );
    memcpy( sh4r.r_bank, tmp, sizeof(uint32_t)*8 );
}

static void sh4_load_sr( uint32_t newval )
{
    if( (newval ^ sh4r.sr) & SR_RB )
        sh4_switch_banks();
    sh4r.sr = newval;
    sh4r.t = (newval&SR_T) ? 1 : 0;
    sh4r.s = (newval&SR_S) ? 1 : 0;
    sh4r.m = (newval&SR_M) ? 1 : 0;
    sh4r.q = (newval&SR_Q) ? 1 : 0;
    intc_mask_changed();
}

static void sh4_write_float( uint32_t addr, int reg )
{
    if( IS_FPU_DOUBLESIZE() ) {
	if( reg & 1 ) {
	    sh4_write_long( addr, *((uint32_t *)&XF((reg)&0x0E)) );
	    sh4_write_long( addr+4, *((uint32_t *)&XF(reg)) );
	} else {
	    sh4_write_long( addr, *((uint32_t *)&FR(reg)) ); 
	    sh4_write_long( addr+4, *((uint32_t *)&FR((reg)|0x01)) );
	}
    } else {
	sh4_write_long( addr, *((uint32_t *)&FR((reg))) );
    }
}

static void sh4_read_float( uint32_t addr, int reg )
{
    if( IS_FPU_DOUBLESIZE() ) {
	if( reg & 1 ) {
	    *((uint32_t *)&XF((reg) & 0x0E)) = sh4_read_long(addr);
	    *((uint32_t *)&XF(reg)) = sh4_read_long(addr+4);
	} else {
	    *((uint32_t *)&FR(reg)) = sh4_read_long(addr);
	    *((uint32_t *)&FR((reg) | 0x01)) = sh4_read_long(addr+4);
	}
    } else {
	*((uint32_t *)&FR(reg)) = sh4_read_long(addr);
    }
}

static uint32_t sh4_read_sr( void )
{
    /* synchronize sh4r.sr with the various bitflags */
    sh4r.sr &= SR_MQSTMASK;
    if( sh4r.t ) sh4r.sr |= SR_T;
    if( sh4r.s ) sh4r.sr |= SR_S;
    if( sh4r.m ) sh4r.sr |= SR_M;
    if( sh4r.q ) sh4r.sr |= SR_Q;
    return sh4r.sr;
}

/**
 * Raise a general CPU exception for the specified exception code.
 * (NOT for TRAPA or TLB exceptions)
 */
gboolean sh4_raise_exception( int code )
{
    RAISE( code, EXV_EXCEPTION );
}

gboolean sh4_raise_slot_exception( int normal_code, int slot_code ) {
    if( sh4r.in_delay_slot ) {
	return sh4_raise_exception(slot_code);
    } else {
	return sh4_raise_exception(normal_code);
    }
}

gboolean sh4_raise_tlb_exception( int code )
{
    RAISE( code, EXV_TLBMISS );
}

void sh4_accept_interrupt( void )
{
    uint32_t code = intc_accept_interrupt();
    sh4r.ssr = sh4_read_sr();
    sh4r.spc = sh4r.pc;
    sh4r.sgr = sh4r.r[15];
    sh4_load_sr( sh4r.ssr|SR_BL|SR_MD|SR_RB );
    MMIO_WRITE( MMU, INTEVT, code );
    sh4r.pc = sh4r.vbr + 0x600;
    sh4r.new_pc = sh4r.pc + 2;
    //    WARN( "Accepting interrupt %03X, from %08X => %08X", code, sh4r.spc, sh4r.pc );
}

gboolean sh4_execute_instruction( void )
{
    uint32_t pc;
    unsigned short ir;
    uint32_t tmp;
    float ftmp;
    double dtmp;
    
#define R0 sh4r.r[0]
    pc = sh4r.pc;
    if( pc > 0xFFFFFF00 ) {
	/* SYSCALL Magic */
	syscall_invoke( pc );
	sh4r.in_delay_slot = 0;
	pc = sh4r.pc = sh4r.pr;
	sh4r.new_pc = sh4r.pc + 2;
    }
    CHECKRALIGN16(pc);

    /* Read instruction */
    uint32_t pageaddr = pc >> 12;
    if( sh4_icache != NULL && pageaddr == sh4_icache_addr ) {
	ir = sh4_icache[(pc&0xFFF)>>1];
    } else {
	sh4_icache = (uint16_t *)mem_get_page(pc);
	if( ((uint32_t)sh4_icache) < MAX_IO_REGIONS ) {
	    /* If someone's actually been so daft as to try to execute out of an IO
	     * region, fallback on the full-blown memory read
	     */
	    sh4_icache = NULL;
	    ir = MEM_READ_WORD(pc);
	} else {
	    sh4_icache_addr = pageaddr;
	    ir = sh4_icache[(pc&0xFFF)>>1];
	}
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
                                        CHECKPRIV();
                                        sh4r.r[Rn] = sh4_read_sr();
                                        }
                                        break;
                                    case 0x1:
                                        { /* STC GBR, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.r[Rn] = sh4r.gbr;
                                        }
                                        break;
                                    case 0x2:
                                        { /* STC VBR, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.r[Rn] = sh4r.vbr;
                                        }
                                        break;
                                    case 0x3:
                                        { /* STC SSR, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.r[Rn] = sh4r.ssr;
                                        }
                                        break;
                                    case 0x4:
                                        { /* STC SPC, Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.r[Rn] = sh4r.spc;
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
                                CHECKPRIV();
                                sh4r.r[Rn] = sh4r.r_bank[Rm_BANK];
                                }
                                break;
                        }
                        break;
                    case 0x3:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* BSRF Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                CHECKSLOTILLEGAL();
                                CHECKDEST( pc + 4 + sh4r.r[Rn] );
                                sh4r.in_delay_slot = 1;
                                sh4r.pr = sh4r.pc + 4;
                                sh4r.pc = sh4r.new_pc;
                                sh4r.new_pc = pc + 4 + sh4r.r[Rn];
                                TRACE_CALL( pc, sh4r.new_pc );
                                return TRUE;
                                }
                                break;
                            case 0x2:
                                { /* BRAF Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                CHECKSLOTILLEGAL();
                                CHECKDEST( pc + 4 + sh4r.r[Rn] );
                                sh4r.in_delay_slot = 1;
                                sh4r.pc = sh4r.new_pc;
                                sh4r.new_pc = pc + 4 + sh4r.r[Rn];
                                return TRUE;
                                }
                                break;
                            case 0x8:
                                { /* PREF @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                tmp = sh4r.r[Rn];
                                if( (tmp & 0xFC000000) == 0xE0000000 ) {
                           	 /* Store queue operation */
                           	 int queue = (tmp&0x20)>>2;
                           	 int32_t *src = &sh4r.store_queue[queue];
                           	 uint32_t hi = (MMIO_READ( MMU, (queue == 0 ? QACR0 : QACR1) ) & 0x1C) << 24;
                           	 uint32_t target = tmp&0x03FFFFE0 | hi;
                           	 mem_copy_to_sh4( target, src, 32 );
                                }
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
                                tmp = sh4r.r[Rn];
                                CHECKWALIGN32(tmp);
                                MEM_WRITE_LONG( tmp, R0 );
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
                        MEM_WRITE_BYTE( R0 + sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x5:
                        { /* MOV.W Rm, @(R0, Rn) */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKWALIGN16( R0 + sh4r.r[Rn] );
                        MEM_WRITE_WORD( R0 + sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x6:
                        { /* MOV.L Rm, @(R0, Rn) */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKWALIGN32( R0 + sh4r.r[Rn] );
                        MEM_WRITE_LONG( R0 + sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x7:
                        { /* MUL.L Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.mac = (sh4r.mac&0xFFFFFFFF00000000LL) |
                                               (sh4r.r[Rm] * sh4r.r[Rn]);
                        }
                        break;
                    case 0x8:
                        switch( (ir&0xFF0) >> 4 ) {
                            case 0x0:
                                { /* CLRT */
                                sh4r.t = 0;
                                }
                                break;
                            case 0x1:
                                { /* SETT */
                                sh4r.t = 1;
                                }
                                break;
                            case 0x2:
                                { /* CLRMAC */
                                sh4r.mac = 0;
                                }
                                break;
                            case 0x3:
                                { /* LDTLB */
                                /* TODO */
                                }
                                break;
                            case 0x4:
                                { /* CLRS */
                                sh4r.s = 0;
                                }
                                break;
                            case 0x5:
                                { /* SETS */
                                sh4r.s = 1;
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
                                /* NOP */
                                }
                                break;
                            case 0x1:
                                { /* DIV0U */
                                sh4r.m = sh4r.q = sh4r.t = 0;
                                }
                                break;
                            case 0x2:
                                { /* MOVT Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] = sh4r.t;
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
                                sh4r.r[Rn] = (sh4r.mac>>32);
                                }
                                break;
                            case 0x1:
                                { /* STS MACL, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] = (uint32_t)sh4r.mac;
                                }
                                break;
                            case 0x2:
                                { /* STS PR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] = sh4r.pr;
                                }
                                break;
                            case 0x3:
                                { /* STC SGR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                CHECKPRIV();
                                sh4r.r[Rn] = sh4r.sgr;
                                }
                                break;
                            case 0x5:
                                { /* STS FPUL, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] = sh4r.fpul;
                                }
                                break;
                            case 0x6:
                                { /* STS FPSCR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] = sh4r.fpscr;
                                }
                                break;
                            case 0xF:
                                { /* STC DBR, Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                CHECKPRIV(); sh4r.r[Rn] = sh4r.dbr;
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
                                CHECKSLOTILLEGAL();
                                CHECKDEST( sh4r.pr );
                                sh4r.in_delay_slot = 1;
                                sh4r.pc = sh4r.new_pc;
                                sh4r.new_pc = sh4r.pr;
                                TRACE_RETURN( pc, sh4r.new_pc );
                                return TRUE;
                                }
                                break;
                            case 0x1:
                                { /* SLEEP */
                                if( MMIO_READ( CPG, STBCR ) & 0x80 ) {
                            	sh4r.sh4_state = SH4_STATE_STANDBY;
                                } else {
                            	sh4r.sh4_state = SH4_STATE_SLEEP;
                                }
                                return FALSE; /* Halt CPU */
                                }
                                break;
                            case 0x2:
                                { /* RTE */
                                CHECKPRIV();
                                CHECKDEST( sh4r.spc );
                                CHECKSLOTILLEGAL();
                                sh4r.in_delay_slot = 1;
                                sh4r.pc = sh4r.new_pc;
                                sh4r.new_pc = sh4r.spc;
                                sh4_load_sr( sh4r.ssr );
                                return TRUE;
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
                        sh4r.r[Rn] = MEM_READ_BYTE( R0 + sh4r.r[Rm] );
                        }
                        break;
                    case 0xD:
                        { /* MOV.W @(R0, Rm), Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN16( R0 + sh4r.r[Rm] );
                                           sh4r.r[Rn] = MEM_READ_WORD( R0 + sh4r.r[Rm] );
                        }
                        break;
                    case 0xE:
                        { /* MOV.L @(R0, Rm), Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN32( R0 + sh4r.r[Rm] );
                                           sh4r.r[Rn] = MEM_READ_LONG( R0 + sh4r.r[Rm] );
                        }
                        break;
                    case 0xF:
                        { /* MAC.L @Rm+, @Rn+ */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN32( sh4r.r[Rm] );
                        CHECKRALIGN32( sh4r.r[Rn] );
                        int64_t tmpl = SIGNEXT32(MEM_READ_LONG(sh4r.r[Rn]));
                        sh4r.r[Rn] += 4;
                        tmpl = tmpl * SIGNEXT32(MEM_READ_LONG(sh4r.r[Rm])) + sh4r.mac;
                        sh4r.r[Rm] += 4;
                        if( sh4r.s ) {
                            /* 48-bit Saturation. Yuch */
                            if( tmpl < (int64_t)0xFFFF800000000000LL )
                                tmpl = 0xFFFF800000000000LL;
                            else if( tmpl > (int64_t)0x00007FFFFFFFFFFFLL )
                                tmpl = 0x00007FFFFFFFFFFFLL;
                        }
                        sh4r.mac = tmpl;
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
                tmp = sh4r.r[Rn] + disp;
                CHECKWALIGN32( tmp );
                MEM_WRITE_LONG( tmp, sh4r.r[Rm] );
                }
                break;
            case 0x2:
                switch( ir&0xF ) {
                    case 0x0:
                        { /* MOV.B Rm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        MEM_WRITE_BYTE( sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x1:
                        { /* MOV.W Rm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKWALIGN16( sh4r.r[Rn] ); MEM_WRITE_WORD( sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x2:
                        { /* MOV.L Rm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKWALIGN32( sh4r.r[Rn] ); MEM_WRITE_LONG( sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x4:
                        { /* MOV.B Rm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] --; MEM_WRITE_BYTE( sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x5:
                        { /* MOV.W Rm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] -= 2; CHECKWALIGN16( sh4r.r[Rn] ); MEM_WRITE_WORD( sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x6:
                        { /* MOV.L Rm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] -= 4; CHECKWALIGN32( sh4r.r[Rn] ); MEM_WRITE_LONG( sh4r.r[Rn], sh4r.r[Rm] );
                        }
                        break;
                    case 0x7:
                        { /* DIV0S Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.q = sh4r.r[Rn]>>31;
                        sh4r.m = sh4r.r[Rm]>>31;
                        sh4r.t = sh4r.q ^ sh4r.m;
                        }
                        break;
                    case 0x8:
                        { /* TST Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.t = (sh4r.r[Rn]&sh4r.r[Rm] ? 0 : 1);
                        }
                        break;
                    case 0x9:
                        { /* AND Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] &= sh4r.r[Rm];
                        }
                        break;
                    case 0xA:
                        { /* XOR Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] ^= sh4r.r[Rm];
                        }
                        break;
                    case 0xB:
                        { /* OR Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] |= sh4r.r[Rm];
                        }
                        break;
                    case 0xC:
                        { /* CMP/STR Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        /* set T = 1 if any byte in RM & RN is the same */
                        tmp = sh4r.r[Rm] ^ sh4r.r[Rn];
                        sh4r.t = ((tmp&0x000000FF)==0 || (tmp&0x0000FF00)==0 ||
                                 (tmp&0x00FF0000)==0 || (tmp&0xFF000000)==0)?1:0;
                        }
                        break;
                    case 0xD:
                        { /* XTRCT Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = (sh4r.r[Rn]>>16) | (sh4r.r[Rm]<<16);
                        }
                        break;
                    case 0xE:
                        { /* MULU.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.mac = (sh4r.mac&0xFFFFFFFF00000000LL) |
                                   (uint32_t)((sh4r.r[Rm]&0xFFFF) * (sh4r.r[Rn]&0xFFFF));
                        }
                        break;
                    case 0xF:
                        { /* MULS.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.mac = (sh4r.mac&0xFFFFFFFF00000000LL) |
                                   (uint32_t)(SIGNEXT32(sh4r.r[Rm]&0xFFFF) * SIGNEXT32(sh4r.r[Rn]&0xFFFF));
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
                        sh4r.t = ( sh4r.r[Rm] == sh4r.r[Rn] ? 1 : 0 );
                        }
                        break;
                    case 0x2:
                        { /* CMP/HS Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.t = ( sh4r.r[Rn] >= sh4r.r[Rm] ? 1 : 0 );
                        }
                        break;
                    case 0x3:
                        { /* CMP/GE Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.t = ( ((int32_t)sh4r.r[Rn]) >= ((int32_t)sh4r.r[Rm]) ? 1 : 0 );
                        }
                        break;
                    case 0x4:
                        { /* DIV1 Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        /* This is just from the sh4p manual with some
                         * simplifications (someone want to check it's correct? :)
                         * Why they couldn't just provide a real DIV instruction...
                         */
                        uint32_t tmp0, tmp1, tmp2, dir;
                    
                        dir = sh4r.q ^ sh4r.m;
                        sh4r.q = (sh4r.r[Rn] >> 31);
                        tmp2 = sh4r.r[Rm];
                        sh4r.r[Rn] = (sh4r.r[Rn] << 1) | sh4r.t;
                        tmp0 = sh4r.r[Rn];
                        if( dir ) {
                             sh4r.r[Rn] += tmp2;
                             tmp1 = (sh4r.r[Rn]<tmp0 ? 1 : 0 );
                        } else {
                             sh4r.r[Rn] -= tmp2;
                             tmp1 = (sh4r.r[Rn]>tmp0 ? 1 : 0 );
                        }
                        sh4r.q ^= sh4r.m ^ tmp1;
                        sh4r.t = ( sh4r.q == sh4r.m ? 1 : 0 );
                        }
                        break;
                    case 0x5:
                        { /* DMULU.L Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.mac = ((uint64_t)sh4r.r[Rm]) * ((uint64_t)sh4r.r[Rn]);
                        }
                        break;
                    case 0x6:
                        { /* CMP/HI Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.t = ( sh4r.r[Rn] > sh4r.r[Rm] ? 1 : 0 );
                        }
                        break;
                    case 0x7:
                        { /* CMP/GT Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.t = ( ((int32_t)sh4r.r[Rn]) > ((int32_t)sh4r.r[Rm]) ? 1 : 0 );
                        }
                        break;
                    case 0x8:
                        { /* SUB Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] -= sh4r.r[Rm];
                        }
                        break;
                    case 0xA:
                        { /* SUBC Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        tmp = sh4r.r[Rn];
                        sh4r.r[Rn] = sh4r.r[Rn] - sh4r.r[Rm] - sh4r.t;
                        sh4r.t = (sh4r.r[Rn] > tmp || (sh4r.r[Rn] == tmp && sh4r.t == 1));
                        }
                        break;
                    case 0xB:
                        UNIMP(ir); /* SUBV Rm, Rn */
                        break;
                    case 0xC:
                        { /* ADD Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] += sh4r.r[Rm];
                        }
                        break;
                    case 0xD:
                        { /* DMULS.L Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.mac = SIGNEXT32(sh4r.r[Rm]) * SIGNEXT32(sh4r.r[Rn]);
                        }
                        break;
                    case 0xE:
                        { /* ADDC Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        tmp = sh4r.r[Rn];
                        sh4r.r[Rn] += sh4r.r[Rm] + sh4r.t;
                        sh4r.t = ( sh4r.r[Rn] < tmp || (sh4r.r[Rn] == tmp && sh4r.t != 0) ? 1 : 0 );
                        }
                        break;
                    case 0xF:
                        { /* ADDV Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        tmp = sh4r.r[Rn] + sh4r.r[Rm];
                        sh4r.t = ( (sh4r.r[Rn]>>31) == (sh4r.r[Rm]>>31) && ((sh4r.r[Rn]>>31) != (tmp>>31)) );
                        sh4r.r[Rn] = tmp;
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
                                sh4r.t = sh4r.r[Rn] >> 31; sh4r.r[Rn] <<= 1;
                                }
                                break;
                            case 0x1:
                                { /* DT Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] --;
                                sh4r.t = ( sh4r.r[Rn] == 0 ? 1 : 0 );
                                }
                                break;
                            case 0x2:
                                { /* SHAL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.t = sh4r.r[Rn] >> 31;
                                sh4r.r[Rn] <<= 1;
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
                                sh4r.t = sh4r.r[Rn] & 0x00000001; sh4r.r[Rn] >>= 1;
                                }
                                break;
                            case 0x1:
                                { /* CMP/PZ Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.t = ( ((int32_t)sh4r.r[Rn]) >= 0 ? 1 : 0 );
                                }
                                break;
                            case 0x2:
                                { /* SHAR Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.t = sh4r.r[Rn] & 0x00000001;
                                sh4r.r[Rn] = ((int32_t)sh4r.r[Rn]) >> 1;
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
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], (sh4r.mac>>32) );
                                }
                                break;
                            case 0x1:
                                { /* STS.L MACL, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], (uint32_t)sh4r.mac );
                                }
                                break;
                            case 0x2:
                                { /* STS.L PR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], sh4r.pr );
                                }
                                break;
                            case 0x3:
                                { /* STC.L SGR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                CHECKPRIV();
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], sh4r.sgr );
                                }
                                break;
                            case 0x5:
                                { /* STS.L FPUL, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], sh4r.fpul );
                                }
                                break;
                            case 0x6:
                                { /* STS.L FPSCR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], sh4r.fpscr );
                                }
                                break;
                            case 0xF:
                                { /* STC.L DBR, @-Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                CHECKPRIV();
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], sh4r.dbr );
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
                                        CHECKPRIV();
                                        sh4r.r[Rn] -= 4;
                                        CHECKWALIGN32( sh4r.r[Rn] );
                                        MEM_WRITE_LONG( sh4r.r[Rn], sh4_read_sr() );
                                        }
                                        break;
                                    case 0x1:
                                        { /* STC.L GBR, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        sh4r.r[Rn] -= 4;
                                        CHECKWALIGN32( sh4r.r[Rn] );
                                        MEM_WRITE_LONG( sh4r.r[Rn], sh4r.gbr );
                                        }
                                        break;
                                    case 0x2:
                                        { /* STC.L VBR, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.r[Rn] -= 4;
                                        CHECKWALIGN32( sh4r.r[Rn] );
                                        MEM_WRITE_LONG( sh4r.r[Rn], sh4r.vbr );
                                        }
                                        break;
                                    case 0x3:
                                        { /* STC.L SSR, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.r[Rn] -= 4;
                                        CHECKWALIGN32( sh4r.r[Rn] );
                                        MEM_WRITE_LONG( sh4r.r[Rn], sh4r.ssr );
                                        }
                                        break;
                                    case 0x4:
                                        { /* STC.L SPC, @-Rn */
                                        uint32_t Rn = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.r[Rn] -= 4;
                                        CHECKWALIGN32( sh4r.r[Rn] );
                                        MEM_WRITE_LONG( sh4r.r[Rn], sh4r.spc );
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
                                CHECKPRIV();
                                sh4r.r[Rn] -= 4;
                                CHECKWALIGN32( sh4r.r[Rn] );
                                MEM_WRITE_LONG( sh4r.r[Rn], sh4r.r_bank[Rm_BANK] );
                                }
                                break;
                        }
                        break;
                    case 0x4:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* ROTL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.t = sh4r.r[Rn] >> 31;
                                sh4r.r[Rn] <<= 1;
                                sh4r.r[Rn] |= sh4r.t;
                                }
                                break;
                            case 0x2:
                                { /* ROTCL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                tmp = sh4r.r[Rn] >> 31;
                                sh4r.r[Rn] <<= 1;
                                sh4r.r[Rn] |= sh4r.t;
                                sh4r.t = tmp;
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
                                sh4r.t = sh4r.r[Rn] & 0x00000001;
                                sh4r.r[Rn] >>= 1;
                                sh4r.r[Rn] |= (sh4r.t << 31);
                                }
                                break;
                            case 0x1:
                                { /* CMP/PL Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.t = ( ((int32_t)sh4r.r[Rn]) > 0 ? 1 : 0 );
                                }
                                break;
                            case 0x2:
                                { /* ROTCR Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                tmp = sh4r.r[Rn] & 0x00000001;
                                sh4r.r[Rn] >>= 1;
                                sh4r.r[Rn] |= (sh4r.t << 31 );
                                sh4r.t = tmp;
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
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.mac = (sh4r.mac & 0x00000000FFFFFFFF) |
                                           (((uint64_t)MEM_READ_LONG(sh4r.r[Rm]))<<32);
                                sh4r.r[Rm] += 4;
                                }
                                break;
                            case 0x1:
                                { /* LDS.L @Rm+, MACL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.mac = (sh4r.mac & 0xFFFFFFFF00000000LL) |
                                           (uint64_t)((uint32_t)MEM_READ_LONG(sh4r.r[Rm]));
                                sh4r.r[Rm] += 4;
                                }
                                break;
                            case 0x2:
                                { /* LDS.L @Rm+, PR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.pr = MEM_READ_LONG( sh4r.r[Rm] );
                                sh4r.r[Rm] += 4;
                                }
                                break;
                            case 0x3:
                                { /* LDC.L @Rm+, SGR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKPRIV();
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.sgr = MEM_READ_LONG(sh4r.r[Rm]);
                                sh4r.r[Rm] +=4;
                                }
                                break;
                            case 0x5:
                                { /* LDS.L @Rm+, FPUL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.fpul = MEM_READ_LONG(sh4r.r[Rm]);
                                sh4r.r[Rm] +=4;
                                }
                                break;
                            case 0x6:
                                { /* LDS.L @Rm+, FPSCR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.fpscr = MEM_READ_LONG(sh4r.r[Rm]);
                                sh4r.r[Rm] +=4;
                                }
                                break;
                            case 0xF:
                                { /* LDC.L @Rm+, DBR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKPRIV();
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.dbr = MEM_READ_LONG(sh4r.r[Rm]);
                                sh4r.r[Rm] +=4;
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
                                        CHECKSLOTILLEGAL();
                                        CHECKPRIV();
                                        CHECKWALIGN32( sh4r.r[Rm] );
                                        sh4_load_sr( MEM_READ_LONG(sh4r.r[Rm]) );
                                        sh4r.r[Rm] +=4;
                                        }
                                        break;
                                    case 0x1:
                                        { /* LDC.L @Rm+, GBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKRALIGN32( sh4r.r[Rm] );
                                        sh4r.gbr = MEM_READ_LONG(sh4r.r[Rm]);
                                        sh4r.r[Rm] +=4;
                                        }
                                        break;
                                    case 0x2:
                                        { /* LDC.L @Rm+, VBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        CHECKRALIGN32( sh4r.r[Rm] );
                                        sh4r.vbr = MEM_READ_LONG(sh4r.r[Rm]);
                                        sh4r.r[Rm] +=4;
                                        }
                                        break;
                                    case 0x3:
                                        { /* LDC.L @Rm+, SSR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        CHECKRALIGN32( sh4r.r[Rm] );
                                        sh4r.ssr = MEM_READ_LONG(sh4r.r[Rm]);
                                        sh4r.r[Rm] +=4;
                                        }
                                        break;
                                    case 0x4:
                                        { /* LDC.L @Rm+, SPC */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        CHECKRALIGN32( sh4r.r[Rm] );
                                        sh4r.spc = MEM_READ_LONG(sh4r.r[Rm]);
                                        sh4r.r[Rm] +=4;
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
                                CHECKPRIV();
                                CHECKRALIGN32( sh4r.r[Rm] );
                                sh4r.r_bank[Rn_BANK] = MEM_READ_LONG( sh4r.r[Rm] );
                                sh4r.r[Rm] += 4;
                                }
                                break;
                        }
                        break;
                    case 0x8:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* SHLL2 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] <<= 2;
                                }
                                break;
                            case 0x1:
                                { /* SHLL8 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] <<= 8;
                                }
                                break;
                            case 0x2:
                                { /* SHLL16 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] <<= 16;
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
                                sh4r.r[Rn] >>= 2;
                                }
                                break;
                            case 0x1:
                                { /* SHLR8 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] >>= 8;
                                }
                                break;
                            case 0x2:
                                { /* SHLR16 Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                sh4r.r[Rn] >>= 16;
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
                                sh4r.mac = (sh4r.mac & 0x00000000FFFFFFFF) |
                                           (((uint64_t)sh4r.r[Rm])<<32);
                                }
                                break;
                            case 0x1:
                                { /* LDS Rm, MACL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                sh4r.mac = (sh4r.mac & 0xFFFFFFFF00000000LL) |
                                           (uint64_t)((uint32_t)(sh4r.r[Rm]));
                                }
                                break;
                            case 0x2:
                                { /* LDS Rm, PR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                sh4r.pr = sh4r.r[Rm];
                                }
                                break;
                            case 0x3:
                                { /* LDC Rm, SGR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKPRIV();
                                sh4r.sgr = sh4r.r[Rm];
                                }
                                break;
                            case 0x5:
                                { /* LDS Rm, FPUL */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                sh4r.fpul = sh4r.r[Rm];
                                }
                                break;
                            case 0x6:
                                { /* LDS Rm, FPSCR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                sh4r.fpscr = sh4r.r[Rm];
                                }
                                break;
                            case 0xF:
                                { /* LDC Rm, DBR */
                                uint32_t Rm = ((ir>>8)&0xF); 
                                CHECKPRIV();
                                sh4r.dbr = sh4r.r[Rm];
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
                                CHECKDEST( sh4r.r[Rn] );
                                CHECKSLOTILLEGAL();
                                sh4r.in_delay_slot = 1;
                                sh4r.pc = sh4r.new_pc;
                                sh4r.new_pc = sh4r.r[Rn];
                                sh4r.pr = pc + 4;
                                TRACE_CALL( pc, sh4r.new_pc );
                                return TRUE;
                                }
                                break;
                            case 0x1:
                                { /* TAS.B @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                tmp = MEM_READ_BYTE( sh4r.r[Rn] );
                                sh4r.t = ( tmp == 0 ? 1 : 0 );
                                MEM_WRITE_BYTE( sh4r.r[Rn], tmp | 0x80 );
                                }
                                break;
                            case 0x2:
                                { /* JMP @Rn */
                                uint32_t Rn = ((ir>>8)&0xF); 
                                CHECKDEST( sh4r.r[Rn] );
                                CHECKSLOTILLEGAL();
                                sh4r.in_delay_slot = 1;
                                sh4r.pc = sh4r.new_pc;
                                sh4r.new_pc = sh4r.r[Rn];
                                return TRUE;
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
                        tmp = sh4r.r[Rm];
                        if( (tmp & 0x80000000) == 0 ) sh4r.r[Rn] <<= (tmp&0x1f);
                        else if( (tmp & 0x1F) == 0 )  
                            sh4r.r[Rn] = ((int32_t)sh4r.r[Rn]) >> 31;
                        else 
                    	sh4r.r[Rn] = ((int32_t)sh4r.r[Rn]) >> (((~sh4r.r[Rm]) & 0x1F)+1);
                        }
                        break;
                    case 0xD:
                        { /* SHLD Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        tmp = sh4r.r[Rm];
                        if( (tmp & 0x80000000) == 0 ) sh4r.r[Rn] <<= (tmp&0x1f);
                        else if( (tmp & 0x1F) == 0 ) sh4r.r[Rn] = 0;
                        else sh4r.r[Rn] >>= (((~tmp) & 0x1F)+1);
                        }
                        break;
                    case 0xE:
                        switch( (ir&0x80) >> 7 ) {
                            case 0x0:
                                switch( (ir&0x70) >> 4 ) {
                                    case 0x0:
                                        { /* LDC Rm, SR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKSLOTILLEGAL();
                                        CHECKPRIV();
                                        sh4_load_sr( sh4r.r[Rm] );
                                        }
                                        break;
                                    case 0x1:
                                        { /* LDC Rm, GBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        sh4r.gbr = sh4r.r[Rm];
                                        }
                                        break;
                                    case 0x2:
                                        { /* LDC Rm, VBR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.vbr = sh4r.r[Rm];
                                        }
                                        break;
                                    case 0x3:
                                        { /* LDC Rm, SSR */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.ssr = sh4r.r[Rm];
                                        }
                                        break;
                                    case 0x4:
                                        { /* LDC Rm, SPC */
                                        uint32_t Rm = ((ir>>8)&0xF); 
                                        CHECKPRIV();
                                        sh4r.spc = sh4r.r[Rm];
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
                                CHECKPRIV();
                                sh4r.r_bank[Rn_BANK] = sh4r.r[Rm];
                                }
                                break;
                        }
                        break;
                    case 0xF:
                        { /* MAC.W @Rm+, @Rn+ */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN16( sh4r.r[Rn] );
                        CHECKRALIGN16( sh4r.r[Rm] );
                        int32_t stmp = SIGNEXT16(MEM_READ_WORD(sh4r.r[Rn]));
                        sh4r.r[Rn] += 2;
                        stmp = stmp * SIGNEXT16(MEM_READ_WORD(sh4r.r[Rm]));
                        sh4r.r[Rm] += 2;
                        if( sh4r.s ) {
                    	int64_t tmpl = (int64_t)((int32_t)sh4r.mac) + (int64_t)stmp;
                    	if( tmpl > (int64_t)0x000000007FFFFFFFLL ) {
                    	    sh4r.mac = 0x000000017FFFFFFFLL;
                    	} else if( tmpl < (int64_t)0xFFFFFFFF80000000LL ) {
                    	    sh4r.mac = 0x0000000180000000LL;
                    	} else {
                    	    sh4r.mac = (sh4r.mac & 0xFFFFFFFF00000000LL) |
                    		((uint32_t)(sh4r.mac + stmp));
                    	}
                        } else {
                    	sh4r.mac += SIGNEXT32(stmp);
                        }
                        }
                        break;
                }
                break;
            case 0x5:
                { /* MOV.L @(disp, Rm), Rn */
                uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); uint32_t disp = (ir&0xF)<<2; 
                tmp = sh4r.r[Rm] + disp;
                CHECKRALIGN32( tmp );
                sh4r.r[Rn] = MEM_READ_LONG( tmp );
                }
                break;
            case 0x6:
                switch( ir&0xF ) {
                    case 0x0:
                        { /* MOV.B @Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = MEM_READ_BYTE( sh4r.r[Rm] );
                        }
                        break;
                    case 0x1:
                        { /* MOV.W @Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN16( sh4r.r[Rm] ); sh4r.r[Rn] = MEM_READ_WORD( sh4r.r[Rm] );
                        }
                        break;
                    case 0x2:
                        { /* MOV.L @Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN32( sh4r.r[Rm] ); sh4r.r[Rn] = MEM_READ_LONG( sh4r.r[Rm] );
                        }
                        break;
                    case 0x3:
                        { /* MOV Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = sh4r.r[Rm];
                        }
                        break;
                    case 0x4:
                        { /* MOV.B @Rm+, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = MEM_READ_BYTE( sh4r.r[Rm] ); sh4r.r[Rm] ++;
                        }
                        break;
                    case 0x5:
                        { /* MOV.W @Rm+, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN16( sh4r.r[Rm] ); sh4r.r[Rn] = MEM_READ_WORD( sh4r.r[Rm] ); sh4r.r[Rm] += 2;
                        }
                        break;
                    case 0x6:
                        { /* MOV.L @Rm+, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        CHECKRALIGN32( sh4r.r[Rm] ); sh4r.r[Rn] = MEM_READ_LONG( sh4r.r[Rm] ); sh4r.r[Rm] += 4;
                        }
                        break;
                    case 0x7:
                        { /* NOT Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = ~sh4r.r[Rm];
                        }
                        break;
                    case 0x8:
                        { /* SWAP.B Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = (sh4r.r[Rm]&0xFFFF0000) | ((sh4r.r[Rm]&0x0000FF00)>>8) | ((sh4r.r[Rm]&0x000000FF)<<8);
                        }
                        break;
                    case 0x9:
                        { /* SWAP.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = (sh4r.r[Rm]>>16) | (sh4r.r[Rm]<<16);
                        }
                        break;
                    case 0xA:
                        { /* NEGC Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        tmp = 0 - sh4r.r[Rm];
                        sh4r.r[Rn] = tmp - sh4r.t;
                        sh4r.t = ( 0<tmp || tmp<sh4r.r[Rn] ? 1 : 0 );
                        }
                        break;
                    case 0xB:
                        { /* NEG Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = 0 - sh4r.r[Rm];
                        }
                        break;
                    case 0xC:
                        { /* EXTU.B Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = sh4r.r[Rm]&0x000000FF;
                        }
                        break;
                    case 0xD:
                        { /* EXTU.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = sh4r.r[Rm]&0x0000FFFF;
                        }
                        break;
                    case 0xE:
                        { /* EXTS.B Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = SIGNEXT8( sh4r.r[Rm]&0x000000FF );
                        }
                        break;
                    case 0xF:
                        { /* EXTS.W Rm, Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] = SIGNEXT16( sh4r.r[Rm]&0x0000FFFF );
                        }
                        break;
                }
                break;
            case 0x7:
                { /* ADD #imm, Rn */
                uint32_t Rn = ((ir>>8)&0xF); int32_t imm = SIGNEXT8(ir&0xFF); 
                sh4r.r[Rn] += imm;
                }
                break;
            case 0x8:
                switch( (ir&0xF00) >> 8 ) {
                    case 0x0:
                        { /* MOV.B R0, @(disp, Rn) */
                        uint32_t Rn = ((ir>>4)&0xF); uint32_t disp = (ir&0xF); 
                        MEM_WRITE_BYTE( sh4r.r[Rn] + disp, R0 );
                        }
                        break;
                    case 0x1:
                        { /* MOV.W R0, @(disp, Rn) */
                        uint32_t Rn = ((ir>>4)&0xF); uint32_t disp = (ir&0xF)<<1; 
                        tmp = sh4r.r[Rn] + disp;
                        CHECKWALIGN16( tmp );
                        MEM_WRITE_WORD( tmp, R0 );
                        }
                        break;
                    case 0x4:
                        { /* MOV.B @(disp, Rm), R0 */
                        uint32_t Rm = ((ir>>4)&0xF); uint32_t disp = (ir&0xF); 
                        R0 = MEM_READ_BYTE( sh4r.r[Rm] + disp );
                        }
                        break;
                    case 0x5:
                        { /* MOV.W @(disp, Rm), R0 */
                        uint32_t Rm = ((ir>>4)&0xF); uint32_t disp = (ir&0xF)<<1; 
                        tmp = sh4r.r[Rm] + disp;
                        CHECKRALIGN16( tmp );
                        R0 = MEM_READ_WORD( tmp );
                        }
                        break;
                    case 0x8:
                        { /* CMP/EQ #imm, R0 */
                        int32_t imm = SIGNEXT8(ir&0xFF); 
                        sh4r.t = ( R0 == imm ? 1 : 0 );
                        }
                        break;
                    case 0x9:
                        { /* BT disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        CHECKSLOTILLEGAL();
                        if( sh4r.t ) {
                            CHECKDEST( sh4r.pc + disp + 4 )
                            sh4r.pc += disp + 4;
                            sh4r.new_pc = sh4r.pc + 2;
                            return TRUE;
                        }
                        }
                        break;
                    case 0xB:
                        { /* BF disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        CHECKSLOTILLEGAL();
                        if( !sh4r.t ) {
                            CHECKDEST( sh4r.pc + disp + 4 )
                            sh4r.pc += disp + 4;
                            sh4r.new_pc = sh4r.pc + 2;
                            return TRUE;
                        }
                        }
                        break;
                    case 0xD:
                        { /* BT/S disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        CHECKSLOTILLEGAL();
                        if( sh4r.t ) {
                            CHECKDEST( sh4r.pc + disp + 4 )
                            sh4r.in_delay_slot = 1;
                            sh4r.pc = sh4r.new_pc;
                            sh4r.new_pc = pc + disp + 4;
                            sh4r.in_delay_slot = 1;
                            return TRUE;
                        }
                        }
                        break;
                    case 0xF:
                        { /* BF/S disp */
                        int32_t disp = SIGNEXT8(ir&0xFF)<<1; 
                        CHECKSLOTILLEGAL();
                        if( !sh4r.t ) {
                            CHECKDEST( sh4r.pc + disp + 4 )
                            sh4r.in_delay_slot = 1;
                            sh4r.pc = sh4r.new_pc;
                            sh4r.new_pc = pc + disp + 4;
                            return TRUE;
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
                CHECKSLOTILLEGAL();
                tmp = pc + 4 + disp;
                sh4r.r[Rn] = MEM_READ_WORD( tmp );
                }
                break;
            case 0xA:
                { /* BRA disp */
                int32_t disp = SIGNEXT12(ir&0xFFF)<<1; 
                CHECKSLOTILLEGAL();
                CHECKDEST( sh4r.pc + disp + 4 );
                sh4r.in_delay_slot = 1;
                sh4r.pc = sh4r.new_pc;
                sh4r.new_pc = pc + 4 + disp;
                return TRUE;
                }
                break;
            case 0xB:
                { /* BSR disp */
                int32_t disp = SIGNEXT12(ir&0xFFF)<<1; 
                CHECKDEST( sh4r.pc + disp + 4 );
                CHECKSLOTILLEGAL();
                sh4r.in_delay_slot = 1;
                sh4r.pr = pc + 4;
                sh4r.pc = sh4r.new_pc;
                sh4r.new_pc = pc + 4 + disp;
                TRACE_CALL( pc, sh4r.new_pc );
                return TRUE;
                }
                break;
            case 0xC:
                switch( (ir&0xF00) >> 8 ) {
                    case 0x0:
                        { /* MOV.B R0, @(disp, GBR) */
                        uint32_t disp = (ir&0xFF); 
                        MEM_WRITE_BYTE( sh4r.gbr + disp, R0 );
                        }
                        break;
                    case 0x1:
                        { /* MOV.W R0, @(disp, GBR) */
                        uint32_t disp = (ir&0xFF)<<1; 
                        tmp = sh4r.gbr + disp;
                        CHECKWALIGN16( tmp );
                        MEM_WRITE_WORD( tmp, R0 );
                        }
                        break;
                    case 0x2:
                        { /* MOV.L R0, @(disp, GBR) */
                        uint32_t disp = (ir&0xFF)<<2; 
                        tmp = sh4r.gbr + disp;
                        CHECKWALIGN32( tmp );
                        MEM_WRITE_LONG( tmp, R0 );
                        }
                        break;
                    case 0x3:
                        { /* TRAPA #imm */
                        uint32_t imm = (ir&0xFF); 
                        CHECKSLOTILLEGAL();
                        MMIO_WRITE( MMU, TRA, imm<<2 );
                        sh4r.pc += 2;
                        sh4_raise_exception( EXC_TRAP );
                        }
                        break;
                    case 0x4:
                        { /* MOV.B @(disp, GBR), R0 */
                        uint32_t disp = (ir&0xFF); 
                        R0 = MEM_READ_BYTE( sh4r.gbr + disp );
                        }
                        break;
                    case 0x5:
                        { /* MOV.W @(disp, GBR), R0 */
                        uint32_t disp = (ir&0xFF)<<1; 
                        tmp = sh4r.gbr + disp;
                        CHECKRALIGN16( tmp );
                        R0 = MEM_READ_WORD( tmp );
                        }
                        break;
                    case 0x6:
                        { /* MOV.L @(disp, GBR), R0 */
                        uint32_t disp = (ir&0xFF)<<2; 
                        tmp = sh4r.gbr + disp;
                        CHECKRALIGN32( tmp );
                        R0 = MEM_READ_LONG( tmp );
                        }
                        break;
                    case 0x7:
                        { /* MOVA @(disp, PC), R0 */
                        uint32_t disp = (ir&0xFF)<<2; 
                        CHECKSLOTILLEGAL();
                        R0 = (pc&0xFFFFFFFC) + disp + 4;
                        }
                        break;
                    case 0x8:
                        { /* TST #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        sh4r.t = (R0 & imm ? 0 : 1);
                        }
                        break;
                    case 0x9:
                        { /* AND #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        R0 &= imm;
                        }
                        break;
                    case 0xA:
                        { /* XOR #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        R0 ^= imm;
                        }
                        break;
                    case 0xB:
                        { /* OR #imm, R0 */
                        uint32_t imm = (ir&0xFF); 
                        R0 |= imm;
                        }
                        break;
                    case 0xC:
                        { /* TST.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        sh4r.t = ( MEM_READ_BYTE(R0 + sh4r.gbr) & imm ? 0 : 1 );
                        }
                        break;
                    case 0xD:
                        { /* AND.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        MEM_WRITE_BYTE( R0 + sh4r.gbr, imm & MEM_READ_BYTE(R0 + sh4r.gbr) );
                        }
                        break;
                    case 0xE:
                        { /* XOR.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        MEM_WRITE_BYTE( R0 + sh4r.gbr, imm ^ MEM_READ_BYTE(R0 + sh4r.gbr) );
                        }
                        break;
                    case 0xF:
                        { /* OR.B #imm, @(R0, GBR) */
                        uint32_t imm = (ir&0xFF); 
                        MEM_WRITE_BYTE( R0 + sh4r.gbr, imm | MEM_READ_BYTE(R0 + sh4r.gbr) );
                        }
                        break;
                }
                break;
            case 0xD:
                { /* MOV.L @(disp, PC), Rn */
                uint32_t Rn = ((ir>>8)&0xF); uint32_t disp = (ir&0xFF)<<2; 
                CHECKSLOTILLEGAL();
                tmp = (pc&0xFFFFFFFC) + disp + 4;
                sh4r.r[Rn] = MEM_READ_LONG( tmp );
                }
                break;
            case 0xE:
                { /* MOV #imm, Rn */
                uint32_t Rn = ((ir>>8)&0xF); int32_t imm = SIGNEXT8(ir&0xFF); 
                sh4r.r[Rn] = imm;
                }
                break;
            case 0xF:
                switch( ir&0xF ) {
                    case 0x0:
                        { /* FADD FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        CHECKFPUEN();
                        if( IS_FPU_DOUBLEPREC() ) {
                    	DR(FRn) += DR(FRm);
                        } else {
                    	FR(FRn) += FR(FRm);
                        }
                        }
                        break;
                    case 0x1:
                        { /* FSUB FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        CHECKFPUEN();
                        if( IS_FPU_DOUBLEPREC() ) {
                    	DR(FRn) -= DR(FRm);
                        } else {
                    	FR(FRn) -= FR(FRm);
                        }
                        }
                        break;
                    case 0x2:
                        { /* FMUL FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        CHECKFPUEN();
                        if( IS_FPU_DOUBLEPREC() ) {
                    	DR(FRn) *= DR(FRm);
                        } else {
                    	FR(FRn) *= FR(FRm);
                        }
                        }
                        break;
                    case 0x3:
                        { /* FDIV FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        CHECKFPUEN();
                        if( IS_FPU_DOUBLEPREC() ) {
                    	DR(FRn) /= DR(FRm);
                        } else {
                    	FR(FRn) /= FR(FRm);
                        }
                        }
                        break;
                    case 0x4:
                        { /* FCMP/EQ FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        CHECKFPUEN();
                        if( IS_FPU_DOUBLEPREC() ) {
                    	sh4r.t = ( DR(FRn) == DR(FRm) ? 1 : 0 );
                        } else {
                    	sh4r.t = ( FR(FRn) == FR(FRm) ? 1 : 0 );
                        }
                        }
                        break;
                    case 0x5:
                        { /* FCMP/GT FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        CHECKFPUEN();
                        if( IS_FPU_DOUBLEPREC() ) {
                    	sh4r.t = ( DR(FRn) > DR(FRm) ? 1 : 0 );
                        } else {
                    	sh4r.t = ( FR(FRn) > FR(FRm) ? 1 : 0 );
                        }
                        }
                        break;
                    case 0x6:
                        { /* FMOV @(R0, Rm), FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        MEM_FP_READ( sh4r.r[Rm] + R0, FRn );
                        }
                        break;
                    case 0x7:
                        { /* FMOV FRm, @(R0, Rn) */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        MEM_FP_WRITE( sh4r.r[Rn] + R0, FRm );
                        }
                        break;
                    case 0x8:
                        { /* FMOV @Rm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        MEM_FP_READ( sh4r.r[Rm], FRn );
                        }
                        break;
                    case 0x9:
                        { /* FMOV @Rm+, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t Rm = ((ir>>4)&0xF); 
                        MEM_FP_READ( sh4r.r[Rm], FRn ); sh4r.r[Rm] += FP_WIDTH;
                        }
                        break;
                    case 0xA:
                        { /* FMOV FRm, @Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        MEM_FP_WRITE( sh4r.r[Rn], FRm );
                        }
                        break;
                    case 0xB:
                        { /* FMOV FRm, @-Rn */
                        uint32_t Rn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        sh4r.r[Rn] -= FP_WIDTH; MEM_FP_WRITE( sh4r.r[Rn], FRm );
                        }
                        break;
                    case 0xC:
                        { /* FMOV FRm, FRn */
                        uint32_t FRn = ((ir>>8)&0xF); uint32_t FRm = ((ir>>4)&0xF); 
                        if( IS_FPU_DOUBLESIZE() )
                    	DR(FRn) = DR(FRm);
                        else
                    	FR(FRn) = FR(FRm);
                        }
                        break;
                    case 0xD:
                        switch( (ir&0xF0) >> 4 ) {
                            case 0x0:
                                { /* FSTS FPUL, FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN(); FR(FRn) = FPULf;
                                }
                                break;
                            case 0x1:
                                { /* FLDS FRm, FPUL */
                                uint32_t FRm = ((ir>>8)&0xF); 
                                CHECKFPUEN(); FPULf = FR(FRm);
                                }
                                break;
                            case 0x2:
                                { /* FLOAT FPUL, FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() )
                            	DR(FRn) = (float)FPULi;
                                else
                            	FR(FRn) = (float)FPULi;
                                }
                                break;
                            case 0x3:
                                { /* FTRC FRm, FPUL */
                                uint32_t FRm = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() ) {
                                    dtmp = DR(FRm);
                                    if( dtmp >= MAX_INTF )
                                        FPULi = MAX_INT;
                                    else if( dtmp <= MIN_INTF )
                                        FPULi = MIN_INT;
                                    else 
                                        FPULi = (int32_t)dtmp;
                                } else {
                            	ftmp = FR(FRm);
                            	if( ftmp >= MAX_INTF )
                            	    FPULi = MAX_INT;
                            	else if( ftmp <= MIN_INTF )
                            	    FPULi = MIN_INT;
                            	else
                            	    FPULi = (int32_t)ftmp;
                                }
                                }
                                break;
                            case 0x4:
                                { /* FNEG FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() ) {
                            	DR(FRn) = -DR(FRn);
                                } else {
                                    FR(FRn) = -FR(FRn);
                                }
                                }
                                break;
                            case 0x5:
                                { /* FABS FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() ) {
                            	DR(FRn) = fabs(DR(FRn));
                                } else {
                                    FR(FRn) = fabsf(FR(FRn));
                                }
                                }
                                break;
                            case 0x6:
                                { /* FSQRT FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() ) {
                            	DR(FRn) = sqrt(DR(FRn));
                                } else {
                                    FR(FRn) = sqrtf(FR(FRn));
                                }
                                }
                                break;
                            case 0x7:
                                { /* FSRRA FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( !IS_FPU_DOUBLEPREC() ) {
                            	FR(FRn) = 1.0/sqrtf(FR(FRn));
                                }
                                }
                                break;
                            case 0x8:
                                { /* FLDI0 FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() ) {
                            	DR(FRn) = 0.0;
                                } else {
                                    FR(FRn) = 0.0;
                                }
                                }
                                break;
                            case 0x9:
                                { /* FLDI1 FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() ) {
                            	DR(FRn) = 1.0;
                                } else {
                                    FR(FRn) = 1.0;
                                }
                                }
                                break;
                            case 0xA:
                                { /* FCNVSD FPUL, FRn */
                                uint32_t FRn = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() && !IS_FPU_DOUBLESIZE() ) {
                            	DR(FRn) = (double)FPULf;
                                }
                                }
                                break;
                            case 0xB:
                                { /* FCNVDS FRm, FPUL */
                                uint32_t FRm = ((ir>>8)&0xF); 
                                CHECKFPUEN();
                                if( IS_FPU_DOUBLEPREC() && !IS_FPU_DOUBLESIZE() ) {
                            	FPULf = (float)DR(FRm);
                                }
                                }
                                break;
                            case 0xE:
                                { /* FIPR FVm, FVn */
                                uint32_t FVn = ((ir>>10)&0x3); uint32_t FVm = ((ir>>8)&0x3); 
                                CHECKFPUEN();
                                if( !IS_FPU_DOUBLEPREC() ) {
                                    int tmp2 = FVn<<2;
                                    tmp = FVm<<2;
                                    FR(tmp2+3) = FR(tmp)*FR(tmp2) +
                                        FR(tmp+1)*FR(tmp2+1) +
                                        FR(tmp+2)*FR(tmp2+2) +
                                        FR(tmp+3)*FR(tmp2+3);
                                }
                                }
                                break;
                            case 0xF:
                                switch( (ir&0x100) >> 8 ) {
                                    case 0x0:
                                        { /* FSCA FPUL, FRn */
                                        uint32_t FRn = ((ir>>9)&0x7)<<1; 
                                        CHECKFPUEN();
                                        if( !IS_FPU_DOUBLEPREC() ) {
                                            float angle = (((float)(FPULi&0xFFFF))/65536.0) * 2 * M_PI;
                                            FR(FRn) = sinf(angle);
                                            FR((FRn)+1) = cosf(angle);
                                        }
                                        }
                                        break;
                                    case 0x1:
                                        switch( (ir&0x200) >> 9 ) {
                                            case 0x0:
                                                { /* FTRV XMTRX, FVn */
                                                uint32_t FVn = ((ir>>10)&0x3); 
                                                CHECKFPUEN();
                                                if( !IS_FPU_DOUBLEPREC() ) {
                                                    tmp = FVn<<2;
                                                    float fv[4] = { FR(tmp), FR(tmp+1), FR(tmp+2), FR(tmp+3) };
                                                    FR(tmp) = XF(0) * fv[0] + XF(4)*fv[1] +
                                            	    XF(8)*fv[2] + XF(12)*fv[3];
                                                    FR(tmp+1) = XF(1) * fv[0] + XF(5)*fv[1] +
                                            	    XF(9)*fv[2] + XF(13)*fv[3];
                                                    FR(tmp+2) = XF(2) * fv[0] + XF(6)*fv[1] +
                                            	    XF(10)*fv[2] + XF(14)*fv[3];
                                                    FR(tmp+3) = XF(3) * fv[0] + XF(7)*fv[1] +
                                            	    XF(11)*fv[2] + XF(15)*fv[3];
                                                }
                                                }
                                                break;
                                            case 0x1:
                                                switch( (ir&0xC00) >> 10 ) {
                                                    case 0x0:
                                                        { /* FSCHG */
                                                        CHECKFPUEN(); sh4r.fpscr ^= FPSCR_SZ;
                                                        }
                                                        break;
                                                    case 0x2:
                                                        { /* FRCHG */
                                                        CHECKFPUEN(); sh4r.fpscr ^= FPSCR_FR;
                                                        }
                                                        break;
                                                    case 0x3:
                                                        { /* UNDEF */
                                                        UNDEF(ir);
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
                        CHECKFPUEN();
                        if( IS_FPU_DOUBLEPREC() ) {
                            DR(FRn) += DR(FRm)*DR(0);
                        } else {
                    	FR(FRn) += FR(FRm)*FR(0);
                        }
                        }
                        break;
                    default:
                        UNDEF();
                        break;
                }
                break;
        }

    sh4r.pc = sh4r.new_pc;
    sh4r.new_pc += 2;
    sh4r.in_delay_slot = 0;
    return TRUE;
}
