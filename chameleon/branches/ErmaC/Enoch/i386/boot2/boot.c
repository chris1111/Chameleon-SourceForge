/*
 * Copyright (c) 1999-2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Portions Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 2.0 (the "License").	You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON- INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * Mach Operating System
 * Copyright (c) 1990 Carnegie-Mellon University
 * Copyright (c) 1989 Carnegie-Mellon University
 * All rights reserved.	 The CMU software License Agreement specifies
 * the terms and conditions for use and redistribution.
 *
 *          INTEL CORPORATION PROPRIETARY INFORMATION
 *
 *  This software is supplied under the terms of a license  agreement or
 *  nondisclosure agreement with Intel Corporation and may not be copied
 *  nor disclosed except in accordance with the terms of that agreement.
 *
 *	Copyright 1988, 1989 by Intel Corporation
 *
 *
 * Copyright 1993 NeXT Computer, Inc.
 * All rights reserved.
 *
 * Completely reworked by Sam Streeper (sam_s@NeXT.com)
 * Reworked again by Curtis Galloway (galloway@NeXT.com)
 */

#include "config.h"
#include "boot.h"
#include "bootstruct.h"
#include "fake_efi.h"
#include "sl.h"
#include "libsa.h"
#include "ramdisk.h"
#include "gui.h"
#include "platform.h"
#include "modules.h"
#include "device_tree.h"
#include "xml.h" 

#if DEBUG_BOOT2
	#define DBG(x...)	printf(x)
#else
	#define DBG(x...)	msglog(x)
#endif

/*
 * How long to wait (in seconds) to load the
 * kernel after displaying the "boot:" prompt.
 */
#define kBootErrorTimeout 5

bool		gOverrideKernel;
bool		gEnableCDROMRescan;
bool		gScanSingleDrive;
bool		useGUI;

/* recovery or installer ? */
bool isInstaller;
bool isRecoveryHD;
bool isMacOSXUpgrade;
bool isOSXUpgrade;

#if DEBUG_INTERRUPTS
	static int	interruptsAvailable = 0;
#endif

static bool	gUnloadPXEOnExit = false;

static char	gCacheNameAdler[64 + 256];
char		*gPlatformName = gCacheNameAdler;

char		gRootDevice[ROOT_DEVICE_SIZE];
char		gMKextName[512];
int		bvCount = 0;
int		gDeviceCount = 0;
//int		menucount = 0;
long		gBootMode; /* defaults to 0 == kBootModeNormal */

BVRef		bvr;
BVRef		menuBVR;
BVRef		bvChain;

static unsigned long	Adler32(unsigned char *buffer, long length);
//static void			selectBiosDevice(void);

/** options.c **/
extern char *msgbuf;
void showTextBuffer(char *buf, int size);

//==========================================================================
// Zero the BSS.

static void zeroBSS(void)
{
	extern char  bss_start  __asm("section$start$__DATA$__bss");
	extern char  bss_end    __asm("section$end$__DATA$__bss");
	extern char  common_start  __asm("section$start$__DATA$__common");
	extern char  common_end    __asm("section$end$__DATA$__common");

	bzero(&bss_start, (&bss_end - &bss_start));
	bzero(&common_start, (&common_end - &common_start));
}

//==========================================================================
// Malloc error function

static void malloc_error(char *addr, size_t size, const char *file, int line)
{
	stop("\nMemory allocation error! Addr: 0x%x, Size: 0x%x, File: %s, Line: %d\n",
									 (unsigned)addr, (unsigned)size, file, line);
}

//==========================================================================
//Initializes the runtime. Right now this means zeroing the BSS and initializing malloc.
//
void initialize_runtime(void)
{
	zeroBSS();
	malloc_init(0, 0, 0, malloc_error);
}

// =========================================================================
// Load the Kernel.plist override config file if any
static void setupKernelConfigFile(const char *filename)
{
	char		dirSpec[128];
	const char	*override_pathname = NULL;
	int		len = 0, err = 0;

	// Take in account user overriding
	if (getValueForKey(kKERNELKey, &override_pathname, &len, &bootInfo->chameleonConfig) && len > 0)
	{
		// Specify a path to a file, e.g. KERNELPlist=/Extra/Kernel2.plist
		strcpy(dirSpec, override_pathname);
		err = loadConfigFile(dirSpec, &bootInfo->kernelConfig);
	}
	else
	{
		// Check selected volume's Extra.
		sprintf(dirSpec, "/Extra/%s", filename);
		err = loadConfigFile(dirSpec, &bootInfo->kernelConfig);
	}

	if (!err)
	{
		getBoolForKey(kKernelBooter_kexts,   &KernelBooter_kexts,    KERNELPlist);
		getBoolForKey(kKernelPm,             &KernelPm,              KERNELPlist);
		getBoolForKey(kKernelLapicError,     &KernelLapicError,      KERNELPlist);
		getBoolForKey(kKernelLapicVersion,   &KernelLapicVersion,    KERNELPlist);
		getBoolForKey(kKernelHaswell,        &KernelHaswell,         KERNELPlist);
		getBoolForKey(kKernelcpuFamily,      &KernelcpuFamily,       KERNELPlist);
		getBoolForKey(kKernelSSE3,           &KernelSSE3,            KERNELPlist);
	}
	else
	{
		verbose("No %s replacement found.\n", filename);
	}
}

