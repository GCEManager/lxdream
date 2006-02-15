/**
 * $Id: asic.c,v 1.12 2006-02-15 13:11:42 nkeynes Exp $
 *
 * Support for the miscellaneous ASIC functions (Primarily event multiplexing,
 * and DMA). 
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

#define MODULE asic_module

#include <assert.h>
#include "dream.h"
#include "mem.h"
#include "sh4/intc.h"
#include "sh4/dmac.h"
#include "dreamcast.h"
#include "maple/maple.h"
#include "gdrom/ide.h"
#include "asic.h"
#define MMIO_IMPL
#include "asic.h"
/*
 * Open questions:
 *   1) Does changing the mask after event occurance result in the
 *      interrupt being delivered immediately?
 * TODO: Logic diagram of ASIC event/interrupt logic.
 *
 * ... don't even get me started on the "EXTDMA" page, about which, apparently,
 * practically nothing is publicly known...
 */

struct dreamcast_module asic_module = { "ASIC", asic_init, NULL, NULL, NULL,
					NULL, NULL, NULL };

void asic_check_cleared_events( void );

void asic_init( void )
{
    register_io_region( &mmio_region_ASIC );
    register_io_region( &mmio_region_EXTDMA );
    mmio_region_ASIC.trace_flag = 0; /* Because this is called so often */
    asic_event( EVENT_GDROM_CMD );
}

void mmio_region_ASIC_write( uint32_t reg, uint32_t val )
{
    switch( reg ) {
    case PIRQ0:
    case PIRQ1:
    case PIRQ2:
	/* Clear any interrupts */
	MMIO_WRITE( ASIC, reg, MMIO_READ(ASIC, reg)&~val );
	asic_check_cleared_events();
	break;
    case MAPLE_STATE:
	MMIO_WRITE( ASIC, reg, val );
	if( val & 1 ) {
	    uint32_t maple_addr = MMIO_READ( ASIC, MAPLE_DMA) &0x1FFFFFE0;
	    WARN( "Maple request initiated at %08X, halting", maple_addr );
	    maple_handle_buffer( maple_addr );
	    MMIO_WRITE( ASIC, reg, 0 );
	}
	break;
    case PVRDMACTL: /* Initiate PVR DMA transfer */
	MMIO_WRITE( ASIC, reg, val );
	WARN( "Write to ASIC (%03X <= %08X) [%s: %s]",
	      reg, val, MMIO_REGID(ASIC,reg), MMIO_REGDESC(ASIC,reg) );
	if( val & 1 ) {
	    uint32_t dest_addr = MMIO_READ( ASIC, PVRDMADEST) &0x1FFFFFE0;
	    uint32_t count = MMIO_READ( ASIC, PVRDMACNT );
	    char *data = alloca( count );
	    uint32_t rcount = DMAC_get_buffer( 2, data, count );
	    if( rcount != count )
		WARN( "PVR received %08X bytes from DMA, expected %08X", rcount, count );
	    mem_copy_to_sh4( dest_addr, data, rcount );
	    asic_event( EVENT_PVR_DMA );
	}
	break;
    default:
	MMIO_WRITE( ASIC, reg, val );
	WARN( "Write to ASIC (%03X <= %08X) [%s: %s]",
	      reg, val, MMIO_REGID(ASIC,reg), MMIO_REGDESC(ASIC,reg) );
    }
}

int32_t mmio_region_ASIC_read( uint32_t reg )
{
    int32_t val;
    switch( reg ) {
        /*
        case 0x89C:
            sh4_stop();
            return 0x000000B;
        */     
    case PIRQ0:
    case PIRQ1:
    case PIRQ2:
    case IRQA0:
    case IRQA1:
    case IRQA2:
    case IRQB0:
    case IRQB1:
    case IRQB2:
    case IRQC0:
    case IRQC1:
    case IRQC2:
	val = MMIO_READ(ASIC, reg);
	//            WARN( "Read from ASIC (%03X => %08X) [%s: %s]",
	//                  reg, val, MMIO_REGID(ASIC,reg), MMIO_REGDESC(ASIC,reg) );
	return val;            
    case G2STATUS:
	return 0; /* find out later if there's any cases we actually need to care about */
    default:
	val = MMIO_READ(ASIC, reg);
	WARN( "Read from ASIC (%03X => %08X) [%s: %s]",
	      reg, val, MMIO_REGID(ASIC,reg), MMIO_REGDESC(ASIC,reg) );
	return val;
    }
    
}

