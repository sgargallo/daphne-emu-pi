/*
* mach3.cpp
*
* Copyright (C) 2003-2004 Warren Ondras (warren@dragons-lair-project.com)
*
* This file is part of DAPHNE, a laserdisc arcade game emulator
*
* DAPHNE is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* DAPHNE is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

// Thanks to Fabrice Frances & MAME team for I86 core

// TODO:
//  - prevent up/down or left/right switches active at the same time (causes sprite glitches)
//  - shift sprites  to the right, so targets line up (16 pixels?)
//  - hide bogus targets in upper right corner
//  - eliminate redundant ROM loads for UVT
//  - make test switch a toggle
//  - add samples for UVT/CC

#ifdef WIN32
#pragma warning (disable:4100) // disable warning about unreferenced parameter
#endif

// Win32 doesn't use strcasecmp, it uses stricmp (lame)
#ifdef WIN32
#define strcasecmp stricmp
#endif

#ifdef _XBOX
#include "../xbox/xbox_grafx.h"
#include "../cpu/mamewrap.h"
#endif

#include <stdio.h>	// for sprintf
#include <math.h>   // for pow() in palette gamma
#include "mach3.h"
#include "../video/palette.h"
#include "../ldp-out/ldp.h"
#include "../ldp-in/pr8210.h"
#include "../io/conout.h"
#include "../io/mpo_mem.h"
#include "../sound/sound.h"
#include "../cpu/nes6502.h"
#include "../cpu/cpu-debug.h"	// for set_cpu_trace when we use it
#ifndef _XBOX
#include "../cpu/x86/i86intf.h"
#else
#include "../cpu/i86intf.h"
#endif

#ifdef _XBOX
#include "../xbox/daphne_xbox.h"
#include "../xbox/xfile.h"
#include "../xbox/gamepad.h"
#endif


// MACK3 :)
// Memory Map (partial)

// 0x0000-0FFF RAM (battery-backed)
// 0x1000-2FFF RAM
// 0x3000-30FF sprite RAM
// 0x4000-4FFF character RAM 
// 0x5000-501F Palette RAM
// 0x5800-5807 I/O

// 0x6000-FFFF Program ROM 4
// 0x8000-FFFF Program ROM 3
// 0xA000-FFFF Program ROM 2
// 0xC000-FFFF Program ROM 1
// 0xE000-FFFF Program ROM 0

// sound is handled by a separate board with 2 6502 CPUs, 2 AY-8910's, and a (Votrax SC01?) speech chip
// these are not currently emulated

//////////////////////////////////////////////////////////////////////////

// mach3 class constructor
mach3::mach3()
{
	m_shortgamename = "mach3";
	memset(m_cpumem, 0, sizeof(m_cpumem));
	memset(m_cpumem2, 0, sizeof(m_cpumem2));
	memset(m_cpumem3, 0, sizeof(m_cpumem3));

	struct cpudef cpu;
	memset(&cpu, 0, sizeof(struct cpudef));
	cpu.type = CPU_I88;
	cpu.hz = MACH3_CPU_HZ / 4; // 5 MHz clock
	cpu.irq_period[0] = 0;  // mach3 doesn't use IRQs
	cpu.nmi_period = (1000.0 / 59.94);	// NMI generated by VBLANK (60 Hz) - 
	// *must* match mpeg frame rate, or target audio data decoder loses sync,
	// causing the dreaded 'DISC ERROR .... STAY PUT!' message

	cpu.initial_pc = 0xFFF0;	// restart vector (jumps to 0x8000 - would normally be mirrored up at F000:FFF0,
	// but we'll just access it from here
	cpu.must_copy_context = false;
	cpu.mem = m_cpumem;
	add_cpu(&cpu);	// add this cpu to the list (it will be our only one)

	cpu.type = CPU_M6502;
	cpu.hz = MACH3_CPU_HZ / 20;  // 1 MHz clock
	cpu.irq_period[0] = 0; // IRQ's generated by x86 cpu
	cpu.nmi_period = 0;	// NMI generated by CPU #2
	cpu.initial_pc = 0;
	cpu.must_copy_context = true;	// set this to true when you add multiple 6502's
	cpu.mem = m_cpumem2;
	add_cpu(&cpu);	// add first sound 6502 cpu

	cpu.type = CPU_M6502;
	cpu.hz = MACH3_CPU_HZ / 20; // 1 MHz clock
	cpu.irq_period[0] = 0; // IRQ's generated by x86 cpu
	cpu.nmi_period = 0;	// can be dynamically changed by cpu #2 writing to 0xA000
	cpu.initial_pc = 0;
	cpu.must_copy_context = true;	// set this to true when you add multiple 6502's
	cpu.mem = m_cpumem3;
	add_cpu(&cpu);	// add second sound 6502 cpu

	struct sounddef soundchip;
	soundchip.type = SOUNDCHIP_AY_3_8910;
	soundchip.hz = MACH3_CPU_HZ / 10; // 2 MHz clock
	m_soundchip1_id = add_soundchip(&soundchip); // first ay-3-8910
	m_soundchip2_id = add_soundchip(&soundchip); // second ay-3-8910

	soundchip.type = SOUNDCHIP_DAC;
	soundchip.hz = MACH3_CPU_HZ / 20;	// Hz of the 6502 that controls it
	m_dac_id = add_soundchip(&soundchip);	// the DAC

	m_disc_fps = 29.97;
	m_game_type = GAME_MACH3;

	m_video_overlay_width = MACH3_OVERLAY_W;
	m_video_overlay_height = MACH3_OVERLAY_H;
	m_palette_color_count = MACH3_COLOR_COUNT + 1;  // one extra dummy color to 'disable' transparency

	// mack3 LD video starts off disabled
	m_ldvideo_enabled = false;

	//m_game_issues = "This game doesn't wook at all yet.";

	// We don't use samples anymore, and I believe that the use of .ogg samples
	//  was causing at least one person's PC to crash (Leslie's)
	/*
	m_num_sounds = 49;
	m_sound_name[1] = "mach3-01.ogg";
	m_sound_name[2] = "mach3-02.ogg";
	m_sound_name[3] = "mach3-03.ogg";
	m_sound_name[4] = "mach3-04.ogg";
	m_sound_name[5] = "mach3-05.ogg";
	m_sound_name[6] = "mach3-06.ogg";
	m_sound_name[7] = "mach3-07.ogg";
	m_sound_name[8] = "mach3-08.ogg";
	m_sound_name[9] = "mach3-09.ogg";
	m_sound_name[11] = "mach3-11.ogg";
	m_sound_name[13] = "mach3-13.ogg";
	m_sound_name[15] = "mach3-15.ogg";
	m_sound_name[19] = "mach3-19.ogg";
	m_sound_name[20] = "mach3-20.ogg";
	m_sound_name[22] = "mach3-22.ogg";
	m_sound_name[33] = "mach3-33.ogg";
	m_sound_name[34] = "mach3-34.ogg";
	m_sound_name[35] = "mach3-35.ogg";
	m_sound_name[36] = "mach3-36.ogg";
	m_sound_name[37] = "mach3-37.ogg";
	m_sound_name[39] = "mach3-39.ogg";
	m_sound_name[40] = "mach3-40.ogg";
	m_sound_name[41] = "mach3-41.ogg";
	m_sound_name[42] = "mach3-42.ogg";
	m_sound_name[43] = "mach3-43.ogg";
	m_sound_name[45] = "mach3-45.ogg";
	m_sound_name[49] = "mach3-49.ogg";

	//unused sounds:
	// FIXME: deal with unused sounds more efficiently,
	//        without getting too messy.
	m_sound_name[0] =  "mach3-null.ogg";
	m_sound_name[10] = "mach3-null.ogg";
	m_sound_name[12] = "mach3-null.ogg";
	m_sound_name[14] = "mach3-null.ogg";
	m_sound_name[16] = "mach3-null.ogg";
	m_sound_name[17] = "mach3-null.ogg";
	m_sound_name[18] = "mach3-null.ogg";
	m_sound_name[21] = "mach3-null.ogg";
	m_sound_name[23] = "mach3-null.ogg";
	m_sound_name[24] = "mach3-null.ogg";
	m_sound_name[25] = "mach3-null.ogg";
	m_sound_name[26] = "mach3-null.ogg";
	m_sound_name[27] = "mach3-null.ogg";
	m_sound_name[28] = "mach3-null.ogg";
	m_sound_name[29] = "mach3-null.ogg";
	m_sound_name[30] = "mach3-null.ogg";
	m_sound_name[31] = "mach3-null.ogg";
	m_sound_name[32] = "mach3-null.ogg";
	m_sound_name[38] = "mach3-null.ogg";
	m_sound_name[44] = "mach3-null.ogg";
	m_sound_name[46] = "mach3-null.ogg";
	m_sound_name[47] = "mach3-null.ogg";
	m_sound_name[48] = "mach3-null.ogg";
*/

	m_gamecontrols = 0x00; //joystick and fire buttons, all active high, all cleared

	m_serviceswitches = 0x21;	// all clear (bit 1 is service, bit 5 is tilt, both active low) 
	//	m_serviceswitches = 0x20;	// service switch pressed on startup - runs test mode

	m_dipswitches = 0x4C;		// reasonable defaults (3 lives for 2 credits,  

	m_current_targetdata = 0x00000000;	//pointer to active 'packet' of target data
	m_targetdata_offset = 0x0000;		//counter for current position in target data
	m_audio_ready_bit = 0;				//status register for target data audio decoder

	// selects whether audio or video data is returned from decoder
	// 1 = video frame decoder, 0 = audio decoder data buffer
	// video is the default until a write to the control register (0x5806) changes it
	// cobram3 needs this, otherwise the self-test won't start properly
	m_frame_decoder_select_bit = 1; 

	// tracks simulated searches
	m_signal_loss_counter = 0;

	static struct rom_def g_mach3_roms[] =
	{
		{ "m3rom4.bin", NULL, &m_cpumem[0x6000], 0x2000, 0x8bfd5a44 },
		{ "m3rom3.bin", NULL, &m_cpumem[0x8000], 0x2000, 0xb1b045af },
		{ "m3rom2.bin", NULL, &m_cpumem[0xA000], 0x2000, 0xfbdfb03d },
		{ "m3rom1.bin", NULL, &m_cpumem[0xC000], 0x2000, 0x3b0ba80b },
	    { "m3rom0.bin", NULL, &m_cpumem[0xE000], 0x2000, 0x70c12bf4 },

		{ "m3drom1.bin", NULL, &m_cpumem2[0xF000], 0x1000, 0xa6e29212 },
		{ "m3yrom1.bin", NULL, &m_cpumem3[0xF000], 0x1000, 0xeddf8872 },

		{ "mach3fg3.bin", NULL, &sprite[0x0000], 0x2000, 0x472128b4 },
		{ "mach3fg2.bin", NULL, &sprite[0x4000], 0x2000, 0x2a59e99e },
		{ "mach3fg1.bin", NULL, &sprite[0x8000], 0x2000, 0x9b88767b },
		{ "mach3fg0.bin", NULL, &sprite[0xc000], 0x2000, 0x0bae12a5 }, 
		{ "m3target.bin", NULL, &targetdata[0x00000000], 971776, 0xa2a9f8e5 },
		{ "mach3bg0.bin", NULL, &character[0x0000], 0x1000, 0xea2f5257 },
		{ "mach3bg1.bin", NULL, &character[0x1000], 0x1000, 0xf543e4ce },
		{ NULL }
	};

	m_rom_list = g_mach3_roms;

	m_nvram_begin = &m_cpumem[0x0000];
	m_nvram_size = 0x1000; // battery-backed RAM
}