#if KEXTPATCH_SUPPORT
// =========================================================================
// Load the Kexts.plist override config file if any
static void setupKextConfigFile(const char *filename)
{
	char		dirSpec[128];
	const char	*override_pathname = NULL;
	int		len = 0, err = 0;

	AppleRTCPatch = false;
	AICPMPatch = false;
	OrangeIconFixSata = false;
	NVIDIAWebDrv = false;
	TrimEnablerSata = false;

	// Take in account user overriding
	if (getValueForKey(kKEXTKey, &override_pathname, &len, &bootInfo->chameleonConfig) && len > 0)
	{
		// Specify a path to a file, e.g. KEXTPlist=/Extra/Kext2.plist
		strcpy(dirSpec, override_pathname);
		err = loadConfigFile(dirSpec, &bootInfo->kextConfig);
	} else {
		// Check selected volume's Extra.
		sprintf(dirSpec, "/Extra/%s", filename);
		err = loadConfigFile(dirSpec, &bootInfo->kextConfig);
	}

	if (!err)
	{
		getBoolForKey(kAppleRTCPatch,       &AppleRTCPatch,        KEXTPlist);
		getBoolForKey(kAICPMPatch,          &AICPMPatch,           KEXTPlist);
		getBoolForKey(kNVIDIAWebDrv,        &NVIDIAWebDrv,         KEXTPlist);
		getBoolForKey(kOrangeIconFixSata,   &OrangeIconFixSata,    KEXTPlist);
		getBoolForKey(kTrimEnablerSata,     &TrimEnablerSata,      KEXTPlist);
	} else {
		verbose("No %s replacement found.\n", filename);
	}
}
#endif

//==========================================================================
// ExecKernel - Load the kernel image (mach-o) and jump to its entry point.

static int ExecKernel(void *binary)
{
	int			ret;
	entry_t		kernelEntry;

	bootArgs->kaddr = bootArgs->ksize = 0;

// ===============================================================================

	gMacOSVersion[0] = 0;
	verbose("Booting on %s %s (%s)\n", (MacOSVerCurrent < MacOSVer2Int("10.8")) ? "Mac OS X" : (MacOSVerCurrent < MacOSVer2Int("10.12")) ? "OS X" : "macOS", gBootVolume->OSFullVer, gBootVolume->OSBuildVer );

	setupBooterArgs();

// ===============================================================================

	execute_hook("ExecKernel", (void *)binary, NULL, NULL, NULL);

	ret = DecodeKernel(binary,
					   &kernelEntry,
					   (char **) &bootArgs->kaddr,
					   (int *)&bootArgs->ksize);

	if ( ret != 0 )
	{
		printf("Decoding kernel failed.\n");
		pause();
		return ret;
	}

	// Reserve space for boot args
	reserveKernBootStruct();

	// Notify modules that the kernel has been decoded
	execute_hook("DecodedKernel", (void *)binary, (void *)bootArgs->kaddr, (void *)bootArgs->ksize, NULL);

	setupFakeEfi();

	// Load boot drivers from the specifed root path.
	//if (!gHaveKernelCache)
	{
		LoadDrivers("/");
	}

	execute_hook("DriversLoaded", (void *)binary, NULL, NULL, NULL);

	clearActivityIndicator();

	if (gErrors)
	{
		printf("Errors encountered while starting up the computer.\n");
		printf("Pausing %d seconds...\n", kBootErrorTimeout);
		sleep(kBootErrorTimeout);
	}

	md0Ramdisk();

	// Cleanup the PXE base code.

	if ( (gBootFileType == kNetworkDeviceType) && gUnloadPXEOnExit )
	{
		if ( (ret = nbpUnloadBaseCode()) != nbpStatusSuccess )
		{
			printf("nbpUnloadBaseCode error %d\n", (int)ret);
			sleep(2);
		}
	}

	bool dummyVal;
	if (getBoolForKey(kWaitForKeypressKey, &dummyVal, &bootInfo->chameleonConfig) && dummyVal)
	{
		showTextBuffer(msgbuf, strlen(msgbuf));
	}

	usb_loop();

#if DEBUG
	if (interruptsAvailable) ShowInterruptCounters();
#endif

	// If we were in text mode, switch to graphics mode.
	// This will draw the boot graphics unless we are in
	// verbose mode.
	if (gVerboseMode)
	{
		setVideoMode( GRAPHICS_MODE );
	}
	else
	{
		drawBootGraphics();
	}

	DBG("Starting Darwin/%s [%s]\n",( archCpuType == CPU_TYPE_I386 ) ? "x86" : "x86_64", gDarwinBuildVerStr);
	DBG("Boot Args: %s\n", bootArgs->CommandLine);

	setupBooterLog();

	finalizeBootStruct();

	// Jump to kernel's entry point. There's no going back now.
	if ( MacOSVerCurrent < MacOSVer2Int("10.7") )
	{
		// Notify modules that the kernel is about to be started
		execute_hook("Kernel Start", (void *)kernelEntry, (void *)bootArgsLegacy, NULL, NULL);

		startprog( kernelEntry, bootArgsLegacy );
	}
	else
	{
		// Notify modules that the kernel is about to be started
		execute_hook("Kernel Start", (void *)kernelEntry, (void *)bootArgs, NULL, NULL);

		startprog( kernelEntry, bootArgs );
	}

	// Not reached
	__builtin_unreachable();
}

