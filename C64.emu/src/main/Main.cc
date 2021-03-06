/*  This file is part of C64.emu.

	C64.emu is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	C64.emu is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with C64.emu.  If not, see <http://www.gnu.org/licenses/> */

#define LOGTAG "main"
#include <emuframework/EmuApp.hh>
#include <emuframework/EmuInput.hh>
#include <emuframework/EmuAppInlines.hh>
#include <imagine/thread/Thread.hh>
#include <imagine/thread/Semaphore.hh>
#include <imagine/gui/AlertView.hh>
#include "internal.hh"
#include <sys/time.h>

extern "C"
{
	#include "machine.h"
	#include "maincpu.h"
	#include "drive.h"
	#include "lib.h"
	#include "util.h"
	#include "ioutil.h"
	#include "uiapi.h"
	#include "console.h"
	#include "monitor.h"
	#include "video.h"
	#include "cmdline.h"
	#include "gfxoutput.h"
	#include "videoarch.h"
	#include "init.h"
	#include "resources.h"
	#include "sysfile.h"
	#include "log.h"
	#include "archdep.h"
	#include "palette.h"
	#include "keyboard.h"
	#include "autostart.h"
	#include "kbdbuf.h"
	#include "attach.h"
	#include "raster.h"
	#include "sound.h"
	#include "cartridge.h"
	#include "tape.h"
	#include "interrupt.h"
	#include "sid/sid.h"
	#include "sid/sid-resources.h"
	#include "c64/cart/c64cartsystem.h"
	#include "vicii.h"
	#include "diskimage.h"
	#include "vdrive-internal.h"
	#include "autostart-prg.h"
}

const char *EmuSystem::creditsViewStr = CREDITS_INFO_STRING "(c) 2013-2018\nRobert Broglia\nwww.explusalpha.com\n\nPortions (c) the\nVice Team\nwww.viceteam.org";
IG::Semaphore execSem{0}, execDoneSem{0};
bool runningFrame = false, doAudio = false;
static bool c64IsInit = false, c64FailedInit = false;
bool autostartOnLoad = true;
FS::PathString firmwareBasePath{};
FS::PathString sysFilePath[Config::envIsLinux ? 5 : 3]{};
bool isPal = false;
VicePlugin plugin{};
ViceSystem currSystem = VICE_SYSTEM_C64;

bool EmuSystem::hasPALVideoSystem = true;
bool EmuSystem::hasResetModes = true;
bool EmuSystem::handlesGenericIO = false;

const char *EmuSystem::shortSystemName()
{
	return "C64";
}

const char *EmuSystem::systemName()
{
	return "Commodore 64";
}

int intResource(const char *name)
{
	int val = 0;
	auto failed = plugin.resources_get_int(name, &val);
	assert(!failed);
	return val;
}

void setAutostartWarp(bool on)
{
	plugin.resources_set_int("AutostartWarp", on);
}

static bool autostartWarp()
{
	return intResource("AutostartWarp");
}

void setAutostartTDE(bool on)
{
	plugin.resources_set_int("AutostartHandleTrueDriveEmulation", on);
}

static bool autostartTDE()
{
	return intResource("AutostartHandleTrueDriveEmulation");
}

static bool sysIsPal()
{
	switch(intResource("MachineVideoStandard"))
	{
		case MACHINE_SYNC_PAL:
		case MACHINE_SYNC_PALN:
			return true;
	}
	return false;
}

static void setModel(int model)
{
	logMsg("setting model id:%d", model);
	plugin.model_set(model);
}

int sysModel()
{
	return plugin.model_get();
}

void setSysModel(int model)
{
	setModel(model);
	isPal = sysIsPal();
	if(isPal)
	{
		logMsg("C64 model has PAL timings");
	}
}

void setBorderMode(int mode)
{
	if(!plugin.borderModeStr)
		return;
	plugin.resources_set_int(plugin.borderModeStr, mode);
}

static int borderMode()
{
	if(!plugin.borderModeStr)
		return -1;
	return intResource(plugin.borderModeStr);
}

void setSidEngine(int engine)
{
	logMsg("set SID engine %d", engine);
	plugin.resources_set_int("SidEngine", engine);
}

static int sidEngine()
{
	return intResource("SidEngine");
}

void setReSidSampling(int sampling)
{
	logMsg("set ReSID sampling %d", sampling);
	plugin.resources_set_int("SidResidSampling", sampling);
}

static int reSidSampling()
{
	return intResource("SidResidSampling");
}