// Us Vs Them - uvt class constructor
uvt::uvt()
{
	m_shortgamename = "uvt";
	m_game_type = GAME_UVT;	

	// UVT ROMs
	static struct rom_def g_uvt_roms[] =
	{
		{ "usvs.rm4", NULL, &m_cpumem[0x6000], 0x2000, 0x0d7a4072 },
		{ "usvs.rm3", NULL, &m_cpumem[0x8000], 0x2000, 0x6f32a671 },
		{ "usvs.rm2", NULL, &m_cpumem[0xA000], 0x2000, 0x36770716 },
		{ "usvs.rm1", NULL, &m_cpumem[0xC000], 0x2000, 0x697bc989 },
	    { "usvs.rm0", NULL, &m_cpumem[0xE000], 0x2000, 0x30cf6bd9 },

		// no segments for 6502
		{ "usvsdrom.1", NULL, &m_cpumem2[0xE000], 0x2000, 0xc0b5cab0 },
		{ "usvsyrom.1", NULL, &m_cpumem3[0xE000], 0x2000, 0xc3d245ca },

		// sometimes UVT accesses the same memory from the E000:xxxx segment
		// (the mirrored segment code in cpu_mem_read should eliminate the need
		// for these copies, but it isn't doing so...)
		{ "usvs.rm4", NULL, &m_cpumem[0x000E6000], 0x2000, 0x0d7a4072 },  
		{ "usvs.rm3", NULL, &m_cpumem[0x000E8000], 0x2000, 0x6f32a671 },
		{ "usvs.rm2", NULL, &m_cpumem[0x000EA000], 0x2000, 0x36770716 },
		{ "usvs.rm1", NULL, &m_cpumem[0x000EC000], 0x2000, 0x697bc989 },
	    { "usvs.rm0", NULL, &m_cpumem[0x000EE000], 0x2000, 0x30cf6bd9 },

		{ "usvs.fg3", NULL, &sprite[0x0000], 0x4000, 0x98703015 },
		{ "usvs.fg2", NULL, &sprite[0x4000], 0x4000, 0xd3990707 },
		{ "usvs.fg1", NULL, &sprite[0x8000], 0x4000, 0xa2057430 },
		{ "usvs.fg0", NULL, &sprite[0xC000], 0x4000, 0x7734e53f },

		{ "usvs.bg0", NULL, &character[0x0000], 0x1000, 0x8a0de09f },
		{ "usvs.bg1", NULL, &character[0x1000], 0x1000, 0x6fb64d3c },

		{ NULL }
	};

	m_rom_list = g_uvt_roms;

	m_dipswitches=0x00;  // all switches OFF (1 coin per credit, normal difficulty)
}