//==========================================================================
// LoadKernelCache - Try to load Kernel Cache.
// return the length of the loaded cache file or -1 on error
long LoadKernelCache(const char *cacheFile, void **binary)
{
	char		kernelCacheFile[512];
	char		kernelCachePath[512];
	long		flags, ret=-1;
	unsigned long	adler32 = 0;
	u_int32_t time, cachetime, kerneltime, exttime;

	if((gBootMode & kBootModeSafe) != 0)
	{
		DBG("Kernel Cache ignored.\n");
		return -1;
	}

	// Use specify kernel cache file if not empty
	if (cacheFile[0] != 0)
	{
		strlcpy(kernelCacheFile, cacheFile, sizeof(kernelCacheFile));
		verbose("Specified kernel cache file path: %s\n", cacheFile);
	}
	else
	{
		// Leopard prelink kernel cache file
		if ( MacOSVerCurrent >= MacOSVer2Int("10.4") && MacOSVerCurrent <= MacOSVer2Int("10.5") ) // OSX is 10.4 or 10.5
		{
			// Reset cache name.
			bzero(gCacheNameAdler + 64, sizeof(gCacheNameAdler) - 64);
			snprintf(gCacheNameAdler + 64, sizeof(gCacheNameAdler) - 64, "%s,%s", gRootDevice, bootInfo->bootFile);
			adler32 = Adler32((unsigned char *)gCacheNameAdler, sizeof(gCacheNameAdler));
			snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%s.%08lX", kDefaultCachePathLeo, adler32);
			verbose("Reseted kernel cache file path: %s\n", kernelCacheFile);

		}
		// Snow Leopard prelink kernel cache file
		else if ( MacOSVerCurrent >= MacOSVer2Int("10.6") && MacOSVerCurrent < MacOSVer2Int("10.7") )
		{
			snprintf(kernelCacheFile, sizeof(kernelCacheFile), "kernelcache_%s",
				(archCpuType == CPU_TYPE_I386) ? "i386" : "x86_64");

			int		lnam = strlen(kernelCacheFile) + 9; //with adler32
			char		*name;
			u_int32_t	prev_time = 0;

			struct dirstuff *cacheDir = opendir(kDefaultCachePathSnow);

			/* TODO: handle error? */
			if (cacheDir)
			{
				while(readdir(cacheDir, (const char**)&name, &flags, &time) >= 0)
				{
					if (((flags & kFileTypeMask) != kFileTypeDirectory) && time > prev_time
						&& strstr(name, kernelCacheFile) && (name[lnam] != '.'))
					{
						snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%s%s", kDefaultCachePathSnow, name);
						prev_time = time;
					}
				}
				verbose("Kernel Cache file path (Mac OS X 10.6): %s\n", kernelCacheFile);
			}
			closedir(cacheDir);
		}
		else if ( MacOSVerCurrent >= MacOSVer2Int("10.7") && MacOSVerCurrent < MacOSVer2Int("10.9") )
		{
			// Lion, Mountain Lion
			// for 10.7 10.8
			if (isMacOSXUpgrade)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", "/Mac OS X Install Data/");
			}
			else if (isInstaller)
			{
				if (MacOSVerCurrent >= MacOSVer2Int("10.7") && MacOSVerCurrent < MacOSVer2Int("10.8") )
				{
					snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kLionInstallerDataFolder);
				}
				else if ( MacOSVerCurrent >= MacOSVer2Int("10.8") && MacOSVerCurrent < MacOSVer2Int("10.9") )
				{
					snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kMLionInstallerDataFolder);
				}
				else if ( MacOSVerCurrent > MacOSVer2Int("10.12") && MacOSVerCurrent < MacOSVer2Int("10.16") )
				{
					snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kHSierraInstallerDataFolder);
				}
			}
			else if (isRecoveryHD)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kDefaultCacheRecoveryHD);
			}
			else
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kDefaultCachePathSnow);
			}

			verbose("Kernel Cache file path (Mac OS X 10.7 and newer): %s\n", kernelCacheFile);

		}
		else if ( MacOSVerCurrent >= MacOSVer2Int("10.9") && MacOSVerCurrent < MacOSVer2Int("10.10") )
		{
			// Mavericks prelinked cache file
			// for 10.9
			if (isOSXUpgrade)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", "/OS X Install Data/");
			}
			else if (isInstaller)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kDefaultCacheInstallerNew);
			}
			else if (isRecoveryHD)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kDefaultCacheRecoveryHD);
			}
			else
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kDefaultCachePathSnow);
			}

			verbose("Kernel Cache file path (OS X 10.9): %s\n", kernelCacheFile);

		}
		else if ( MacOSVerCurrent >= MacOSVer2Int("10.10") && MacOSVerCurrent < MacOSVer2Int("10.11") )
		{
			// Yosemite prelink kernel cache file
			// for 10.10 and 10.11
			if (isOSXUpgrade)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", "/OS X Install Data/");
			}
			else if (isInstaller)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kDefaultCacheInstallerNew);
			}
			else if (isRecoveryHD)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%skernelcache", kDefaultCacheRecoveryHD);
			}
			else
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%sprelinkedkernel", kDefaultCachePathYosemite);
			}

			verbose("Kernel Cache file path (OS X 10.10): %s\n", kernelCacheFile);
		}
		else if ( MacOSVerCurrent >= MacOSVer2Int("10.11") )
		{
			// El Capitan on prelinked kernel cache file
			// for 10.10 and 10.11
			if (isOSXUpgrade)
			{
				if ( MacOSVerCurrent >= MacOSVer2Int("10.13") )
				{
					snprintf(kernelCacheFile, sizeof(kernelCacheFile),
 						"%sprelinkedkernel",
						"/macOS Install Data/Locked Files/Boot Files/");
				}
				else
				{
					snprintf(kernelCacheFile,
						sizeof(kernelCacheFile),
						"%sprelinkedkernel",
						"/OS X Install Data/");
				}

			}
			else if (isInstaller)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%sprelinkedkernel", kDefaultCacheInstallerNew);
			}
			else if (isRecoveryHD)
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%sprelinkedkernel", kDefaultCacheRecoveryHD);
			}
			else
			{
				snprintf(kernelCacheFile, sizeof(kernelCacheFile), "%sprelinkedkernel", kDefaultCachePathYosemite);
			}

			verbose("Kernel Cache file path (OS X 10.11 and newer): %s\n", kernelCacheFile);
		}
	}

	// Check if the kernel cache file exists
	ret = -1;

	if (gBootVolume->flags & kBVFlagBooter)
	{
		if (isRecoveryHD)
		{
			strncpy(kernelCachePath, "/com.apple.recovery.boot/prelinkedkernel", sizeof(kernelCachePath) );
			ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

			if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
			{
				strncpy(kernelCachePath, "/com.apple.recovery.boot/kernelcache", sizeof(kernelCachePath) );
				ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

				if ((flags & kFileTypeMask) != kFileTypeFlat)
				{
					ret = -1;
				}
			}
		}
		else if (isInstaller)
		{
			strncpy(kernelCachePath, "/.IABootFiles/prelinkedkernel", sizeof(kernelCachePath) );
			ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

			if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
			{
				strncpy(kernelCachePath, "/.IABootFiles/kernelcache", sizeof(kernelCachePath) );
				ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

				if ((flags & kFileTypeMask) != kFileTypeFlat)
				{
					ret = -1;
				}
			}
		}
		else if (isMacOSXUpgrade)
		{
			strncpy(kernelCachePath, "/Mac OS X Install Data/kernelcache", sizeof(kernelCachePath) );
			ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

			if ((flags & kFileTypeMask) != kFileTypeFlat)
			{
				ret = -1;
			}
		}
		else if (isOSXUpgrade)
		{
			strncpy(kernelCachePath,
				"/macOS Install Data/Locked Files/Boot Files/prelinkedkernel",
				sizeof(kernelCachePath) );
			ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

			if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
			{
				strncpy(kernelCachePath, "/OS X Install Data/prelinkedkernel", sizeof(kernelCachePath) );
				ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

				if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
				{
					strncpy(kernelCachePath, "/OS X Install Data/kernelcache", sizeof(kernelCachePath) );
					ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

					if ((flags & kFileTypeMask) != kFileTypeFlat)
					{
						ret = -1;
					}
				}
			}
		}
		else
		{
			snprintf(kernelCachePath, sizeof(kernelCachePath), "/com.apple.boot.P/%s", kernelCacheFile);
			ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);
			if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
			{
				snprintf(kernelCachePath, sizeof(kernelCachePath), "/com.apple.boot.R/%s", kernelCacheFile);
				ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

				if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
				{
					snprintf(kernelCachePath, sizeof(kernelCachePath), "/com.apple.boot.S/%s", kernelCacheFile);
					ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

					if ((flags & kFileTypeMask) != kFileTypeFlat)
					{
						ret = -1;
					}
				}
			}
		}
	}

	// If not found, use the original kernel cache path.
	if (ret == -1)
	{
		strlcpy(kernelCachePath, kernelCacheFile, sizeof(kernelCachePath));
		ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);

		if ((flags & kFileTypeMask) != kFileTypeFlat)
		{
			ret = -1;
		}
	}

	// Exit if kernel cache file wasn't found
	if (ret == -1)
	{
		DBG("No Kernel Cache File '%s' found\n", kernelCacheFile);
		return -1;
	}

	if ( !isInstaller && !isRecoveryHD && !isMacOSXUpgrade && !isOSXUpgrade )
	{
		// Check if the kernel cache file is more recent (mtime)
		// than the kernel file or the S/L/E directory
		ret = GetFileInfo(NULL, bootInfo->bootFile, &flags, &kerneltime);

		// Check if the kernel file is more recent than the cache file
		if ((ret == 0) && ((flags & kFileTypeMask) == kFileTypeFlat) && (kerneltime > cachetime))
		{
			DBG("Kernel file '%s' is more recent than Kernel Cache '%s'! Ignoring Kernel Cache.\n", bootInfo->bootFile, kernelCacheFile);
			return -1;
		}

		ret = GetFileInfo("/System/Library/", "Extensions", &flags, &exttime);

		// Check if the S/L/E directory time is more recent than the cache file
		if ((ret == 0) && ((flags & kFileTypeMask) == kFileTypeDirectory) && (exttime > cachetime))
		{
			DBG("Folder '/System/Library/Extensions' is more recent than Kernel Cache file '%s'! Ignoring Kernel Cache.\n", kernelCacheFile);
			return -1;
		}
	}

	// Since the kernel cache file exists and is the most recent try to load it
	DBG("Loading Kernel Cache from: '%s%s' (%s)\n", gBootVolume->label, gBootVolume->altlabel, gBootVolume->type_name);

	ret = LoadThinFatFile(kernelCachePath, binary);
	return ret; // ret contain the length of the binary
}