void asic_event( int event )
{
    int offset = ((event&0x60)>>3);
    int result = (MMIO_READ(ASIC, PIRQ0 + offset))  |=  (1<<(event&0x1F));

    if( result & MMIO_READ(ASIC, IRQA0 + offset) )
        intc_raise_interrupt( INT_IRQ13 );
    if( result & MMIO_READ(ASIC, IRQB0 + offset) )
        intc_raise_interrupt( INT_IRQ11 );
    if( result & MMIO_READ(ASIC, IRQC0 + offset) )
        intc_raise_interrupt( INT_IRQ9 );
}

void asic_check_cleared_events( )
{
    int i, setA = 0, setB = 0, setC = 0;
    uint32_t bits;
    for( i=0; i<3; i++ ) {
	bits = MMIO_READ( ASIC, PIRQ0 + i );
	setA |= (bits & MMIO_READ(ASIC, IRQA0 + i ));
	setB |= (bits & MMIO_READ(ASIC, IRQB0 + i ));
	setC |= (bits & MMIO_READ(ASIC, IRQC0 + i ));
    }
    if( setA == 0 )
	intc_clear_interrupt( INT_IRQ13 );
    if( setB == 0 )
	intc_clear_interrupt( INT_IRQ11 );
    if( setC == 0 )
	intc_clear_interrupt( INT_IRQ9 );
}


MMIO_REGION_WRITE_FN( EXTDMA, reg, val )
{
    switch( reg ) {
        case IDEALTSTATUS: /* Device control */
            ide_write_control( val );
            break;
        case IDEDATA:
            ide_write_data_pio( val );
            break;
        case IDEFEAT:
            if( ide_can_write_regs() )
                idereg.feature = (uint8_t)val;
            break;
        case IDECOUNT:
            if( ide_can_write_regs() )
                idereg.count = (uint8_t)val;
            break;
        case IDELBA0:
            if( ide_can_write_regs() )
                idereg.lba0 = (uint8_t)val;
            break;
        case IDELBA1:
            if( ide_can_write_regs() )
                idereg.lba1 = (uint8_t)val;
            break;
        case IDELBA2:
            if( ide_can_write_regs() )
                idereg.lba2 = (uint8_t)val;
            break;
        case IDEDEV:
            if( ide_can_write_regs() )
                idereg.device = (uint8_t)val;
            break;
        case IDECMD:
            if( ide_can_write_regs() ) {
                ide_clear_interrupt();
                ide_write_command( (uint8_t)val );
            }
            break;
        default:
	    WARN( "EXTDMA write %08X <= %08X", reg, val );

            MMIO_WRITE( EXTDMA, reg, val );
    }
}

MMIO_REGION_READ_FN( EXTDMA, reg )
{
    uint32_t val;
    switch( reg ) {
        case IDEALTSTATUS: return idereg.status;
        case IDEDATA: return ide_read_data_pio( );
        case IDEFEAT: return idereg.error;
        case IDECOUNT:return idereg.count;
        case IDELBA0: return idereg.disc;
        case IDELBA1: return idereg.lba1;
        case IDELBA2: return idereg.lba2;
        case IDEDEV: return idereg.device;
        case IDECMD:
            ide_clear_interrupt();
            return idereg.status;
        default:
	    val = MMIO_READ( EXTDMA, reg );
	    //DEBUG( "EXTDMA read %08X => %08X", reg, val );
	    return val;
    }
}