// Cobra Command (for MACH3 hardware)
cobram3::cobram3()
{
	m_shortgamename = "cobram3";

	// Corba Comnad for Mack3
	static struct rom_def g_cobram3_roms[] =
	{
		{ "bh03", NULL, &m_cpumem[0x8000], 0x2000, 0x755cbbf5 },
		{ "bh02", NULL, &m_cpumem[0xA000], 0x2000, 0x928ef670 },
		{ "bh01", NULL, &m_cpumem[0xC000], 0x2000, 0x7d86ab08 },
		{ "bh00", NULL, &m_cpumem[0xE000], 0x2000, 0xc19ad038 },

		{ "bh05", NULL, &sprite[0x0000], 0x2000, 0xd8f49994 },
		{ "bh08", NULL, &sprite[0x4000], 0x2000, 0xd6439e2f },
		{ "bh07", NULL, &sprite[0x8000], 0x2000, 0xf94668d2 },
		{ "bh06", NULL, &sprite[0xC000], 0x2000, 0xab6c7cf1 },

		{ "bh09", NULL, &character[0x0000], 0x1000, 0x8c5dfac0 },
		{ "bh0a", NULL, &character[0x1000], 0x1000, 0x8b8da8dc },

		{ NULL }
	};

	m_rom_list = g_cobram3_roms;

	m_dipswitches = 0x07;	// reasonable defaults (1 coin 3 lives, regular difficulty, bonuses at 20,000/40,000
	//	m_dipswitches = 0x87;  // dip switch #8 = self-test if enabled
}



///////////////////////////////////////////////////////////

void mach3::do_nmi()
{
	switch (cpu_getactivecpu())
	{
	case 0:
		// update palette if it has been written to
		if (m_palette_updated)
		{
			palette_calculate();
			m_video_overlay_needs_update = true;
			m_palette_updated = false;
		}  

		// update search counter
		if (m_signal_loss_counter > 0)
		{
			m_signal_loss_counter--;	
		}

		// update overlay
		video_blit();

		// Call game CPU NMI routine (IRQ# 127 is NMI)
		i86_set_irq_line(127, CLEAR_LINE);  // clear the line from the previous NMI    
		i86_set_irq_line(127, ASSERT_LINE); // trigger a new NMI

		// update audio target data buffer (only used by MACH3)
		if (m_game_type == GAME_MACH3)
		{
			static Uint16 prev_frame = 0xFFFF;
			Uint16 cur_frame = pr8210_get_current_frame() ;	

			if ((cur_frame != prev_frame) && (((cur_frame) % 53) == 0) ) // target data buffer fills every 53 frames 
			{
				int cur_targetdata_packet = cur_frame / 53;
				if ((cur_targetdata_packet >= 44) ) //there is no audio where the first 44 packets would be 
				{
					m_current_targetdata = (cur_targetdata_packet - 44) * 1024 + 1;  // skip first byte
					m_targetdata_offset = 0x0000;
					m_audio_ready_bit = 1;
//					sound_play(0);
				}
			}

			if ((cur_frame == prev_frame) && (((cur_frame) % 53) == 0) )  // next field
			{	
				m_audio_ready_bit = 0; //delayed clear of the audio ready bit (when 0x67 at start of next buffer would be received)
			}
			prev_frame = cur_frame;
		}
		break;
	case 1:
		nes6502_nmi();
		break;
	case 2:
		if (m_soundchip2_nmi_enabled)
		{
			nes6502_nmi();
		}
		break;
	default:
		break;
	}
}

void mach3::do_irq(unsigned int which)
{
	switch (cpu_getactivecpu())
	{
	case 0: // the i86 doesn't use IRQ
		break;
	case 1:
		nes6502_irq();
		break;
	case 2:
		nes6502_irq();
		break;
	default:
		break;
	}
}