//==========================================================================
// This is the entrypoint from real-mode which functions exactly as it did
// before. Multiboot does its own runtime initialization, does some of its
// own things, and then calls common_boot.
void boot(int biosdev)
{
	// Enable A20 gate before accessing memory above 1Mb.
	// Note: malloc_init(), called via initialize_runtime() writes
	//   memory >= 1Mb, so A20 must be enabled before calling it. - zenith432
	zeroBSS();
	enableA20();
	malloc_init(0, 0, 0, malloc_error);
	common_boot(biosdev);
}

//==========================================================================
// The 'main' function for the booter. Called by boot0 when booting
// from a block device, or by the network booter.
//
// arguments:
//	 biosdev - Value passed from boot1/NBP to specify the device
//			   that the booter was loaded from.
//
// If biosdev is kBIOSDevNetwork, then this function will return if
// booting was unsuccessful. This allows the PXE firmware to try the
// next boot device on its list.
void common_boot(int biosdev)
{
	bool	 		quiet;
	bool	 		firstRun = true;
	bool	 		instantMenu;
	bool	 		rescanPrompt;
	int			status;
	unsigned int		allowBVFlags = kBVFlagSystemVolume | kBVFlagForeignBoot;
	unsigned int		denyBVFlags = kBVFlagEFISystem;

	// Set reminder to unload the PXE base code. Neglect to unload
	// the base code will result in a hang or kernel panic.
	gUnloadPXEOnExit = true;

	// Record the device that the booter was loaded from.
	gBIOSDev = biosdev & kBIOSDevMask;

	// Initialize boot-log
	initBooterLog();

#if DEBUG_INTERRUPTS
	// Enable interrupts
	interruptsAvailable = SetupInterrupts();
	if (interruptsAvailable)
	{
		EnableInterrupts();
	}
#endif

	// Initialize boot info structure.
	initKernBootStruct();

	// Setup VGA text mode.
	// Not sure if it is safe to call setVideoMode() before the
	// config table has been loaded. Call video_mode() instead.
#if DEBUG
	printf("before video_mode\n");
#endif
	video_mode( 2 );  // 80x25 mono text mode.
#if DEBUG
	printf("after video_mode\n");
#endif

	// Scan and record the system's hardware information.
	scan_platform();

	// First get info for boot volume.
	scanBootVolumes(gBIOSDev, 0);
	bvChain = getBVChainForBIOSDev(gBIOSDev);
	setBootGlobals(bvChain);

	// Load boot.plist config file
	status = loadChameleonConfig(&bootInfo->chameleonConfig, bvChain);

	if (getBoolForKey(kQuietBootKey, &quiet, &bootInfo->chameleonConfig) && quiet)
	{
		gBootMode |= kBootModeQuiet;
	}

	// Override firstRun to get to the boot menu instantly by setting "Instant Menu"=y in system config
	if (getBoolForKey(kInstantMenuKey, &instantMenu, &bootInfo->chameleonConfig) && instantMenu)
	{
		firstRun = false;
	}

	// Loading preboot ramdisk if exists.
	loadPrebootRAMDisk();

	// Disable rescan option by default
	gEnableCDROMRescan = false;

	// Enable it with Rescan=y in system config
	if (getBoolForKey(kRescanKey, &gEnableCDROMRescan, &bootInfo->chameleonConfig)	&& gEnableCDROMRescan)
	{
		gEnableCDROMRescan = true;
	}

	// Disable rescanPrompt option by default
	rescanPrompt = false;

	// Ask the user for Rescan option by setting "Rescan Prompt"=y in system config.
	if (getBoolForKey(kRescanPromptKey, &rescanPrompt , &bootInfo->chameleonConfig) && rescanPrompt && biosDevIsCDROM(gBIOSDev))
	{
		gEnableCDROMRescan = promptForRescanOption();
	}

	// Enable touching a single BIOS device only if "Scan Single Drive"=y is set in system config.
	if (getBoolForKey(kScanSingleDriveKey, &gScanSingleDrive, &bootInfo->chameleonConfig) && gScanSingleDrive)
	{
		gScanSingleDrive = true;
	}

	// Create a list of partitions on device(s).
	if (gScanSingleDrive)
	{
		scanBootVolumes(gBIOSDev, &bvCount);
	}
	else
	{
		scanDisks(gBIOSDev, &bvCount);
	}

	// Create a separated bvr chain using the specified filters.
	bvChain = newFilteredBVChain(0x80, 0xFF, allowBVFlags, denyBVFlags, &gDeviceCount);

	gBootVolume = selectBootVolume(bvChain);

	// Intialize module system
	init_module_system();

#if DEBUG
	printf("Default: %d, ->biosdev: %d, ->part_no: %d ->flags: 0x%08X\n",
		gBootVolume, gBootVolume->biosdev, gBootVolume->part_no, gBootVolume->flags);
	printf("bt(0,0): %d, ->biosdev: %d, ->part_no: %d ->flags: 0x%08X\n",
		gBIOSBootVolume, gBIOSBootVolume->biosdev, gBIOSBootVolume->part_no, gBIOSBootVolume->flags);
	getchar();
#endif

	useGUI = true;
	// Override useGUI default
	getBoolForKey(kGUIKey, &useGUI, &bootInfo->chameleonConfig);
	if (useGUI && initGUI())
	{
		// initGUI() returned with an error, disabling GUI.
		useGUI = false;
	}

	setBootGlobals(bvChain);

	// Parse args, load and start kernel.
	while (1)
	{
		bool		tryresume, tryresumedefault, forceresume;
		bool		useKernelCache = true; // by default try to use the prelinked kernel
		const char	*val;
		int			len, ret = -1;
		long		flags;
		u_int32_t	sleeptime, time;
		void		*binary = (void *)kLoadAddr;

		char		bootFile[sizeof(bootInfo->bootFile)];
		char		bootFilePath[512];
		char		kernelCacheFile[512];

		// Initialize globals.
		sysConfigValid = false;
		gErrors		   = false;

		status = getBootOptions(firstRun);
		firstRun = false;
		if (status == -1) continue;

		status = processBootOptions();
		// Status == 1 means to chainboot
		if ( status ==	1 ) break;
		// Status == -1 means that the config file couldn't be loaded or that gBootVolume is NULL
		if ( status == -1 )
		{
			// gBootVolume == NULL usually means the user hit escape.
			if (gBootVolume == NULL)
			{
				freeFilteredBVChain(bvChain);

				if (gEnableCDROMRescan)
				{
					rescanBIOSDevice(gBIOSDev);
				}

				bvChain = newFilteredBVChain(0x80, 0xFF, allowBVFlags, denyBVFlags, &gDeviceCount);
				setBootGlobals(bvChain);
				setupDeviceList(&bootInfo->themeConfig);
			}
			continue;
		}

		// Other status (e.g. 0) means that we should proceed with boot.

		// Turn off any GUI elements
		if ( bootArgs->Video.v_display == GRAPHICS_MODE )
		{
			gui.devicelist.draw = false;
			gui.bootprompt.draw = false;
			gui.menu.draw = false;
			gui.infobox.draw = false;
			gui.logo.draw = false;
			drawBackground();
			updateVRAM();
		}

		if (platformCPUFeature(CPU_FEATURE_EM64T))
		{
			archCpuType = CPU_TYPE_X86_64;
		}
		else
		{
			archCpuType = CPU_TYPE_I386;
		}

		if (getValueForKey(karch, &val, &len, &bootInfo->chameleonConfig))
		{
			if (strncmp(val, "x86_64", sizeof("x86_64") ) == 0)
			{
				archCpuType = CPU_TYPE_X86_64;
			}
			else if (strncmp(val, "i386", sizeof("i386") ) == 0)
			{
				archCpuType = CPU_TYPE_I386;
			}
			else
			{
				DBG("Incorrect parameter for option 'arch =' , please use x86_64 or i386\n");
			}
		}

		if (getValueForKey(kKernelArchKey, &val, &len, &bootInfo->chameleonConfig))
		{
			if (strncmp(val, "i386", sizeof("i386") ) == 0)
			{
				archCpuType = CPU_TYPE_I386;
			}
		}

		// Notify modules that we are attempting to boot
		execute_hook("PreBoot", NULL, NULL, NULL, NULL);

		//--------------------------------------
		getBoolForKey(kSkipKernelPatcher, &skipKernelPatcher, &bootInfo->chameleonConfig);

		// kernel patcher
		if (!skipKernelPatcher)
		{
			const char	*key = NULL;
			int          len = 0;
			setupKernelConfigFile("kernel.plist");

			// looking at command line for kernel patcher setting and override where needed
			if (getValueForKey(kKernelBooter_kexts, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kKernelBooter_kexts, &KernelBooter_kexts, &bootInfo->chameleonConfig);
			}
			key = NULL; len = 0;

			if (getValueForKey(kKernelPm, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kKernelPm, &KernelPm, &bootInfo->chameleonConfig);
			}
			key = NULL; len = 0;

			if (getValueForKey(kKernelLapicError, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kKernelLapicError, &KernelLapicError, &bootInfo->chameleonConfig);
			}
			key = NULL; len = 0;

			if (getValueForKey(kKernelLapicVersion, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kKernelLapicVersion, &KernelLapicVersion, &bootInfo->chameleonConfig);
			}
			key = NULL; len = 0;

			if (getValueForKey(kKernelHaswell, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kKernelHaswell, &KernelHaswell, &bootInfo->chameleonConfig);
			}
			key = NULL; len = 0;

			if (getValueForKey(kKernelcpuFamily, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kKernelcpuFamily, &KernelcpuFamily, &bootInfo->chameleonConfig);
			}
			key = NULL; len = 0;

			if (getValueForKey(kKernelSSE3, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kKernelSSE3, &KernelSSE3, &bootInfo->chameleonConfig);
			}
		}

#if KEXTPATCH_SUPPORT
		//-------------------------------------- kext patcher
		getBoolForKey(kSkipKextsPatcher, &skipKextsPatcher, &bootInfo->chameleonConfig);

		// kext patcher
		if (!skipKextsPatcher)
		{
			const char	*key = NULL;
			int		len = 0;
			setupKextConfigFile("kexts.plist");

			// looking at command line for kext patcher setting and override where needed
			if (getValueForKey(kAICPMPatch, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kAICPMPatch, &AICPMPatch, &bootInfo->chameleonConfig);
			}

			key = NULL;
			len = 0;
			if (getValueForKey(kNVIDIAWebDrv, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kNVIDIAWebDrv, &NVIDIAWebDrv, &bootInfo->chameleonConfig);
			}

			key = NULL;
			len = 0;
			if (getValueForKey(kOrangeIconFixSata, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kOrangeIconFixSata, &OrangeIconFixSata, &bootInfo->chameleonConfig);
			}

			key = NULL;
			len = 0;
			if (getValueForKey(kTrimEnablerSata, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kTrimEnablerSata, &TrimEnablerSata, &bootInfo->chameleonConfig);
			}

			key = NULL;
			len = 0;
			if (getValueForKey(kAppleRTCPatch, &key, &len, &bootInfo->chameleonConfig) && len > 0)
			{
				getBoolForKey(kAppleRTCPatch, &AppleRTCPatch, &bootInfo->chameleonConfig);
			}
		}

		// -------------------------------------- kext patcher
#endif

		if (gBootVolume->OSisInstaller)
		{
			isInstaller  = true;
		}

		if (gBootVolume->OSisMacOSXUpgrade)
		{
			isMacOSXUpgrade = true;
		}

		if (gBootVolume->OSisOSXUpgrade)
		{
			isOSXUpgrade    = true;
		}

		if (gBootVolume->OSisRecovery)
		{
			isRecoveryHD = true;
		}

		if ( !isRecoveryHD && !isInstaller && !isMacOSXUpgrade && !isOSXUpgrade )
		{
			if (!getBoolForKey (kWake, &tryresume, &bootInfo->chameleonConfig))
			{
				tryresume = true;
				tryresumedefault = true;
			}
			else
			{
				tryresumedefault = false;
			}

			if (!getBoolForKey (kForceWake, &forceresume, &bootInfo->chameleonConfig))
			{
				forceresume = false;
			}

			if (forceresume)
			{
				tryresume = true;
				tryresumedefault = false;
			}

			while (tryresume)
			{
				const char *tmp;
				BVRef bvr;
				if (!getValueForKey(kWakeImage, &val, &len, &bootInfo->chameleonConfig))
					val = "/private/var/vm/sleepimage";

				// Do this first to be sure that root volume is mounted
				ret = GetFileInfo(0, val, &flags, &sleeptime);

				if ((bvr = getBootVolumeRef(val, &tmp)) == NULL)
					break;

				// Can't check if it was hibernation Wake=y is required
				if (bvr->modTime == 0 && tryresumedefault)
					break;

				if ((ret != 0) || ((flags & kFileTypeMask) != kFileTypeFlat))
					break;

				if (!forceresume && ((sleeptime+3)<bvr->modTime))
				{
#if DEBUG
					printf ("Hibernate image is too old by %d seconds. Use ForceWake=y to override\n",
							bvr->modTime-sleeptime);
#endif
					break;
				}

				HibernateBoot((char *)val);
				break;
			}
		}

		if (!isRecoveryHD && !isInstaller && !isMacOSXUpgrade && !isOSXUpgrade )
		{
			getBoolForKey(kUseKernelCache, &useKernelCache, &bootInfo->chameleonConfig);
		}

		if (useKernelCache)
		{
			do
			{
				// Determine the name of the Kernel Cache
				if (getValueForKey(kKernelCacheKey, &val, &len, &bootInfo->bootConfig))
				{
					if (val[0] == '\\')
					{
						len--;
						val++;
					}
					/* FIXME: check len vs sizeof(kernelCacheFile) */
					strlcpy(kernelCacheFile, val, len + 1);
				}
				else
				{
					kernelCacheFile[0] = 0; // Use default kernel cache file
				}

				if (gOverrideKernel && kernelCacheFile[0] == 0)
				{
					DBG("Using a non default kernel (%s) without specifying 'Kernel Cache' path, KernelCache will not be used\n", bootInfo->bootFile);
					useKernelCache = false;
					break;
				}

				if (gMKextName[0] != 0)
				{
					DBG("Using a specific MKext Cache (%s), KernelCache will not be used\n", gMKextName);
					useKernelCache = false;
					break;
				}

				if (gBootFileType != kBlockDeviceType)
				{
					useKernelCache = false;
				}

			} while(0);
		}
		else
		{
			DBG("Kernel Cache using disabled by user.\n");
		}

		do
		{
			if (useKernelCache)
			{
				ret = LoadKernelCache(kernelCacheFile, &binary);
				if (ret >= 0)
				{
					break;
				}
			}

			bool bootFileWithDevice = false;
			// Check if bootFile start with a device ex: bt(0,0)/Extra/mach_kernel
			if (strncmp(bootInfo->bootFile, "bt(", sizeof("bt(") ) == 0 ||
				strncmp(bootInfo->bootFile, "hd(", sizeof("hd(") ) == 0 ||
				strncmp(bootInfo->bootFile, "rd(", sizeof("rd(") ) == 0)
			{
				bootFileWithDevice = true;
			}

			// bootFile must start with a / if it not start with a device name
			if (!bootFileWithDevice && (bootInfo->bootFile)[0] != '/')
			{
				if ( MacOSVerCurrent < MacOSVer2Int("10.10") ) // Mavericks and older
				{
					snprintf(bootFile, sizeof(bootFile), "/%s", bootInfo->bootFile); // append a leading /
				}
				else
				{
					// Yosemite and newer
					snprintf(bootFile, sizeof(bootFile), kDefaultKernelPathForYos"%s", bootInfo->bootFile); // Yosemite or El Capitan
				}
			}
			else
			{
				strlcpy(bootFile, bootInfo->bootFile, sizeof(bootFile));
			}

			// Try to load kernel image from alternate locations on boot helper partitions.
			ret = -1;
			if ((gBootVolume->flags & kBVFlagBooter) && !bootFileWithDevice)
			{
				snprintf(bootFilePath, sizeof(bootFilePath), "com.apple.boot.P%s", bootFile);
				ret = GetFileInfo(NULL, bootFilePath, &flags, &time);
				if (ret == -1)
				{
					snprintf(bootFilePath, sizeof(bootFilePath), "com.apple.boot.R%s", bootFile);
					ret = GetFileInfo(NULL, bootFilePath, &flags, &time);
					if (ret == -1)
					{
						snprintf(bootFilePath, sizeof(bootFilePath), "com.apple.boot.S%s", bootFile);
						ret = GetFileInfo(NULL, bootFilePath, &flags, &time);
					}
				}
			}
			if (ret == -1)
			{
				// No alternate location found, using the original kernel image path.
				strlcpy(bootFilePath, bootFile, sizeof(bootFilePath));
			}

			DBG("Loading kernel from: '%s' (%s)\n", gBootVolume->label, gBootVolume->type_name);
			ret = LoadThinFatFile(bootFilePath, &binary);
			if (ret <= 0 && archCpuType == CPU_TYPE_X86_64)
			{
				archCpuType = CPU_TYPE_I386;
				ret = LoadThinFatFile(bootFilePath, &binary);
			}
		} while (0);

		clearActivityIndicator();

#if DEBUG
		printf("Pausing...");
		sleep(8);
#endif

		if (ret <= 0)
		{
			printf("Can't find boot file: '%s'\n", bootFile);
			sleep(1);

			if (gBootFileType == kNetworkDeviceType)
			{
				// Return control back to PXE. Don't unload PXE base code.
				gUnloadPXEOnExit = false;
				break;
			}
			pause();

		}
		else
		{
			/* Won't return if successful. */
			ret = ExecKernel(binary);
		}
	}

	// chainboot
	if (status == 1)
	{
		// if we are already in graphics-mode,
		if (getVideoMode() == GRAPHICS_MODE)
		{
			setVideoMode( VGA_TEXT_MODE ); // switch back to text mode.
		}
	}

	if ((gBootFileType == kNetworkDeviceType) && gUnloadPXEOnExit)
	{
		nbpUnloadBaseCode();
	}

#if DEBUG_INTERRUPTS
	if (interruptsAvailable)
	{
		DisableInterrupts();
	}
#endif

}