void setDriveTrueEmulation(bool on)
{
	plugin.resources_set_int("DriveTrueEmulation", on);
}

bool driveTrueEmulation()
{
	return intResource("DriveTrueEmulation");
}

void setVirtualDeviceTraps(bool on)
{
	plugin.resources_set_int("VirtualDevices", on);
}

bool virtualDeviceTraps()
{
	return intResource("VirtualDevices");
}

void setDefaultC64Model(int model)
{
	optionC64Model = model;
}

void setDefaultDTVModel(int model)
{
	optionDTVModel = model;
}

void setDefaultC128Model(int model)
{
	optionC128Model = model;
}

void setDefaultSuperCPUModel(int model)
{
	optionSuperCPUModel = model;
}

void setDefaultCBM2Model(int model)
{
	optionCBM2Model = model;
}

void setDefaultCBM5x0Model(int model)
{
	optionCBM5x0Model = model;
}

void setDefaultPETModel(int model)
{
	optionPETModel = model;
}

void setDefaultPlus4Model(int model)
{
	optionPlus4Model = model;
}

void setDefaultVIC20Model(int model)
{
	optionVIC20Model = model;
}

static void setIntResourceToDefault(const char *name)
{
	int val;
	if(plugin.resources_get_default_value(name, &val) < 0)
	{
		return;
	}
	logMsg("setting resource %s to default:%d", name, val);
	plugin.resources_set_int(name, val);
}

void applySessionOptions()
{
	if((int)optionModel == -1)
	{
		logMsg("using default model");
		setSysModel(optionDefaultModel(currSystem));
	}
	else
	{
		setSysModel(optionModel);
	}
	setVirtualDeviceTraps(optionVirtualDeviceTraps);
	setDriveTrueEmulation(optionDriveTrueEmulation);
	setAutostartWarp(optionAutostartWarp);
	setAutostartTDE(optionAutostartTDE);
}

static void applyInitialOptionResources()
{
	applySessionOptions();
	setBorderMode(optionBorderMode);
	setSidEngine(optionSidEngine);
	setReSidSampling(optionReSidSampling);
	// default drive setup
	setIntResourceToDefault("Drive8Type");
	plugin.resources_set_int("Drive9Type", DRIVE_TYPE_NONE);
	plugin.resources_set_int("Drive10Type", DRIVE_TYPE_NONE);
	plugin.resources_set_int("Drive11Type", DRIVE_TYPE_NONE);
}

bool hasC64DiskExtension(const char *name)
{
	return string_hasDotExtension(name, "d64") ||
			string_hasDotExtension(name, "d67") ||
			string_hasDotExtension(name, "d71") ||
			string_hasDotExtension(name, "d80") ||
			string_hasDotExtension(name, "d81") ||
			string_hasDotExtension(name, "d82") ||
			string_hasDotExtension(name, "d1m") ||
			string_hasDotExtension(name, "d2m") ||
			string_hasDotExtension(name, "d4m") ||
			string_hasDotExtension(name, "g64") ||
			string_hasDotExtension(name, "p64") ||
			string_hasDotExtension(name, "g41") ||
			string_hasDotExtension(name, "x64") ||
			string_hasDotExtension(name, "dsk");
}

bool hasC64TapeExtension(const char *name)
{
	return string_hasDotExtension(name, "t64") ||
			string_hasDotExtension(name, "tap");
}

bool hasC64CartExtension(const char *name)
{
	return string_hasDotExtension(name, "bin") ||
			string_hasDotExtension(name, "crt");
}

static bool hasC64Extension(const char *name)
{
	return hasC64DiskExtension(name) ||
			hasC64TapeExtension(name) ||
			hasC64CartExtension(name) ||
			string_hasDotExtension(name, "prg") ||
			string_hasDotExtension(name, "p00");
}

EmuSystem::NameFilterFunc EmuSystem::defaultFsFilter = hasC64Extension;
EmuSystem::NameFilterFunc EmuSystem::defaultBenchmarkFsFilter = hasC64Extension;

void EmuSystem::reset(ResetMode mode)
{
	assert(gameIsRunning());
	plugin.machine_trigger_reset(mode == RESET_HARD ? MACHINE_RESET_MODE_HARD : MACHINE_RESET_MODE_SOFT);
}

FS::PathString EmuSystem::sprintStateFilename(int slot, const char *statePath, const char *gameName)
{
	return FS::makePathStringPrintf("%s/%s.%c.vsf", statePath, gameName, saveSlotChar(slot));
}