Uint8 mach3::cpu_mem_read(Uint32 addr)
{
	unsigned int which_cpu = cpu_getactivecpu();
	Uint8 result = 0;

	if (which_cpu == 0)
	{
		char s[80];
		Uint16 cur_frame = 0;	// temp variable used by more than one 'case' below, so we define it once here

		if (addr > 0xFFFF) //accessing mirrored memory from different segment (UVT uses E0000 segment)
		{
			addr &= 0x0000FFFF;
		}

		result = m_cpumem[addr];

		if (addr >= 0x6000) //ROM
		{
		}
		else if (addr == 0x5800) //DIP switches
		{
			result = m_dipswitches;
		}
		else if (addr == 0x5801) //service switches
		{
			result = m_serviceswitches; 
		}
		else if (addr == 0x5804) //joystick
		{
			result = m_gamecontrols; 
		}
		else if (addr < 0x3c00) //work and sprite RAM
		{
		}
		else if ((addr < 0x5020) && (addr >= 0x5000))  //palette RAM
		{
		}

		else if (addr == 0x5805) // frame decoder board, lowest byte of three-byte BCD laserdisc frame #
		{
			cur_frame = pr8210_get_current_frame();
			result = cur_frame % 10;	// lower digit
			result = result | (((cur_frame % 100) / 10) << 4);	// upper digit

			// frame #s are only encoded in odd video fields, and shouldn't be present
			// during a search either.  However, this behavior doesn't seem to be needed.
			//		if (m_signal_loss_counter > 0) result = 0; 
		}
		else if (addr == 0x5806) // frame decoder board, middle byte of three-byte BCD laserdisc frame #
		{
			cur_frame = (pr8210_get_current_frame() / 100);
			result = cur_frame % 10;	// lower digit
			result = result | (((cur_frame % 100) / 10) << 4);	// upper digit
		}
		else if (addr == 0x5807) // shared by two i/o registers, selected by value written to 0x5806
		{
			// the value returned depends on value written to 0x5806
			// (1 selects video frame decoder, 0 selects audio decoder buffer
			if (m_frame_decoder_select_bit) 
			{
				// returns top digit of BCD frame # (only 0-7), plus some laserdisc status bits

				//bits 0-2 are the frame decode highest digit (0-7)
				//bit 3 audio data buffer ready 
				//bit 4 LD command controller ready (goes low for ~20ms after every byte sent to laserdisc player)
				//bit 5 disc ready (set by the genlock hardware if LD video signal is active, off if player is stopped or searching)
				//bit 6 audio clock (not needed)
				//bit 7 audio clock lost

				cur_frame = pr8210_get_current_frame();

				result = (Uint8) (cur_frame / 10000); // upper single digit, frame cannot reasonably exceed 16-bit border

				// if the current frame is too small (before audio data begins on disc), or if we're simulating
				//  a loss of signal due to a recent seek
				// (NOTE : a cur_frame of 0 means that PR8210 is busy, so we need not call g_ldp->get_status to see
				//  if the disc is playing or not)
				if ((cur_frame < 53 * 44) || (m_signal_loss_counter > 0))
				{
					// result |= 0x08; // audio ready is set by break in transmission (BOT) detector
					result |= 0x80;  // audio clock lost
				}				
				result |=  m_audio_ready_bit << 3; 

				result |= 0x10 ; // LD command controller ready bit -- we can just set it high all the time

				// if we aren't simulating a seek delay, AND if the disc is playing/paused
				if ((m_signal_loss_counter == 0) && (cur_frame != 0))
				{
					result |= 0x20;  //disc is playing, video is detected
				}
			}
			else // audio buffer (targeting data) is selected
			{
				//if (m_current_targetdata > 0)  // this is always true when the ROM reads this register, so we don't have to check
				result = targetdata[m_current_targetdata + m_targetdata_offset++];  // buffer address is auto-incremented on each read

				if (m_targetdata_offset == 0x03FF)
				{
					m_targetdata_offset = 0;
					// m_audio_ready_bit = 1;  //the hardware sets this bit when address counter hits max, but it should already be set
				}
			}
		}
		else if (addr >= 0x5800 && addr <= 0x5FFF) // mapped i/o
		{
			sprintf(s, "Undefined mapped i/o read from %x", addr);
			printline(s);
		}
		else 
		{
			sprintf(s, "Unmapped read from %x", addr);
			printline(s);
		}
	} // end x86 CPU

	// the cpu debugger goes to this mem read function, so we need to redirect it
	else result = cpu_mem_read(static_cast<Uint16>(addr));
	return (result);
}