// =========================================================================
//
void setupBooterArgs()
{
	bool KPRebootOption	= false; // I don't want this by default ( It makes me angry because I do not see the reason for the panic)+
//	bool HiDPIOption	= false; // (Disabled by default) 10.8+
//	bool FlagBlackOption	= false; // (Disabled by default) 10.10+

	// OS X Lion 10.7
	if ( MacOSVerCurrent >= MacOSVer2Int("10.7") ) // Lion and Up!
	{
		// Pike R. Alpha: Adding a 16 KB log space.
		bootArgs->performanceDataSize		= 0;
		bootArgs->performanceDataStart		= 0;

		// Pike R. Alpha: AppleKeyStore.kext
		bootArgs->keyStoreDataSize		= 0;
		bootArgs->keyStoreDataStart		= 0;

		bootArgs->bootMemSize			= 0;
		bootArgs->bootMemStart			= 0;
	}

	// OS X Mountain Lion 10.8
	if ( MacOSVerCurrent >= MacOSVer2Int("10.8") ) // Mountain Lion and Up!
	{
		// cparm
		getBoolForKey(kRebootOnPanic, &KPRebootOption, &bootInfo->chameleonConfig);
		if ( KPRebootOption )
		{
			bootArgs->flags |= kBootArgsFlagRebootOnPanic;
		}

		// cparm
		getBoolForKey(kEnableHiDPI, &HiDPIOption, &bootInfo->chameleonConfig);

		if ( HiDPIOption )
		{
			bootArgs->flags |= kBootArgsFlagHiDPI;
		}
	}

	// OS X Yosemite 10.10
	if ( MacOSVerCurrent >= MacOSVer2Int("10.10") ) // Yosemite and Up!
	{
		// Pike R. Alpha
		getBoolForKey(kBlackMode, &FlagBlackOption, &bootInfo->chameleonConfig);
		if ( FlagBlackOption )
		{
			// bootArgs->flags |= kBootArgsFlagBlack;
			bootArgs->flags |= kBootArgsFlagBlackBg; // Micky1979
		}
	}

	// OS X El Capitan 10.11
	if ( MacOSVerCurrent >= MacOSVer2Int("10.11") ) // El Capitan, Sierra and High Sierra!
	{
		// ErmaC
		verbose("\n");
		int	csrValue;

		/*
		 * A special BootArgs flag "kBootArgsFlagCSRBoot"
		 * is set in the Recovery or Installation environment.
		 * This flag is kind of overkill by turning off all the protections
		 */

		if (isRecoveryHD || isInstaller || isOSXUpgrade || isMacOSXUpgrade)
		{
			// SIP can be controlled with or without FileNVRAM.kext (Pike R. Alpha)
			bootArgs->flags	|=	(kBootArgsFlagCSRActiveConfig + kBootArgsFlagCSRConfigMode + kBootArgsFlagCSRBoot + kBootArgsFlagInstallUI);
		}
		else
		{
			bootArgs->flags	|= kBootArgsFlagCSRActiveConfig;
		}

		// Set limit to 8bit
		if ( getIntForKey(kCsrActiveConfig, &csrValue, &bootInfo->chameleonConfig) && (csrValue >= 0 && csrValue <= 255) )
		{
			bootArgs->csrActiveConfig	= csrValue;
			csrInfo(csrValue, 1);
		}
		else
		{
			// zenith432
			bootArgs->csrActiveConfig	= 0x67;
			csrInfo(0x67, 0);

		}


// ===============================================================================

		bootArgs->csrCapabilities		= CSR_VALID_CAPABILITIES; // CSR_VALID_FLAGS
		bootArgs->boot_SMC_plimit		= 0;
		bootArgs->bootProgressMeterStart	= 0;
		bootArgs->bootProgressMeterEnd		= 0;
	}
}

