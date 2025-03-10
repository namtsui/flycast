/*
    Copyright 2021 flyinghead

    This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "emulator.h"
#include "types.h"
#include "stdclass.h"
#include "cfg/option.h"
#include "hw/aica/aica_if.h"
#include "imgread/common.h"
#include "hw/naomi/naomi_cart.h"
#include "reios/reios.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/holly/sb_mem.h"
#include "cheats.h"
#include "oslib/audiostream.h"
#include "debug/gdb_server.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/arm7/arm7_rec.h"
#include "network/ggpo.h"
#include "hw/mem/mem_watch.h"
#include "network/net_handshake.h"
#include "rend/gui.h"
#include "network/naomi_network.h"
#include "serialize.h"
#include "hw/pvr/pvr.h"
#include "profiler/fc_profiler.h"
#include <chrono>

settings_t settings;

static void loadSpecialSettings()
{
	std::string& prod_id = settings.content.gameId;
	NOTICE_LOG(BOOT, "Game ID is [%s]", prod_id.c_str());

	if (settings.platform.isConsole())
	{
		if (ip_meta.isWindowsCE() || config::ForceWindowsCE
				|| prod_id == "T26702N") // PBA Tour Bowling 2001
		{
			INFO_LOG(BOOT, "Enabling Full MMU and Extra depth scaling for Windows CE game");
			config::ExtraDepthScale.override(0.1f); // taxi 2 needs 0.01 for FMV (amd, per-tri)
			config::FullMMU.override(true);
			if (!config::ForceWindowsCE)
				config::ForceWindowsCE.override(true);
		}

		// Tony Hawk's Pro Skater 2
		if (prod_id == "T13008D 05" || prod_id == "T13006N"
				// Tony Hawk's Pro Skater 1
				|| prod_id == "T40205N"
				// Tony Hawk's Skateboarding
				|| prod_id == "T40204D 50"
				// Skies of Arcadia
				|| prod_id == "MK-51052"
				// Eternal Arcadia (JP)
				|| prod_id == "HDR-0076"
				// Flag to Flag (US)
				|| prod_id == "MK-51007"
				// Super Speed Racing (JP)
				|| prod_id == "HDR-0013"
				// Yu Suzuki Game Works Vol. 1
				|| prod_id == "6108099"
				// L.O.L
				|| prod_id == "T2106M"
				// Miss Moonlight
				|| prod_id == "T18702M"
				// Tom Clancy's Rainbow Six (US)
				|| prod_id == "T40401N"
				// Tom Clancy's Rainbow Six incl. Eagle Watch Missions (EU)
				|| prod_id == "T-45001D05"
				// Jet Grind Radio (US)
				|| prod_id == "MK-51058"
				// JSR (JP)
				|| prod_id == "HDR-0078"
				// JSR (EU)
				|| prod_id == "MK-5105850"
				// Worms World Party
				|| prod_id == "T7016D  50")
		{
			INFO_LOG(BOOT, "Enabling RTT Copy to VRAM for game %s", prod_id.c_str());
			config::RenderToTextureBuffer.override(true);
		}
		if (prod_id == "HDR-0176" || prod_id == "RDC-0057")
		{
			INFO_LOG(BOOT, "Enabling translucent depth multipass for game %s", prod_id.c_str());
			// Cosmic Smash
			config::TranslucentPolygonDepthMask.override(true);
		}
		// NHL 2K2
		if (prod_id == "MK-51182")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(1000000.f);	// Mali needs 1M, 10K is enough for others
		}
		// Re-Volt (US, EU)
		else if (prod_id == "T-8109N" || prod_id == "T8107D  50")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(100.f);
		}
		// Samurai Shodown 6 dc port
		else if (prod_id == "T0002M")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(1e26f);
		}
		// Test Drive V-Rally
		else if (prod_id == "T15110N" || prod_id == "T15105D 50")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(0.1f);
		}
		// South Park Rally
		else if (prod_id == "T-8116N" || prod_id == "T-8112D-50")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(1000.f);
		}

		std::string areas(ip_meta.area_symbols, sizeof(ip_meta.area_symbols));
		bool region_usa = areas.find('U') != std::string::npos;
		bool region_eu = areas.find('E') != std::string::npos;
		bool region_japan = areas.find('J') != std::string::npos;
		if (region_usa || region_eu || region_japan)
		{
			switch (config::Region)
			{
			case 0: // Japan
				if (!region_japan)
				{
					NOTICE_LOG(BOOT, "Japan region not supported. Using %s instead", region_usa ? "USA" : "Europe");
					config::Region.override(region_usa ? 1 : 2);
				}
				break;
			case 1: // USA
				if (!region_usa)
				{
					NOTICE_LOG(BOOT, "USA region not supported. Using %s instead", region_eu ? "Europe" : "Japan");
					config::Region.override(region_eu ? 2 : 0);
				}
				break;
			case 2: // Europe
				if (!region_eu)
				{
					NOTICE_LOG(BOOT, "Europe region not supported. Using %s instead", region_usa ? "USA" : "Japan");
					config::Region.override(region_usa ? 1 : 0);
				}
				break;
			case 3: // Default
				if (region_usa)
					config::Region.override(1);
				else if (region_eu)
					config::Region.override(2);
				else
					config::Region.override(0);
				break;
			}
		}
		else
			WARN_LOG(BOOT, "No region specified in IP.BIN");
		if (config::Cable <= 1 && !ip_meta.supportsVGA())
		{
			NOTICE_LOG(BOOT, "Game doesn't support VGA. Using TV Composite instead");
			config::Cable.override(3);
		}
		if (config::Cable == 2 &&
				(prod_id == "T40602N"	 // Centipede
				|| prod_id == "T9710N"   // Gauntlet Legends (US)
				|| prod_id == "MK-51152" // World Series Baseball 2K2
				|| prod_id == "T-9701N"	 // Mortal Kombat Gold (US)
				|| prod_id == "T1203N"	 // Street Fighter Alpha 3 (US)
				|| prod_id == "T1203M"	 // Street Fighter Zero 3 (JP)
				|| prod_id == "T13002N"	 // Vigilante 8 (US)
				|| prod_id == "T13003N"	 // Toy Story 2 (US)
				|| prod_id == "T1209N"	 // Gigawing (US)
				|| prod_id == "T1208M"	 // Gigawing (JP)
				|| prod_id == "T1235M"   // Vampire Chronicle for Matching Service
				|| prod_id == "T22901N"))// Roadsters (US)
		{
			NOTICE_LOG(BOOT, "Game doesn't support RGB. Using TV Composite instead");
			config::Cable.override(3);
		}
		if (prod_id == "T7001D  50"	// Jimmy White's 2 Cueball
			|| prod_id == "T40505D 50"	// Railroad Tycoon 2 (EU)
			|| prod_id == "T18702M"		// Miss Moonlight
			|| prod_id == "T0019M"		// KenJu Atomiswave DC Conversion
			|| prod_id == "T0020M"		// Force Five Atomiswave DC Conversion
			|| prod_id == "HDR-0187"	// Fushigi no Dungeon - Fuurai no Shiren Gaiden - Onna Kenshi Asuka Kenzan!
			|| prod_id == "T15104D 50") // Slave Zero (PAL)
		{
			NOTICE_LOG(BOOT, "Forcing real BIOS");
			config::UseReios.override(false);
		}
		if (prod_id == "T-9707N"		// San Francisco Rush 2049 (US)
			|| prod_id == "MK-51146"	// Sega Smash Pack - Volume 1
			|| prod_id == "T-9702D-50"	// Hydro Thunder (PAL)
			|| prod_id == "T41601N")	// Elemental Gimmick Gear (US)
		{
			NOTICE_LOG(BOOT, "Forcing NTSC broadcasting");
			config::Broadcast.override(0);
		}
		else if (prod_id == "T-9709D-50")	// San Francisco Rush 2049 (EU)
		{
			NOTICE_LOG(BOOT, "Forcing PAL broadcasting");
			config::Broadcast.override(1);
		}
		if (prod_id == "T1102M"				// Densha de Go! 2
				|| prod_id == "T00000A"		// The Ring of the Nibelungen (demo, hack)
				|| prod_id == "T15124N 00")	// Worms Pinball (prototype)
		{
			NOTICE_LOG(BOOT, "Forcing Full Framebuffer Emulation");
			config::EmulateFramebuffer.override(true);
		}
	}
	else if (settings.platform.isArcade())
	{
		if (prod_id == "SAMURAI SPIRITS 6")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(1e26f);
		}
		if (prod_id == "COSMIC SMASH IN JAPAN")
		{
			INFO_LOG(BOOT, "Enabling translucent depth multipass for game %s", prod_id.c_str());
			config::TranslucentPolygonDepthMask.override(true);
		}
		if (prod_id == "BEACH SPIKERS JAPAN")
		{
			INFO_LOG(BOOT, "Enabling RTT Copy to VRAM for game %s", prod_id.c_str());
			config::RenderToTextureBuffer.override(true);
		}
		if (prod_id == "RADIRGY NOA")
		{
			INFO_LOG(BOOT, "Disabling Free Play for game %s", prod_id.c_str());
			config::ForceFreePlay.override(false);
		}
		// Input configuration
		settings.input.JammaSetup = JVS::Default;
		if (prod_id == "DYNAMIC GOLF"
				|| prod_id == "SHOOTOUT POOL"
				|| prod_id == "SHOOTOUT POOL MEDAL"
				|| prod_id == "CRACKIN'DJ  ver JAPAN"
				|| prod_id == "CRACKIN'DJ PART2  ver JAPAN"
				|| prod_id == "KICK '4' CASH"
				|| prod_id == "DRIVE")			// Waiwai drive
		{
			INFO_LOG(BOOT, "Enabling JVS rotary encoders for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::RotaryEncoders;
		}
		else if (prod_id == "POWER STONE 2 JAPAN"		// Naomi
				|| prod_id == "GUILTY GEAR isuka")		// AW
		{
			INFO_LOG(BOOT, "Enabling 4-player setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::FourPlayers;
		}
		else if (prod_id == "SEGA MARINE FISHING JAPAN"
					|| prod_id == "BASS FISHING SIMULATOR VER.A")	// AW
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::SegaMarineFishing;
		}
		else if (prod_id == "RINGOUT 4X4 JAPAN"
				|| prod_id == "VIRTUA ATHLETE")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::DualIOBoards4P;
		}
		else if (prod_id == "NINJA ASSAULT"
					|| prod_id == "Sports Shooting USA"	// AW
					|| prod_id == "SEGA CLAY CHALLENGE"	// AW
					|| prod_id == "RANGER MISSION"		// AW
					|| prod_id == "EXTREME HUNTING"		// AW
					|| prod_id == "Fixed BOOT strapper")// Extreme hunting 2 (AW)
		{
			INFO_LOG(BOOT, "Enabling lightgun setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::LightGun;
		}
		else if (prod_id == "MAZAN")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::Mazan;
		}
		else if (prod_id == " BIOHAZARD  GUN SURVIVOR2")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::GunSurvivor;
		}
		else if (prod_id == "WORLD KICKS")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::WorldKicks;
		}
		else if (prod_id == "WORLD KICKS PCB")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::WorldKicksPCB;
		}
		else if (prod_id == "THE TYPING OF THE DEAD"
				|| prod_id == " LUPIN THE THIRD  -THE TYPING-"
				|| prod_id == "------La Keyboardxyu------")
		{
			INFO_LOG(BOOT, "Enabling keyboard for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::Keyboard;
		}
		else if (prod_id == "OUTTRIGGER     JAPAN")
		{
			INFO_LOG(BOOT, "Enabling JVS rotary encoders for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::OutTrigger;
		}
		else if (prod_id == "THE MAZE OF THE KINGS"
				|| prod_id == " CONFIDENTIAL MISSION ---------"
				|| prod_id == "DEATH CRIMSON OX"
				|| prod_id.substr(0, 5) == "hotd2"	// House of the Dead 2
				|| prod_id == "LUPIN THE THIRD  -THE SHOOTING-")
		{
			INFO_LOG(BOOT, "Enabling lightgun as analog setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::LightGunAsAnalog;
		}
		else if (prod_id == "WAVE RUNNER GP")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::WaveRunnerGP;
		}
		else if (prod_id == "  18WHEELER")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::_18Wheeler;
		}
		else if (prod_id == "INU NO OSANPO")	// Dog Walking
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::DogWalking;
		}
		else if (prod_id == " TOUCH DE UNOH -------------"
				|| prod_id == "POKASUKA GHOST (JAPANESE)")
		{
			INFO_LOG(BOOT, "Enabling specific JVS setup for game %s", prod_id.c_str());
			settings.input.JammaSetup = JVS::TouchDeUno;
		}
	}
}

void dc_reset(bool hard)
{
	NetworkHandshake::term();
	if (hard)
	{
		memwatch::unprotect();
		memwatch::reset();
	}
	sh4_sched_reset(hard);
	pvr::reset(hard);
	libAICA_Reset(hard);
	aicaarm::reset();
	sh4_cpu.Reset(true);
	mem_Reset(hard);
}

static void setPlatform(int platform)
{
	if (VRAM_SIZE != 0)
		_vmem_unprotect_vram(0, VRAM_SIZE);
	elan::ERAM_SIZE = 0;
	switch (platform)
	{
	case DC_PLATFORM_DREAMCAST:
		settings.platform.ram_size = 16 * 1024 * 1024;
		settings.platform.vram_size = 8 * 1024 * 1024;
		settings.platform.aram_size = 2 * 1024 * 1024;
		settings.platform.bios_size = 2 * 1024 * 1024;
		settings.platform.flash_size = 128 * 1024;
		break;
	case DC_PLATFORM_NAOMI:
		settings.platform.ram_size = 32 * 1024 * 1024;
		settings.platform.vram_size = 16 * 1024 * 1024;
		settings.platform.aram_size = 8 * 1024 * 1024;
		settings.platform.bios_size = 2 * 1024 * 1024;
		settings.platform.flash_size = 32 * 1024;	// battery-backed ram
		break;
	case DC_PLATFORM_NAOMI2:
		settings.platform.ram_size = 32 * 1024 * 1024;
		settings.platform.vram_size = 16 * 1024 * 1024; // 2x16 MB VRAM, only 16 emulated
		settings.platform.aram_size = 8 * 1024 * 1024;
		settings.platform.bios_size = 2 * 1024 * 1024;
		settings.platform.flash_size = 32 * 1024;	// battery-backed ram
		elan::ERAM_SIZE = 32 * 1024 * 1024;
		break;
	case DC_PLATFORM_ATOMISWAVE:
		settings.platform.ram_size = 16 * 1024 * 1024;
		settings.platform.vram_size = 8 * 1024 * 1024;
		settings.platform.aram_size = 2 * 1024 * 1024;
		settings.platform.bios_size = 128 * 1024;
		settings.platform.flash_size = 128 * 1024;	// sram
		break;
	default:
		die("Unsupported platform");
		break;
	}
	settings.platform.system = platform;
	settings.platform.ram_mask = settings.platform.ram_size - 1;
	settings.platform.vram_mask = settings.platform.vram_size - 1;
	settings.platform.aram_mask = settings.platform.aram_size - 1;
	_vmem_init_mappings();
}

void Emulator::init()
{
	if (state != Uninitialized)
	{
		verify(state == Init);
		return;
	}
	// Default platform
	setPlatform(DC_PLATFORM_DREAMCAST);

	pvr::init();
	libAICA_Init();
	aicaarm::init();
	mem_Init();
	reios_init();

	// the recompiler may start generating code at this point and needs a fully configured machine
#if FEAT_SHREC != DYNAREC_NONE
	Get_Sh4Recompiler(&sh4_cpu);
	sh4_cpu.Init();		// Also initialize the interpreter
	if(config::DynarecEnabled)
	{
		INFO_LOG(DYNAREC, "Using Recompiler");
	}
	else
#endif
	{
		Get_Sh4Interpreter(&sh4_cpu);
		sh4_cpu.Init();
		INFO_LOG(INTERPRETER, "Using Interpreter");
	}
	state = Init;
}

int getGamePlatform(const char *path)
{
	if (path == NULL)
		// Dreamcast BIOS
		return DC_PLATFORM_DREAMCAST;

	std::string extension = get_file_extension(path);
	if (extension.empty())
		return DC_PLATFORM_DREAMCAST;	// unknown
	if (extension == "zip" || extension == "7z")
		return naomi_cart_GetPlatform(path);
	if (extension == "bin" || extension == "dat" || extension == "lst")
		return DC_PLATFORM_NAOMI;

	return DC_PLATFORM_DREAMCAST;
}

void Emulator::loadGame(const char *path, LoadProgress *progress)
{
	init();
	try {
		DEBUG_LOG(BOOT, "Loading game %s", path == nullptr ? "(nil)" : path);

		if (path != nullptr)
			settings.content.path = path;
		else
			settings.content.path.clear();

		setPlatform(getGamePlatform(path));
		mem_map_default();

		config::Settings::instance().reset();
		config::Settings::instance().load(false);
		dc_reset(true);
		memset(&settings.network.md5, 0, sizeof(settings.network.md5));

		if (settings.platform.isNaomi2() && config::RendererType == RenderType::DirectX9)
			throw FlycastException("DirectX 9 doesn't support Naomi 2 games. Select a different graphics API");

		if (settings.platform.isConsole())
		{
			if (settings.content.path.empty())
			{
				// Boot BIOS
				if (!LoadRomFiles())
					throw FlycastException("No BIOS file found in " + hostfs::getFlashSavePath("", ""));
				InitDrive("");
			}
			else
			{
				std::string extension = get_file_extension(settings.content.path);
				if (extension != "elf")
				{
					if (InitDrive(settings.content.path))
					{
						loadGameSpecificSettings();
						if (config::UseReios || !LoadRomFiles())
						{
							LoadHle();
							NOTICE_LOG(BOOT, "Did not load BIOS, using reios");
							if (!config::UseReios && config::UseReios.isReadOnly())
								gui_display_notification("This game requires a real BIOS", 15000);
						}
					}
					else
					{
						// Content load failed. Boot the BIOS
						settings.content.path.clear();
						if (!LoadRomFiles())
							throw FlycastException("This media cannot be loaded");
						InitDrive("");
					}
				}
				else
				{
					// Elf only supported with HLE BIOS
					LoadHle();
				}
			}

			if (progress)
				progress->progress = 1.0f;
		}
		else if (settings.platform.isArcade())
		{
			LoadRomFiles();
			naomi_cart_LoadRom(path, progress);
			loadGameSpecificSettings();
			// Reload the BIOS in case a game-specific region is set
			naomi_cart_LoadBios(path);
		}
		mcfg_DestroyDevices();
		mcfg_CreateDevices();
		if (settings.platform.isNaomi())
			// Must be done after the maple devices are created and EEPROM is accessible
			naomi_cart_ConfigureEEPROM();
		cheatManager.reset(settings.content.gameId);
		if (cheatManager.isWidescreen())
		{
			gui_display_notification("Widescreen cheat activated", 1000);
			config::ScreenStretching.override(134);	// 4:3 -> 16:9
		}
		// reload settings so that all settings can be overridden
		loadGameSpecificSettings();
		NetworkHandshake::init();
		settings.input.fastForwardMode = false;
		if (!settings.content.path.empty())
		{
			if (config::GGPOEnable)
				dc_loadstate(-1);
			else if (config::AutoLoadState && !NaomiNetworkSupported())
				dc_loadstate(config::SavestateSlot);
		}
		EventManager::event(Event::Start);

		if (progress)
		{
#ifdef GDB_SERVER
			if(config::GDBWaitForConnection)
				progress->label = "Waiting for debugger...";
			else
#endif
				progress->label = "Starting...";
		}

		state = Loaded;
	} catch (...) {
		state = Error;
		throw;
	}
}

void Emulator::runInternal()
{
	if (singleStep)
	{
		sh4_cpu.Step();
		singleStep = false;
	}
	else if(stepRangeTo != 0)
	{
		while (Sh4cntx.pc >= stepRangeFrom && Sh4cntx.pc <= stepRangeTo)
			sh4_cpu.Step();

		stepRangeFrom = 0;
		stepRangeTo = 0;
	}
	else
	{
		do {
			resetRequested = false;

			sh4_cpu.Run();

			if (resetRequested)
			{
				SaveRomFiles();
				dc_reset(false);
			}
		} while (resetRequested);
	}
}

void Emulator::unloadGame()
{
	stop();
	if (state == Loaded || state == Error)
	{
		if (state == Loaded && config::AutoSaveState && !settings.content.path.empty())
			dc_savestate(config::SavestateSlot);
		dc_reset(true);

		config::Settings::instance().reset();
		config::Settings::instance().load(false);
		settings.content.path.clear();
		settings.content.gameId.clear();
		state = Init;
		EventManager::event(Event::Terminate);
	}
}

void Emulator::term()
{
	unloadGame();
	if (state == Init)
	{
		debugger::term();
		sh4_cpu.Term();
		custom_texture.Terminate();	// lr: avoid deadlock on exit (win32)
		reios_term();
		libAICA_Term();
		pvr::term();
		mem_Term();

		_vmem_release();
		state = Terminated;
	}
}

void Emulator::stop()
{
	if (state != Running)
		return;
	// Avoid race condition with GGPO restarting the sh4 for a new frame
	if (config::GGPOEnable)
		NetworkHandshake::term();
	// must be updated after GGPO is stopped since it may run some rollback frames
	state = Loaded;
	sh4_cpu.Stop();
	if (config::ThreadedRendering)
	{
		rend_cancel_emu_wait();
		try {
			auto future = threadResult;
			if(future.valid())
				future.get();
		} catch (const FlycastException& e) {
			WARN_LOG(COMMON, "%s", e.what());
		}
		SaveRomFiles();
		EventManager::event(Event::Pause);
	}
	else
	{
		// defer stopping audio until after the current frame is finished
		// normally only useful on android due to multithreading
		stopRequested = true;
	}
}

// Called on the emulator thread for soft reset
void Emulator::requestReset()
{
	resetRequested = true;
	if (config::GGPOEnable)
		NetworkHandshake::term();
	sh4_cpu.Stop();
}

void loadGameSpecificSettings()
{
	if (settings.platform.isConsole())
	{
		reios_disk_id();
		settings.content.gameId = trim_trailing_ws(std::string(ip_meta.product_number, sizeof(ip_meta.product_number)));

		if (settings.content.gameId.empty())
			return;
	}
	else
	{
		settings.content.gameId = naomi_game_id;
	}

	// Default per-game settings
	loadSpecialSettings();

	config::Settings::instance().setGameId(settings.content.gameId);

	// Reload per-game settings
	config::Settings::instance().load(true);

	if (config::ForceWindowsCE)
	{
		config::ExtraDepthScale.override(0.1f);
		config::FullMMU.override(true);
	}
}

void Emulator::step()
{
	// FIXME single thread is better
	singleStep = true;
	start();
	stop();
}

void Emulator::stepRange(u32 from, u32 to)
{
	stepRangeFrom = from;
	stepRangeTo = to;
	start();
	stop();
}

void dc_loadstate(Deserializer& deser)
{
	custom_texture.Terminate();
#if FEAT_AREC == DYNAREC_JIT
	aicaarm::recompiler::flush();
#endif
	mmu_flush_table();
#if FEAT_SHREC != DYNAREC_NONE
	bm_Reset();
#endif

	dc_deserialize(deser);

	mmu_set_state();
	sh4_cpu.ResetCache();
}

void Emulator::setNetworkState(bool online)
{
	if (settings.network.online != online)
		DEBUG_LOG(NETWORK, "Network state %d", online);
	settings.network.online = online;
	settings.input.fastForwardMode &= !online;
}

EventManager EventManager::Instance;

void EventManager::registerEvent(Event event, Callback callback, void *param)
{
	unregisterEvent(event, callback, param);
	auto it = callbacks.find(event);
	if (it != callbacks.end())
		it->second.push_back(std::make_pair(callback, param));
	else
		callbacks.insert({ event, { std::make_pair(callback, param) } });
}

void EventManager::unregisterEvent(Event event, Callback callback, void *param) {
	auto it = callbacks.find(event);
	if (it == callbacks.end())
		return;

	auto it2 = std::find(it->second.begin(), it->second.end(), std::make_pair(callback, param));
	if (it2 == it->second.end())
		return;

	it->second.erase(it2);
}

void EventManager::broadcastEvent(Event event) {
	auto it = callbacks.find(event);
	if (it == callbacks.end())
		return;

	for (auto& pair : it->second)
		pair.first(event, pair.second);
}

void Emulator::run()
{
	verify(state == Running);
	startTime = sh4_sched_now64();
	renderTimeout = false;
	try {
		runInternal();
		if (ggpo::active())
			ggpo::nextFrame();
	} catch (...) {
		setNetworkState(false);
		state = Error;
		sh4_cpu.Stop();
		EventManager::event(Event::Pause);
		throw;
	}
}

void Emulator::start()
{
	if (state == Running)
		return;
	verify(state == Loaded);
	state = Running;
	SetMemoryHandlers();
	if (config::GGPOEnable && config::ThreadedRendering)
		// Not supported with GGPO
		config::EmulateFramebuffer.override(false);
#if FEAT_SHREC != DYNAREC_NONE
	if (config::DynarecEnabled)
	{
		Get_Sh4Recompiler(&sh4_cpu);
		INFO_LOG(DYNAREC, "Using Recompiler");
	}
	else
#endif
	{
		Get_Sh4Interpreter(&sh4_cpu);
		INFO_LOG(DYNAREC, "Using Interpreter");
	}

	memwatch::protect();

	if (config::ThreadedRendering)
	{
		const std::lock_guard<std::mutex> lock(mutex);
		threadResult = std::async(std::launch::async, [this] {
				InitAudio();

				try {
					while (state == Running || singleStep || stepRangeTo != 0)
					{
						startTime = sh4_sched_now64();
						renderTimeout = false;
						runInternal();
						if (!ggpo::nextFrame())
							break;
					}
					TermAudio();
				} catch (...) {
					setNetworkState(false);
					state = Error;
					sh4_cpu.Stop();
					TermAudio();
					throw;
				}
		}).share();
	}
	else
	{
		stopRequested = false;
		InitAudio();
	}

	EventManager::event(Event::Resume);
}

bool Emulator::checkStatus()
{
	try {
		const std::lock_guard<std::mutex> lock(mutex);
		if (threadResult.valid())
		{
			auto result = threadResult.wait_for(std::chrono::seconds(0));
			if (result == std::future_status::timeout)
				return true;
			threadResult.get();
		}
		return false;
	} catch (...) {
		EventManager::event(Event::Pause);
		throw;
	}
}

bool Emulator::render()
{
	FC_PROFILE_SCOPE;

	if (!config::ThreadedRendering)
	{
		if (state != Running)
			return false;
		run();
		if (stopRequested)
		{
			stopRequested = false;
			TermAudio();
			SaveRomFiles();
			EventManager::event(Event::Pause);
		}
		// TODO if stopping due to a user request, no frame has been rendered
		return !renderTimeout;
	}
	if (!checkStatus())
		return false;
	if (state != Running)
		return false;
	return rend_single_frame(true); // FIXME stop flag?
}

void Emulator::vblank()
{
	EventManager::event(Event::VBlank);
	// Time out if a frame hasn't been rendered for 50 ms
	if (sh4_sched_now64() - startTime <= 10000000)
		return;
	renderTimeout = true;
	if (ggpo::active())
		ggpo::endOfFrame();
	else if (!config::ThreadedRendering)
		sh4_cpu.Stop();
}

Emulator emu;