void mach3::cpu_mem_write(Uint32 Addr, Uint8 Value)
{
	char s[80];

	if (Addr > 0xFFFF) //accessing mirrored memory from different segment (UVT does this)
	{
		Addr &= 0x0000FFFF;
	}

	if (Addr <= 0x1FFF)  //work RAM
	{
		m_cpumem[Addr] = Value; // store to RAM
	}
	else if (Addr >= 0x3000 && Addr <= 0x3FFF) // character RAM
	{
		if (Value != m_cpumem[Addr]) //skip if no change
		{
			m_cpumem[Addr] = Value; // store to RAM
			m_video_overlay_needs_update = true;
		}
	}
	else if (Addr >= 0x4000 && Addr <= 0x4FFF)
	{
		sprintf(s, "invalid write to character ROM at %x, value %x", Addr, Value);
		printline(s);

		m_cpumem[Addr] = Value; // store to RAM anyway
	}
	else if (Addr >= 0x5000 && Addr <= 0x501F)  // palette RAM
	{
		if (Value != m_cpumem[Addr]) //skip if no change
		{
			m_cpumem[Addr] = Value; // store to RAM
			m_palette_updated = true;  //causes palette recalc and frame redraw
		}
		m_cpumem[Addr] = Value; // store to RAM
	}
	else if (Addr == 0x5800) // watchdog counter register (not implemented)
	{
		//FIXME:  watchdog counter decrement/test/reset could be added to do_nmi()
		m_cpumem[Addr] = Value; // store to RAM
	}
	else if (Addr == 0x5802) // sound board i/o
	{
		/*
		if  (Value != 0)
		{
			sprintf(s, "Write to sound board at %x, value %x", Addr, Value);
			printline(s);
		}
		*/
		//		sound_play(Value);

		m_sounddata_latch1.push(Value & 0x3F);
		cpu_generate_irq(1, 0);	// generate IRQ for cpu #1

		// maybe we should generate IRQ's for 0 being written too?
		if (Value != 0)
		{
			m_sounddata_latch2.push(Value & 0x3F);
			cpu_generate_irq(2, 0);	// generate IRQ for cpu #2
		}
		m_cpumem[Addr] = Value; // store to RAM
	}
	else if (Addr == 0x5803) // video control
	{
		//		sprintf(s, "Write to video control register at %x, value %x", Addr, Value);
		//		printline(s);

		//bit 0 - foreground/background (sprite/character) priority reversal
		//bit 1 - sprite bank select (for Us Vs Them)
		//bit 2 - enable graphics (blank screen if 0)
		//bit 3 - enable graphics overlay (genlock to LD video)
		if ((Value & 0x08) != (m_cpumem[0x5803] & 0x08)) //genlock bit has changed
		{
			// whether laserdisc video can be seen
			m_ldvideo_enabled = ((Value & 8) == 8);
			palette_set_transparency(0, m_ldvideo_enabled);
		}
		if ((Value & 0x04) != (m_cpumem[0x5803] & 0x04)) //enable display bit has changed
		{
			m_video_overlay_needs_update = true; // we need to redraw when this bit changes
		}
		m_cpumem[Addr] = Value; // store to RAM to compare next time
	}
	else if (Addr == 0x5805)	// 0x5805 is ld command register 
	{
		//	sprintf(s, "Write to LD command register at %x, value %x", Addr, Value);
		//	printline(s);

		// commands are written twice, followed by a zero (which is EOC - "End of Command")
		// (other PR8210 games like Cliffy do similar things)

		unsigned int blips = ((unsigned int) Value) << 2;		// has proper layout aside from the trailing two 0 bits
		//m_cpumem[Addr] = Value; // store to RAM (not used)

		pr8210_command(blips);	// this has the proper layout aside from the trailing two 0 bits

		// Andrew Hepburn says that the PR-8210 shuts off the video signal temporarily whilst it seeks
		// See http://www.dragons-lair-project.com/tech/command/command8210.asp
		// We simulate this behavior here
		// FIXME : we may want to move this into pr8210.cpp some day although Cliff Hanger doesn't need it

		// if this is a seek command, then we need to fool ROM into thinking the video signal has been disabled
		if (((blips & 0x7C) >> 2) == 0x1A)
		{
			m_signal_loss_counter = MACH3_SEARCH_PERIOD;	// see comments in mach3.h
		}

		// 0x24 is a pause command, which will trigger the audio break-in-transission (BOT) detector
		// (all commands are sent twice, so the repeat must be ignored)
		// BOT causes the audio buffer to reset
		static Uint8 prev_cmd = 0;
		if ((Value == 0x24) && (prev_cmd != 0x24) && (pr8210_get_current_frame() > 53 * 44))
		{
			m_audio_ready_bit = 1;
			m_targetdata_offset = 0;
		}
		prev_cmd=Value;
	}
	else if (Addr == 0x5806)	// 0x5806 is ld audio/status register toggle
	{
		//	sprintf(s, "Write to decode select register at %x, value %x", Addr, Value);
		//	printline(s);

		// m_cpumem[Addr] = Value; // store to RAM (not needed, we are using a separate variable, and value is only used
		//                            by our own cpu_mem_read() )
		m_frame_decoder_select_bit = Value & 0x01;
	}
	else if (Addr >= 0x5800 && Addr <= 0x5FFF)
	{
		sprintf(s, "Undefined write to memory-mapped i/o device at %x, value %x", Addr, Value);
		printline(s);

		//m_cpumem[Addr] = Value; // store to RAM

	}
	else if (Addr >= 0x6000 && Addr <= 0xFFFF)
	{
		//	sprintf(s, "Ignoring write to ROM at %x, value %x", Addr, Value);
		//	printline(s);
	}
	else
	{		
		sprintf(s, "Unmapped write to %x, value %x", Addr, Value);
		printline(s);
		//m_cpumem[Addr] = Value; // store to RAM
	}
}

// only the 6502's use this read handler since the I86 uses 20 bit addressing
Uint8 mach3::cpu_mem_read(Uint16 addr)
{
//	char s[80];	
	Uint8 result = 0;

	switch (cpu_getactivecpu())
	{
	case 1:
		{   		
			result = m_cpumem2[addr];

			// ram
			if (addr <= 0x3fff)
			{
			}
			// sound data
			else if (addr == 0x8000)
			{
				if (!m_sounddata_latch1.empty())
				{
					result = m_sounddata_latch1.front();
					m_sounddata_latch1.pop();
				}
				// we may want to know about a lot of these
				else printline("MACH3 NOTE: CPU #1 queried 0x8000 even though nothing is available");
			}
			// main rom (mack 3 uses f000, uvt uses e000)
			else if (addr >= 0xe000)
			{
			}
			else
			{
//				sprintf(s, "CPU 1: Unmapped read from %x", addr);
//				printline(s);
			}
		}      
		break;
	case 2:
		{
			result = m_cpumem3[addr];

			// ram
			if (addr <= 0x3fff)
			{
			}
			else if (addr == 0x6000) result = 0xC0;
			// sound data
			else if (addr == 0xa800)
			{
				if (!m_sounddata_latch2.empty())
				{
					result = m_sounddata_latch2.front();
//					if (result != 0xFF) set_cpu_trace(1);
					m_sounddata_latch2.pop();
				}
				// we may want to know about a lot of these
				else printline("MACH3 NOTE: CPU #2 queried 0xA800 when no data was present");
			}
			// main rom
			else if (addr >= 0xf000)
			{
			}
			else
			{
//				sprintf(s, "CPU 2: Unmapped read from %x", addr);
//				printline(s);
			}
		}      
		break;
	default:
		break;
	}   
	return (result);
}