// =========================================================================
// ErmaC
void csrInfo(int csrValue, bool custom)
{
#define CSR_BITS 0x080

	int mask = CSR_BITS;
	verbose("System Integrity Protection status: %s ", (csrValue == 0) ? "enabled":"disabled");
	verbose("(%s Configuration).\nCsrActiveConfig = 0x%02X (", custom ? "Custom":"Default", csrValue);

	// Display integer number into binary using bitwise operator
	((csrValue & 0x020) == 0) ? verbose("0"): verbose("1");
	while (mask != 0)
	{
		( ((csrValue & mask) == 0) ? verbose("0"): verbose("1") );
		mask = mask >> 1; // Right Shift
	}
	verbose(")\n");
	if (csrValue != 0)
	{
		verbose("\nConfiguration:\n");
		verbose("\tKext Signing:            %s\n", ((csrValue & 0x001) == 0) ? "enabled" : "disabled");   /* (1 << 0) Allow untrusted kexts */
		verbose("\tFilesystem Protections:  %s\n", ((csrValue & 0x002) == 0) ? "enabled" : "disabled");   /* (1 << 1) Allow unrestricted file system. */
		verbose("\tTask for PID:            %s\n", ((csrValue & 0x004) == 0) ? "enabled" : "disabled");   /* (1 << 2) */
		verbose("\tDebugging Restrictions:  %s\n", ((csrValue & 0x008) == 0) ? "enabled" : "disabled");   /* (1 << 3) */
		verbose("\tApple Internal:          %s\n", ((csrValue & 0x010) == 0) ? "enabled" : "disabled");   /* (1 << 4) */
		verbose("\tDTrace Restrictions:     %s\n", ((csrValue & 0x020) == 0) ? "enabled" : "disabled");   /* (1 << 5) Allow unrestricted dtrace */
		verbose("\tNVRAM Protections:       %s\n", ((csrValue & 0x040) == 0) ? "enabled" : "disabled");   /* (1 << 6) Allow unrestricted NVRAM */
		verbose("\tDevice configuration:    %s\n", ((csrValue & 0x080) == 0) ? "enabled" : "disabled");   /* (1 << 7) Allow device configuration */
		verbose("\tBaseSystem Verification: %s\n", ((csrValue & 0x100) == 0) ? "enabled" : "disabled");   /* (1 << 8) Allow any Recovery OS */
//		verbose("\tApple Internal HS:       %s\n", ((csrValue & 0x200) == 0) ? "enabled" : "disabled");   /* (1 << 9) Allow Apple Internal HS */
	}
	verbose("\n");
}