struct SnapshotTrapData
{
	constexpr SnapshotTrapData() {}
	bool hasError = true;
	const char *pathStr{};
};

static void loadSnapshotTrap(WORD, void *data)
{
	auto snapData = (SnapshotTrapData*)data;
	logMsg("loading state: %s", snapData->pathStr);
	if(plugin.machine_read_snapshot(snapData->pathStr, 0) < 0)
		snapData->hasError = true;
	else
		snapData->hasError = false;
}

static void saveSnapshotTrap(WORD, void *data)
{
	auto snapData = (SnapshotTrapData*)data;
	logMsg("saving state: %s", snapData->pathStr);
	if(plugin.machine_write_snapshot(snapData->pathStr, 1, 1, 0) < 0)
		snapData->hasError = true;
	else
		snapData->hasError = false;
}

EmuSystem::Error EmuSystem::saveState(const char *path)
{
	SnapshotTrapData data;
	data.pathStr = path;
	plugin.interrupt_maincpu_trigger_trap(saveSnapshotTrap, (void*)&data);
	skipFrames(1, false); // execute cpu trap
	return data.hasError ? makeFileWriteError() : Error{};
}

EmuSystem::Error EmuSystem::loadState(const char *path)
{
	plugin.resources_set_int("WarpMode", 0);
	SnapshotTrapData data;
	data.pathStr = path;
	skipFrames(1, false); // run extra frame in case C64 was just started
	plugin.interrupt_maincpu_trigger_trap(loadSnapshotTrap, (void*)&data);
	skipFrames(1, false); // execute cpu trap, snapshot load may cause reboot from a C64 model change
	if(data.hasError)
		return makeFileReadError();
	// reload snapshot in case last load caused a reboot
	plugin.interrupt_maincpu_trigger_trap(loadSnapshotTrap, (void*)&data);
	skipFrames(1, false); // execute cpu trap
	bool hasError = data.hasError;
	isPal = sysIsPal();
	return hasError ? makeFileReadError() : Error{};
}

void EmuSystem::saveBackupMem()
{
	if(gameIsRunning())
	{
		// nothing to do for now
	}
}

bool EmuSystem::vidSysIsPAL() { return isPal; }

void EmuSystem::closeSystem()
{
	if(!gameIsRunning())
	{
		return;
	}
	logMsg("closing game %s", gameName().data());
	saveBackupMem();
	plugin.resources_set_int("WarpMode", 0);
	plugin.tape_image_detach(1);
	plugin.file_system_detach_disk(8);
	plugin.file_system_detach_disk(9);
	plugin.file_system_detach_disk(10);
	plugin.file_system_detach_disk(11);
	plugin.cartridge_detach_image(-1);
	plugin.machine_trigger_reset(MACHINE_RESET_MODE_HARD);
	autostartOnLoad = true;
}

static EmuSystem::Error c64FirmwareError()
{
	return EmuSystem::makeError("System files missing, place C64, DRIVES, & PRINTER directories from VICE"
		" in a path below, or set a custom path in options:\n"
		#if defined CONFIG_ENV_LINUX && !defined CONFIG_MACHINE_PANDORA
		"%s\n%s\n%s", EmuApp::assetPath().data(), "~/.local/share/C64.emu", "/usr/share/games/vice");
		#else
		"%s/C64.emu", Base::sharedStoragePath().data());
		#endif
}

static bool initC64()
{
	if(c64IsInit)
		return true;
	logMsg("initializing C64");
  if(plugin.init_main() < 0)
  {
  	logErr("error in init_main()");
  	c64FailedInit = true;
  	return false;
	}
	c64IsInit = true;
	return true;
}

bool EmuApp::willCreateSystem(ViewAttachParams attach, Input::Event e)
{
	if(!c64FailedInit)
		return true;
	pushAndShowNewYesNoAlertView(attach, e,
		"A previous system file load failed, you must restart the app to run any C64 software",
		"Exit Now", "Cancel", []() { Base::exit(); }, {});
	return false;
}