// only the 6502's use this write handler since the I86 uses 20 bit addressing
void mach3::cpu_mem_write(Uint16 Addr, Uint8 Value)
{
//	char s[80];

	switch (cpu_getactivecpu())
	{
	case 1:
		// Dac control ?
		if (Addr == 0x4000)
		{
			// this seems to be either 0xFF or 0x2F, no idea what it means
//			sprintf(s, "4000: 0x%x", Value);
//			printline(s);
		}
		// Dac data ?
		else if (Addr == 0x4001)
		{
//			sprintf(s, "4001: 0x%x", Value);
//			printline(s);

			// if it's time to re-calculate
			if (Value != m_dac_last_val)
			{
				Uint64 u64CurCycs = get_total_cycles_executed(1);
				unsigned int uDiff = (unsigned int) (u64CurCycs - m_dac_last_cycs);
				m_dac_last_cycs = u64CurCycs;

				audio_write_ctrl_data(uDiff , Value, m_dac_id);
				m_dac_last_val = Value;
			}
		}
		else
		{		
			/*
			sprintf(s, "CPU 1: Unmapped write to %x, value %x", Addr, Value);
			printline(s);
			*/
			m_cpumem2[Addr] = Value; // store to RAM
		}
		break;

	case 2:
		switch (Addr)
		{
		case 0x2000:
			break;
		case 0x4000:
			{
				m_soundchip2_nmi_enabled = (Value & 1);

				/* bit 2 goes to 8913 BDIR pin  */
				if ((m_last0x4000 & 0x04) == 0x04 && (Value & 0x04) == 0x00)
				{
					/* bit 3 selects which of the two 8913 to enable */
					if (Value & 0x08)
					{
						/* bit 4 goes to the 8913 BC1 pin */
						if (Value & 0x10) m_soundctrl1 = m_psg_latch;
						else 
						{
							/*
							sprintf(s, "AY8910 1: 0x%x->0x%x", m_soundctrl1, m_psg_latch);
							printline(s);
							*/
							audio_write_ctrl_data(m_soundctrl1, m_psg_latch, m_soundchip1_id);
						}
					}
					else
					{
						/* bit 4 goes to the 8913 BC1 pin */
						if (Value & 0x10) m_soundctrl2 = m_psg_latch;
						else
						{
							/*
							sprintf(s, "AY8910 2: 0x%x->0x%x", m_soundctrl2, m_psg_latch);
							printline(s);
							*/
							audio_write_ctrl_data(m_soundctrl2, m_psg_latch, m_soundchip2_id);
						}
					}
				}

				m_last0x4000 = Value & 0x44;
			}
			break;
		case 0x8000:
			m_psg_latch = Value;
			break;
		case 0xA000:
			cpu_change_nmi(2, 1000.0 / (250000.0/256/(256-Value)));
			break;
		case 0xB000:
			cpu_generate_nmi(1);	// CPU #2 generates NMI's for CPU #1 by writing to this spot
			break;
		default:
//			sprintf(s, "CPU 2: Unmapped write to %x, value %x", Addr, Value);
//			printline(s);
			break;
		}

		m_cpumem3[Addr] = Value; // store to RAM

		break;
	default:
		break;
	}
}

// mack3 hardware doesn't appear to use ports
void mach3::port_write(Uint16 port, Uint8 value)
{
	char s[80];

	sprintf(s, "Unmapped write to port %x, value %x", port, value);
	printline(s);
}

// mack3 hardware doesn't appear to use ports

Uint8 mach3::port_read(Uint16 port)
{
	char s[80];

	unsigned char result = 0;

	sprintf(s, "Unmapped read from port %x", port);
	printline(s);
	return (result);
}


// used to set dip switch values
bool mach3::set_bank(unsigned char which_bank, unsigned char value)
{
	bool result = true;

	if (which_bank == 0) 
	{
		// incoming bits need to be rearranged
		// apparently the MACH3 hardware doesn't map them in order
		// bit 0 = switch 2
		// bit 1 = switch 6
		// bit 2 = switch 4
		// bit 3 = switch 1
		// bit 4 = switch 3
		// bit 5 = switch 5
		// bit 6 = switch 7
		// bit 7 = switch 8

		m_dipswitches = (value & 0xC0) + ((value & 0x01) << 3) +
			((value & 0x02) >> 1) + ((value & 0x04) << 2) + ((value & 0x08) >> 1) +
			((value & 0x10) << 1) + ((value & 0x20) >> 4);
	}
	else
	{
		printline("ERROR: Bank specified is out of range!");
		result = false;
	}

	return result;
}

void mach3::input_disable(Uint8 move)
{
	switch(move)
	{
	case SWITCH_UP:
		m_gamecontrols &= (unsigned char) ~0x01;	// clear bit 0
		break;
	case SWITCH_DOWN:
		m_gamecontrols &= (unsigned char) ~0x02;	// clear bit 1
		break;
	case SWITCH_LEFT:
		m_gamecontrols &= (unsigned char) ~0x04;	// clear bit 2
		break;
	case SWITCH_RIGHT:
		m_gamecontrols &= (unsigned char) ~0x08;	// clear bit 3
		break;
	case SWITCH_START1: // PLAYER 1
		m_serviceswitches &= ~0x40;	// clear bit 6 ...
		break;
	case SWITCH_START2:
		m_serviceswitches &= ~0x80;	// clear bit 7
		break;
	case SWITCH_BUTTON1: 
		m_gamecontrols &= (unsigned char) ~0x20;	// clear bit 5
		break;
	case SWITCH_BUTTON2: 
		m_gamecontrols &= (unsigned char) ~0x40;	// clear bit 6
		break;
	case SWITCH_BUTTON3: 
		m_gamecontrols &= (unsigned char) ~0x10;	// clear bit 6
		break;
	case SWITCH_COIN1: 
		m_serviceswitches &= (unsigned char) ~0x08;	
		break;
	case SWITCH_COIN2: 
		m_serviceswitches &= (unsigned char) ~0x04;	
		break;
	case SWITCH_SERVICE: //active low
		if (!(m_serviceswitches & 0x01))
      {
         m_serviceswitches |= 0x01; // turn off test mode
      }
      else
      {
         m_serviceswitches &= ~0x01; // turn on test mode
         reset();
      }
		break;
	case SWITCH_TEST: //active high
		m_serviceswitches &= (unsigned char) ~0x02;	
		break;
		//	case SWITCH_BUTTON3: // slam/tilt active low, but switch is normally closed
		//		m_serviceswitches |= 0x20;	
		//		break;

	default:
		break;
	}
}

void mach3::input_enable(Uint8 move)
{
	// FIXME:   don't allow up/down or left/right to be pressed at the same time
	//   (MACH3 reacts badly to this -- displays the wrong sprites for the plane)

	switch(move)
	{
	case SWITCH_UP:
		m_gamecontrols |= 0x01;	// set bit 0
		break;
	case SWITCH_DOWN:
		m_gamecontrols |= 0x02;	// set bit 1
		break;
	case SWITCH_LEFT:
		m_gamecontrols |= 0x04;	// set bit 2
		break;
	case SWITCH_RIGHT:
		m_gamecontrols |= 0x08;	// set bit 3
		break;
	case SWITCH_START1:
		m_serviceswitches |= 0x40; // set bit 6
		break;
	case SWITCH_START2:
		m_serviceswitches |= 0x80;	// set bit 7
		break;
	case SWITCH_BUTTON1: 
		m_gamecontrols |= 0x20;	// set bit 5
		break;
	case SWITCH_BUTTON2: 
		m_gamecontrols |= 0x40;	// set bit 6
		break;
	case SWITCH_BUTTON3: 
		m_gamecontrols |= 0x10;	// set bit 6
		break;
	case SWITCH_COIN1: 
		m_serviceswitches |= 0x08;	
		break;
	case SWITCH_COIN2: 
		m_serviceswitches |= 0x04;	
		break;
	case SWITCH_SERVICE: //active low
		//m_serviceswitches &= (unsigned char) ~0x01;	
		break;
	case SWITCH_TEST: 
		m_serviceswitches |= 0x02;	
		break;
		//	case SWITCH_BUTTON3: // slam/tilt, active low
		//		m_serviceswitches &= (unsigned char) ~0x20;	
		//		break;
	default:
		break;
	}
}