// =========================================================================

/*!
	Selects a new BIOS device, taking care to update the global state appropriately.
 */
/*
static void selectBiosDevice(void)
{
	struct DiskBVMap *oldMap = diskResetBootVolumes(gBIOSDev);
	CacheReset();
	diskFreeMap(oldMap);
	oldMap = NULL;

	int dev = selectAlternateBootDevice(gBIOSDev);

	BVRef bvchain = scanBootVolumes(dev, 0);
	BVRef bootVol = selectBootVolume(bvchain);
	gBootVolume = bootVol;
	setRootVolume(bootVol);
	gBIOSDev = dev;
}
*/

// =========================================================================
bool checkOSVersion(const char *version)
{
	if ( '\0' != version[4] )
	{
		return !memcmp(&gMacOSVersion[0], &version[0], 5);
	}
	else
	{
		return !memcmp(&gMacOSVersion[0], &version[0], 4);
	}
}

uint32_t getMacOSVerCurrent()
{
	MacOSVerCurrent = MacOSVer2Int(gBootVolume->OSVersion);
	return MacOSVerCurrent;
}

// =========================================================================
unsigned long Adler32(unsigned char *buf, long len)
{
#define BASE 65521L /* largest prime smaller than 65536 */
#define NMAX 5000
// NMAX (was 5521) the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1

#define DO1(buf, i)	{s1 += buf[i]; s2 += s1;}
#define DO2(buf, i)	DO1(buf, i); DO1(buf, i + 1);
#define DO4(buf, i)	DO2(buf, i); DO2(buf, i + 2);
#define DO8(buf, i)	DO4(buf, i); DO4(buf, i + 4);
#define DO16(buf)	DO8(buf, 0); DO8(buf, 8);

	int k;

	unsigned long s1 = 1; // adler & 0xffff;
	unsigned long s2 = 0; // (adler >> 16) & 0xffff;
	unsigned long result;

	while (len > 0)
	{
		k = len < NMAX ? len : NMAX;
		len -= k;
		while (k >= 16)
		{
			DO16(buf);
			buf += 16;
			k -= 16;
		}

		if (k != 0)
		{
			do
			{
				s1 += *buf++;
				s2 += s1;
			} while (--k);
		}

		s1 %= BASE;
		s2 %= BASE;
	}

	result = (s2 << 16) | s1;

	return OSSwapHostToBigInt32(result);
}