EmuSystem::Error EmuSystem::loadGame(IO &, OnLoadProgressDelegate)
{
	if(!initC64())
	{
		return c64FirmwareError();
	}
	applyInitialOptionResources();
	logMsg("loading %s", fullGamePath());
	if(autostartOnLoad)
	{
		if(plugin.autostart_autodetect_)
		{
			if(string_hasDotExtension(fullGamePath(), "prg"))
			{
				// needed to store AutostartPrgDisk.d64
				makeDefaultBaseSavePath();
			}
			if(plugin.autostart_autodetect(fullGamePath(), nullptr, 0, AUTOSTART_MODE_RUN) != 0)
			{
				return EmuSystem::makeFileReadError();
			}
		}
		else
		{
			// TODO
			return EmuSystem::makeError("Autostart not implemented");
		}
	}
	else
	{
		if(hasC64DiskExtension(fullGamePath()))
		{
			if(plugin.file_system_attach_disk(8, fullGamePath()) != 0)
			{
				return EmuSystem::makeFileReadError();
			}
		}
	}
	return {};
}

static void execC64Frame()
{
	// signal C64 thread to execute one frame and wait for it to finish
	execSem.notify();
	execDoneSem.wait();
}

void EmuSystem::runFrame(EmuVideo *video, bool renderAudio)
{
	runningFrame = 1;
	doAudio = renderAudio;
	setCanvasSkipFrame(!video);
	execC64Frame();
	if(video)
	{
		video->setFormat(canvasSrcPix);
		video->writeFrame(canvasSrcPix);
	}
	runningFrame = 0;
}

void EmuSystem::configAudioRate(double frameTime, int rate)
{
	logMsg("set audio rate %d", rate);
	pcmFormat.rate = rate;
	int mixRate = std::round(rate * (systemFrameRate * frameTime));
	int currRate = 0;
	plugin.resources_get_int("SoundSampleRate", &currRate);
	if(currRate != mixRate)
	{
		plugin.resources_set_int("SoundSampleRate", mixRate);
	}
}

bool EmuSystem::shouldFastForward()
{
	return *plugin.warp_mode_enabled;
}

void EmuApp::onCustomizeNavView(EmuApp::NavView &view)
{
	const Gfx::LGradientStopDesc navViewGrad[] =
	{
		{ .0, Gfx::VertexColorPixelFormat.build(.5, .5, .5, 1.) },
		{ .03, Gfx::VertexColorPixelFormat.build(48./255., 36./255., 144./255., 1.) },
		{ .3, Gfx::VertexColorPixelFormat.build(48./255., 36./255., 144./255., 1.) },
		{ .97, Gfx::VertexColorPixelFormat.build((48./255.) * .4, (36./255.) * .4, (144./255.) * .4, 1.) },
		{ 1., Gfx::VertexColorPixelFormat.build(.5, .5, .5, 1.) },
	};
	view.setBackgroundGradient(navViewGrad);
}

void EmuApp::onMainWindowCreated(ViewAttachParams attach, Input::Event e)
{
	sysFilePath[0] = firmwareBasePath;
	EmuControls::updateKeyboardMapping();
	setSysModel(optionDefaultModel(currSystem));
	plugin.resources_set_string("AutostartPrgDiskImage",
		FS::makePathStringPrintf("%s/AutostartPrgDisk.d64", EmuSystem::baseDefaultGameSavePath().data()).data());
}

EmuSystem::Error EmuSystem::onInit()
{
	IG::makeDetachedThread(
		[]()
		{
			execSem.wait();
			logMsg("running C64");
			plugin.maincpu_mainloop();
		});

	#if defined CONFIG_ENV_LINUX && !defined CONFIG_MACHINE_PANDORA
	sysFilePath[1] = EmuApp::assetPath();
	sysFilePath[2] = FS::makePathStringPrintf("%s/C64.emu.zip", EmuApp::assetPath().data());
	sysFilePath[3] = {"~/.local/share/C64.emu"};
	sysFilePath[4] = {"/usr/share/games/vice"};
	#else
	{
		sysFilePath[1] = FS::makePathStringPrintf("%s/C64.emu", Base::sharedStoragePath().data());
		sysFilePath[2] = FS::makePathStringPrintf("%s/C64.emu.zip", Base::sharedStoragePath().data());
	}
	#endif

	// higher quality ReSID sampling modes take orders of magnitude more CPU power,
	// set some reasonable defaults based on CPU type
	#if defined __x86_64__
	optionReSidSampling.initDefault(SID_RESID_SAMPLING_RESAMPLING);
	#elif defined __aarch64__
	optionReSidSampling.initDefault(SID_RESID_SAMPLING_INTERPOLATION);
	#else
	optionReSidSampling.initDefault(SID_RESID_SAMPLING_FAST);
	#endif

	EmuSystem::pcmFormat.channels = 1;
	return {};
}