void mach3::palette_calculate()
{
	// palette is stored in ram from 0x5000-0x501F
	// uses 16 simultaneous colors
	// each color is a two-byte value, bits 0-3 blue, 4-7 green, 8-11 red, 12-15 unused

	// As with most games of this time, resistors are used as DACs for the RGB monitor outputs
	// For each 4-bit color value, bit 0 = 240 ohm, bit 1 = 470, bit 2 = 1K, bit 3 = 2K

	// This is roughly a doubling for each bit, so the colors will be close if we
	// just push the 4-bit palette values into the top 4 bits of the 8-bit rgb output values

	// a more accurate method is to extract the individual bits, weight the values, and sum them:
	// color = (bit0 * 0x10) + (bit1 * 0x21)+ (bit2 * 0x47)+ (bit3 * 0x87)
	// (the difference is very small, but it does boost some of the darker colors)

	SDL_Color temp_color;

	// this cannot be done in the constructor, so it must be done here
	palette_set_transparency(0, m_ldvideo_enabled);

	//Convert palette rom into a useable palette
	for (int i = 0; i < MACH3_COLOR_COUNT; i++)
	{
		Uint8 bit0, bit1, bit2, bit3;
		Uint8 paletteval;

		paletteval = (m_cpumem[0x5000 + i * 2]);

		/* blue component */
		bit0 = static_cast<Uint8>((paletteval >> 0) & 0x01);
		bit1 = static_cast<Uint8>((paletteval >> 1) & 0x01);
		bit2 = static_cast<Uint8>((paletteval >> 2) & 0x01); 
		bit3 = static_cast<Uint8>((paletteval >> 3) & 0x01); 
		temp_color.b = static_cast<Uint8>((0x10 * bit0) + (0x21 * bit1) + (0x47 * bit2) + (0x87 * bit3));

		/* green component */
		bit0 = static_cast<Uint8>((paletteval >> 4) & 0x01);
		bit1 = static_cast<Uint8>((paletteval >> 5) & 0x01);
		bit2 = static_cast<Uint8>((paletteval >> 6) & 0x01); 
		bit3 = static_cast<Uint8>((paletteval >> 7) & 0x01); 
		temp_color.g = static_cast<Uint8>((0x10 * bit0) + (0x21 * bit1) + (0x47 * bit2) + (0x87 * bit3));

		paletteval = (m_cpumem[0x5001 + i * 2]);

		/* red component */
		bit0 = static_cast<Uint8>((paletteval >> 0) & 0x01);
		bit1 = static_cast<Uint8>((paletteval >> 1) & 0x01);
		bit2 = static_cast<Uint8>((paletteval >> 2) & 0x01); 
		bit3 = static_cast<Uint8>((paletteval >> 3) & 0x01); 
		temp_color.r = static_cast<Uint8>((0x10 * bit0) + (0x21 * bit1) + (0x47 * bit2) + (0x87 * bit3));

		//alternatively, we could just shift the four bits into the upper half of the 8-bit output color value:
		//temp_color.b = static_cast<Uint8>((m_cpumem[0x5000 + i * 2] & 0x0F) << 4);
		//temp_color.g = static_cast<Uint8>((m_cpumem[0x5000 + i * 2] & 0xF0));
		//temp_color.r = static_cast<Uint8>((m_cpumem[0x5001 + i * 2] & 0x0F) << 4);

		////apply gamma correction to make colors more or less bright overall
		//Corrected value = 255 * (uncorrected value / 255) ^ (1.0 / gamma)
		//temp_color.r = (Uint8) (255 * pow((static_cast<double>(temp_color.r)) / 255, 1/MACH3_GAMMA));
		//temp_color.g = (Uint8) (255 * pow((static_cast<double>(temp_color.g)) / 255, 1/MACH3_GAMMA));
		//temp_color.b = (Uint8) (255 * pow((static_cast<double>(temp_color.b)) / 255, 1/MACH3_GAMMA));

		palette_set_color(i, temp_color);
	}

	palette_finalize();	

}

void mach3::video_repaint()
{

	//fast screen clear
	SDL_FillRect(m_video_overlay[m_active_video_overlay], NULL, 0);

	// graphics are blanked when this bit is cleared
	// FIXME:  LD video should be blanked as well (screen should be all black)
	if ((m_cpumem[0x5803] & 0x04) != 0x04) return;  

	if (m_cpumem[0x5803] & 0x01) // bit 1 set = priority reversed
	{
		draw_sprites();
		draw_characters();
	}
	else
	{
		draw_characters();
		draw_sprites();
	}
}

void mach3::draw_characters()
{
	// 8x8 pixel characters, 32 columns (256 pixels) x 30 rows (240 pixels)
	for (int charx = 0; charx < 32; charx++) 
	{
		for (int chary = 0; chary < 30; chary++) 
		{
			// draw 8x8 tiles from character generator 
			int current_character = m_cpumem[chary * 32 + charx + 0x3800];
			draw_8x8(current_character, character, charx*8, chary*8);
		}
	}
}

void mach3::draw_sprites()
{
	Uint16 offset = 0x0000;  //uvt has two banks

	if ((m_cpumem[0x5803] & 0x02))  //bank select bit
	{
		offset = 0x2000;
	}

	Uint8 * spritebank = (Uint8 *)&sprite[offset];

	//docs say 63 sprites, each 16x16
	for (int spritenum = 0; spritenum < 62; spritenum++)  
	{
		Uint8 *pSpriteInfo = &m_cpumem[0x3000 + spritenum * 4];

		// force little-endian memory load
		unsigned int uSpriteInfo = LOAD_LIL_UINT32(pSpriteInfo);

		//don't draw this sprite if it is not in use (all zero bytes)
		if (uSpriteInfo != 0x00000000) 
		{
			// sprite data contains:
			// y coordinate
			// x coordinate
			// character number  //256 characters available for sprites (in each bank)
			// unknown (priority? - manual has a vague reference to having 63 planes/priorities)

			Uint8 ypos = static_cast<Uint8>((uSpriteInfo & 0x000000FF) >> 0);
			Uint8 xpos = static_cast<Uint8>((uSpriteInfo & 0x0000FF00) >> 8);
			//WDO: not sure why characters need to be accessed in reverse order
			Uint8 current_character = 255 - static_cast<Uint8>((uSpriteInfo & 0x00FF0000) >> 16);
			draw_16x16(current_character, spritebank, xpos, ypos);
		}
	}  

	//hack to show all sprites
	/*	static Uint8 snum = 0;
	for (int x = 0; x < 256; x+=16)
	{
	for (int y = 0; y < 256; y+=16)
	{
	draw_16x16(snum++, spritebank, x, y);
	}
	} */
}

void mach3::draw_8x8(Uint8 character_number, Uint8 *character_set, Uint8 xcoord, Uint8 ycoord)
{
	Uint8 pixel[8] = {0};

	//	static Uint8 tmpchar = 0;  // test hack to show the whole character set
	//  character_number =  tmpchar++;

	for (int y = 0; y < 8; y++)
	{
		//characters are contiguous blocks of 4-bpp values (32 bytes total for each 8x8 char)
		pixel[0] = static_cast<Uint8>((character_set[(character_number * 32) + 0 + (y * 4)] & 0xF0) >> 4  );
		pixel[1] = static_cast<Uint8>((character_set[(character_number * 32) + 0 + (y * 4)] & 0x0F) >> 0  );
		pixel[2] = static_cast<Uint8>((character_set[(character_number * 32) + 1 + (y * 4)] & 0xF0) >> 4  );
		pixel[3] = static_cast<Uint8>((character_set[(character_number * 32) + 1 + (y * 4)] & 0x0F) >> 0  );
		pixel[4] = static_cast<Uint8>((character_set[(character_number * 32) + 2 + (y * 4)] & 0xF0) >> 4  );
		pixel[5] = static_cast<Uint8>((character_set[(character_number * 32) + 2 + (y * 4)] & 0x0F) >> 0  );
		pixel[6] = static_cast<Uint8>((character_set[(character_number * 32) + 3 + (y * 4)] & 0xF0) >> 4  );
		pixel[7] = static_cast<Uint8>((character_set[(character_number * 32) + 3 + (y * 4)] & 0x0F) >> 0  );

		for (int x = 0; x < 8; x++)
		{
			if (pixel[x])
			{
				*((Uint8 *) m_video_overlay[m_active_video_overlay]->pixels + ((ycoord + y) * MACH3_OVERLAY_W) + (xcoord + x)) = pixel[x];
			}
		}
	}
}

void mach3::draw_16x16(Uint8 character_number, Uint8 *character_set, Uint8 xpos, Uint8 ypos)
{
	Uint8 pixel[16] = {0};

	Uint8 xmin = 0, xmax = 16,  ymin = 0, ymax = 16;

	int ycoord = ypos - 13;   // sprites are offset from tiles (so they can be partially off-screen)
	int xcoord = xpos - 4;	// used cobram3 ROM to align - cockpit has tiles and sprites that should line up


	if (xcoord < 0)
	{
		xmin = -xcoord;
	}
	if (xcoord > MACH3_OVERLAY_W - 16)
	{
		if (xcoord >= MACH3_OVERLAY_W)
		{
			xmax = xmin; // don't draw at all if it's somewhere completely off the screen
		}
		else
		{
			xmax = MACH3_OVERLAY_W - xcoord;  //draw the visible portion
		}
	}

	if (ycoord < 0)
	{
		ymin = -ycoord;
	}
	if (ycoord > MACH3_OVERLAY_H - 16)
	{
		if (ycoord >= MACH3_OVERLAY_H)
		{
			ymax = ymin; // don't draw at all if it's somewhere completely off the screen
		}
		else
		{
			ymax = MACH3_OVERLAY_H - ycoord;  //draw the visible portion
		}
	}

	for (int y = ymin; y < ymax; y++)
	{

		Uint8 *current_line = &character_set[character_number * 32 + (y * 2)];

		//characters are in blocks of 16-pixel lines x 16 rows, across 4 bitplanes (32 bytes in each bitplane for each 16x16 char)
		for (int i = 7; i >= 0; i--)
		{
			pixel[15 - (i + 8)] = static_cast<Uint8>(
				(((*(current_line + 0 + 0x0000) >> (i) ) & 0x01) << 3) +
				(((*(current_line + 0 + 0x4000) >> (i) ) & 0x01) << 2) +
				(((*(current_line + 0 + 0x8000) >> (i) ) & 0x01) << 1) +
				(((*(current_line + 0 + 0xC000) >> (i) ) & 0x01) << 0));

			pixel[15 - i] = static_cast<Uint8>(
				(((*(current_line + 1 + 0x0000) >> (i) ) & 0x01) << 3) +
				(((*(current_line + 1 + 0x4000) >> (i) ) & 0x01) << 2) +
				(((*(current_line + 1 + 0x8000) >> (i) ) & 0x01) << 1) +
				(((*(current_line + 1 + 0xC000) >> (i) ) & 0x01) << 0));
		}	

		for (int x = xmin; x < xmax; x++)
		{
			if (pixel[x])
			{
				*((Uint8 *) m_video_overlay[m_active_video_overlay]->pixels + ((ycoord + y) * MACH3_OVERLAY_W) + (xcoord + x)) = pixel[x];
			}
		}
	}
}

// to help with debugging
void mach3::patch_roms()
{
	// if this is not UvT or CobraM3
	if (m_game_type == GAME_MACH3)
	{
		// this modification does not work
		/*
		m_cpumem2[0xF41C] = 0xA9;	// LDA
		m_cpumem2[0xF41D] = 0x11;	// this is the sound to be played
		m_cpumem2[0xF41e] = 0x4c;	// JUMP
		m_cpumem2[0xf41f] = 0x7C;	// F47C is the routine to play the sound in A
		m_cpumem2[0xf420] = 0xf4;
		m_cpumem2[0xf421] = 0xEA;	// NOP
		m_cpumem2[0xf422] = 0xEA;	// NOP
		*/
	}

}

void cobram3::patch_roms()
{
	if (m_cheat_requested)
	{
		m_cpumem[0xBB8F] = 0xB0;
		// Replace instruction sbb al,1 (2C 01) with
		// mov al,1 (B0 01)
		printline("CobraM3 infinite lives cheat enabled!");
	}
} 
