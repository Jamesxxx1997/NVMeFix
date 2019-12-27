//
// @file NVMeFix.cpp
//
// NVMeFix
//
// Copyright © 2019 acidanthera. All rights reserved.
//
// This program and the accompanying materials
// are licensed and made available under the terms and conditions of the BSD License
// which accompanies this distribution.  The full text of the license may be found at
// http://opensource.org/licenses/bsd-license.php
// THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
// WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver
 * Portions Copyright (c) 2011-2014, Intel Corporation.
 */

#include <IOKit/IOService.h> /* conficting old header from lilu */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOKitKeys.h>
#include <kern/assert.h>
#include <libkern/c++/OSMetaClass.h>

#include <Headers/kern_api.hpp>
#include <Headers/kern_disasm.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/hde64.h>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include "nvme.h"
#include "NVMeFixPlugin.hpp"

static NVMeFixPlugin plugin;

/**
 * This may be invoked before or after we get IOBSD mount notification, so in the both functions we
 * attempt to solve symbols and handle the controllers.
 */
void NVMeFixPlugin::processKext(void* that, KernelPatcher& patcher, size_t index, mach_vm_address_t,
								size_t) {
	auto plugin = static_cast<NVMeFixPlugin*>(that);
	assert(plugin);

	IOLockLock(plugin->lck);

	if (index != plugin->kextInfo.loadIndex) {
		IOLockUnlock(plugin->lck);
		return;
	}
	DBGLOG("nvmef", "processKext %s", plugin->kextInfo.id);

	plugin->kp = &patcher;

	if (plugin->solveSymbols())
		plugin->handleControllers();
	IOLockUnlock(plugin->lck);
}

bool NVMeFixPlugin::solveSymbols() {
	if (!kp)
		return false;

	bool res = true;
	res &= kextFuncs.IONVMeController__IssueIdentifyCommand.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.IONVMeController__ProcessSyncNVMeRequest.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.IONVMeController__GetRequest.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.AppleNVMeRequest__BuildCommandGetFeatures.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.AppleNVMeRequest__BuildCommandSetFeaturesCommon.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.IONVMeController__ReturnRequest.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.AppleNVMeRequest__GetStatus.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.AppleNVMeRequest__GetOpcode.solve(*kp, kextInfo.loadIndex) &&
	kextFuncs.AppleNVMeRequest__GenerateIOVMSegments.solve(*kp, kextInfo.loadIndex);

	auto offsetFromFunc = [](auto start, auto opcode, auto reg, auto rm, size_t ninsts_max=128) {
		assert(start);

		hde64s dis;

		for (size_t i = 0; i < ninsts_max; i++) {
			auto sz = Disassembler::hdeDisasm(start, &dis);

			if (dis.flags & F_ERROR)
				break;

			/* mov reg, [reg+disp] */
			if (dis.opcode == opcode && dis.modrm_reg == reg && dis.modrm_rm == rm)
				return dis.disp.disp32;

			start += sz;
		}

		return 0u;
	};

	/* mov eax, [rdi+0xA8] */
	if (!kextMembers.AppleNVMeRequest__result.offs)
		kextMembers.AppleNVMeRequest__result.offs = offsetFromFunc(kextFuncs.AppleNVMeRequest__GetStatus.fptr,
															   0x8b, 0, 7) + 4;
	kextMembers.AppleNVMeRequest__controller.offs = kextMembers.AppleNVMeRequest__result.offs - 12;

	/* movzx eax, byte ptr [rdi+0x10A] */
	if (!kextMembers.AppleNVMeRequest__command.offs)
		kextMembers.AppleNVMeRequest__command.offs = offsetFromFunc(kextFuncs.AppleNVMeRequest__GetOpcode.fptr,
		0xf, 0, 7);

	/* mov [rbx+0xC0], r12 */
	if (!kextMembers.AppleNVMeRequest__prpDescriptor.offs)
		kextMembers.AppleNVMeRequest__prpDescriptor.offs = offsetFromFunc(kextFuncs.IONVMeController__IssueIdentifyCommand.fptr, 0x89, 4, 3);

	res &= kextMembers.AppleNVMeRequest__result.offs &&
		kextMembers.AppleNVMeRequest__controller.offs &&
		kextMembers.AppleNVMeRequest__command.offs &&
		kextMembers.AppleNVMeRequest__prpDescriptor.offs;
	if (!res)
		DBGLOG("nvmef", "Failed to solve symbols");
	return res;
}

/**
 * This handler will be invoked when a media (whole disk or a partition) BSD node becomes registered.
 * We need to do two things now:
 * 1. Discover any undetected NVMe controllers.
 * 2. Try and solve symbols. If the relevant partition for symbol solving is not available, the call
 * will fail and we may succeed at next mount.
 * If we have all the symbols ready, we can proceed working with controllers.
 */
bool NVMeFixPlugin::matchingNotificationHandler(void* that, void* , IOService* service,
												IONotifier* notifier) {
	auto plugin = static_cast<NVMeFixPlugin*>(that);
	assert(plugin);
	assert(service);

	IOLockLock(plugin->lck);

//	DBGLOG("nvmef", "matchingNotificationHandler for %s", service->getName());

	auto parent = service->getProvider();

	/* Typical depth is 9 on real setups */
	for (int i = 0; parent && i < controllerSearchDepth; i++) {
//		DBGLOG("nvmef", "Parent %s", parent->getName());

		if (parent->metaCast("IONVMeController")) {
			bool has = false;

			for (size_t i = 0; i < plugin->controllers.size(); i++)
				if (plugin->controllers[i].controller == parent) {
					has = true;
					break;
				}

			if (!has) {
				plugin->controllers.push_back({parent, false});
				break;
			}
		}

		parent = parent->getProvider();
	}

//	DBGLOG("nvmef", "Discovered %u controllers", plugin->controllers.size());

	/* We will not try to solve already solved symbols, so it's ok to call this multiple times */
	if (plugin->solveSymbols())
		plugin->handleControllers();

	IOLockUnlock(plugin->lck);
	return true;
}

void NVMeFixPlugin::handleControllers() {
	DBGLOG("nvmef", "handleControllers for %u controllers", controllers.size());
	for (size_t i = 0; i < controllers.size(); i++)
		handleController(controllers[i]);
}

void NVMeFixPlugin::handleController(ControllerEntry& entry) {
	assert(entry.controller);

	if (entry.processed)
		return;

	/* No error signaling -- just ACK the discovery to notification handler */
	entry.processed = true;

	IOBufferMemoryDescriptor* identifyDesc {nullptr};

	if (identify(entry, identifyDesc) != kIOReturnSuccess) {
		SYSLOG("nvmef", "Failed to identify controller");
		return;
	}

	auto ctrl = reinterpret_cast<NVMe::nvme_id_ctrl*>(identifyDesc->getBytesNoCopy());
	if (!ctrl) {
		DBGLOG("nvmef", "Failed to get identify buffer bytes");
		identifyDesc->release();
		return;
	}

#ifdef DEBUG
	char mn[40];
	lilu_os_memcpy(mn, ctrl->mn, sizeof(mn));
	mn[sizeof(mn) - 1] = '\0';

	DBGLOG("nvmef", "Identified model %s", mn);
	DBGLOG("nvmef", "vid 0x%x ssvid 0x%x", ctrl->vid, ctrl->ssvid);
#endif

	bool apste {false};
	if (APSTenabled(entry, apste) == kIOReturnSuccess)
		DBGLOG("nvmef", "APST status %d", apste);

	if (!apste) {
		DBGLOG("nvmef", "Configuring APST");
		auto res = configureAPST(entry, ctrl);
		if (res != kIOReturnSuccess)
			DBGLOG("nvmef", "Failed to configure APST with 0x%x", res);
	}
	if (APSTenabled(entry, apste) == kIOReturnSuccess)
		DBGLOG("nvmef", "APST status %d", apste);

	entry.controller->setProperty("apst", apste);

	identifyDesc->release();
}

IOReturn NVMeFixPlugin::identify(ControllerEntry& entry, IOBufferMemoryDescriptor*& desc) {
	IOReturn ret = kIOReturnSuccess;

	uint8_t* data {nullptr};
	bool prepared {false};

	desc = IOBufferMemoryDescriptor::withCapacity(sizeof(NVMe::nvme_id_ctrl), kIODirectionIn);

	if (!desc) {
		SYSLOG("nvmef", "Failed to init descriptor");
		ret = kIOReturnNoResources;
		goto fail;
	}
	data = static_cast<uint8_t*>(desc->getBytesNoCopy());
	memset(data, '\0', desc->getLength());

	ret = desc->prepare();
	if (ret != kIOReturnSuccess) {
		SYSLOG("nvmef", "Failed to prepare descriptor");
		goto fail;
	}
	prepared = true;

	ret = kextFuncs.IONVMeController__IssueIdentifyCommand(entry.controller, desc, nullptr, 0);
	if (ret != kIOReturnSuccess) {
		SYSLOG("nvmef", "issueIdentifyCommand failed");
		goto fail;
	}

fail:
	if (prepared)
		desc->complete();
	return ret;
}

/* linux/drivers/nvme/host/core.c:nvme_configure_apst */
IOReturn NVMeFixPlugin::configureAPST(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl) {
	assert(ctrl);
	assert(entry.controller);

	if (!ctrl->apsta) {
		SYSLOG("apst", "APST unsupported by this controller");
		return kIOReturnUnsupported;
	}
	if (ctrl->npss > 31) {
		SYSLOG("apst", "Invalid NPSS");
		return kIOReturnUnsupported;
	}

	auto ret = kIOReturnSuccess;

	auto apstDesc = IOBufferMemoryDescriptor::withCapacity(sizeof(NVMe::nvme_feat_auto_pst), kIODirectionOut);

	if (!apstDesc) {
		SYSLOG("apst", "Failed to create APST table descriptor");
		return kIOReturnNoResources;
	}

	bool prepared {false};
	auto apstTable = reinterpret_cast<NVMe::nvme_feat_auto_pst*>(apstDesc->getBytesNoCopy());
	if (!apstTable) {
		SYSLOG("apst", "Failed to get table buffer");
		ret = kIOReturnNoResources;
	} else {
		ret = apstDesc->prepare();
		prepared = ret == kIOReturnSuccess;
	}

	constexpr uint64_t ps_max_latency_us {100000};

	int max_ps {-1};

	if (prepared) {
		memset(apstTable, '\0', sizeof(*apstTable));

		uint64_t target {0};
		uint64_t max_lat_us {0};

		/*
		* Walk through all states from lowest- to highest-power.
		* According to the spec, lower-numbered states use more
		* power.  NPSS, despite the name, is the index of the
		* lowest-power state, not the number of states.
		*/
		for (int state = ctrl->npss; state >= 0; state--) {
			if (target) {
				apstTable->entries[state] = target;
				DBGLOG("apst", "Set entry %d to 0x%llx", state, target);
			}

			/*
			 * Don't allow transitions to the deepest state
			 * if it's quirked off.
			 */
			// FIXME: add quirks
//			if (state == ctrl->npss &&
//			    (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS))
//				continue;

			/*
			 * Is this state a useful non-operational state for
			 * higher-power states to autonomously transition to?
			 */
			if (!(ctrl->psd[state].flags & NVMe::NVME_PS_FLAGS_NON_OP_STATE))
				continue;

			uint64_t exit_latency_us = ctrl->psd[state].exit_lat;
			if (exit_latency_us > ps_max_latency_us)
				continue;

			uint64_t total_latency_us = exit_latency_us + ctrl->psd[state].entry_lat;
			/*
			 * This state is good.  Use it as the APST idle
			 * target for higher power states.
			 */
			uint64_t transition_ms = total_latency_us + 19;
			transition_ms /= 20;
			if (transition_ms > (1ull << 24) - 1)
				transition_ms = (1ull << 24) - 1;

			target = (state << 3ull) | (transition_ms << 8ull);

			if (max_ps == -1)
				max_ps = state;

			if (total_latency_us > max_lat_us)
				max_lat_us = total_latency_us;
		}

		if (max_ps == -1)
			DBGLOG("apst", "No non-operational states are available");
		else
			DBGLOG("apst", "APST enabled: max PS = %d, max round-trip latency = %lluus\n",
				max_ps, max_lat_us);
	} else {
		SYSLOG("apst", "Failed to prepare table buffer");
	}

	if (max_ps != -1) {
		auto req = kextFuncs.IONVMeController__GetRequest(entry.controller, 1);

		if (!req) {
			DBGLOG("apst", "IONVMeController::GetRequest failed");
			ret = kIOReturnNoResources;
		} else
			ret = reinterpret_cast<IODMACommand*>(req)->setMemoryDescriptor(apstDesc);

		if (ret == kIOReturnSuccess) {
			kextFuncs.AppleNVMeRequest__BuildCommandSetFeaturesCommon(static_cast<void*&&>(req), NVMe::NVME_FEAT_AUTO_PST);

			kextMembers.AppleNVMeRequest__command.get(req)->features.dword11 = 1;
			*kextMembers.AppleNVMeRequest__prpDescriptor.get(req) = apstDesc;
			ret = reinterpret_cast<IODMACommand*>(req)->prepare(0, sizeof(*apstTable));

			if (ret != kIOReturnSuccess)
				DBGLOG("apst", "Failed to prepare DMA command");
			else {
				ret = kextFuncs.AppleNVMeRequest__GenerateIOVMSegments(static_cast<void*&&>(req), 0, sizeof(*apstTable));

				if (ret != kIOReturnSuccess)
					DBGLOG("apst", "Failed to generate IO VM segments");
				else {
					*kextMembers.AppleNVMeRequest__controller.get(req) = entry.controller;

					ret = kextFuncs.IONVMeController__ProcessSyncNVMeRequest(entry.controller, static_cast<void*&&>(req));
					if (ret != kIOReturnSuccess)
						DBGLOG("apst", "ProcessSyncNVMeRequest failed");
				}
			}
			reinterpret_cast<IODMACommand*>(req)->complete();
			kextFuncs.IONVMeController__ReturnRequest(entry.controller, static_cast<void*&&>(req));
		}
	}

	if (prepared)
		apstDesc->complete();
	if (apstDesc)
		apstDesc->release();
	return ret;
}

IOReturn NVMeFixPlugin::APSTenabled(ControllerEntry& entry, bool& enabled) {
	auto req = kextFuncs.IONVMeController__GetRequest(entry.controller, 1);

	if (!req) {
		SYSLOG("nvmef", "IONVMeController::GetRequest failed");
		return kIOReturnNoResources;
	}

	*kextMembers.AppleNVMeRequest__controller.get(req) = entry.controller;
	kextFuncs.AppleNVMeRequest__BuildCommandGetFeatures(static_cast<void*&&>(req), NVMe::NVME_FEAT_AUTO_PST);

	auto res = kextFuncs.IONVMeController__ProcessSyncNVMeRequest(entry.controller,
																  static_cast<void*&&>(req));
	if (res == kIOReturnSuccess)
		enabled = *kextMembers.AppleNVMeRequest__result.get(req);
	if (req)
		kextFuncs.IONVMeController__ReturnRequest(entry.controller, static_cast<void*&&>(req));
	return res;
}

/* Notifications are serialized for a single controller, so we don't have to sync with removal */
bool NVMeFixPlugin::terminatedNotificationHandler(void* that, void* , IOService* service,
												IONotifier* notifier) {
	auto plugin = static_cast<NVMeFixPlugin*>(that);
	assert(plugin);
	assert(service && service->metaCast("IONVMeController"));

	for (size_t i = 0; i < plugin->controllers.size(); i++)
		if (plugin->controllers[i].controller == service) {
			plugin->controllers.erase(i);
			break;
		}

	return false;
}

/**
 * NOTE: We are in kmod context, not IOService. This works fine as long as we publish our personality
 * in Info.plist to match something in ioreg, but specify a non-existing IOClass so that IOKit attempts
 * to load us anyway. It is otherwise unsafe to use matching notifications from kmod when we have a
 * living IOService.
 */
void NVMeFixPlugin::init() {
	LiluAPI::Error err;

	if (!(lck = IOLockAlloc())) {
		SYSLOG("nvmef", "Failed to alloc lock");
		goto fail;
	}

	matchingNotifier = IOService::addMatchingNotification(gIOPublishNotification,
							IOService::serviceMatching("IOMediaBSDClient"),
							matchingNotificationHandler,
						    this);
	if (!matchingNotifier) {
		SYSLOG("nvmef", "Failed to register for matching notification");
		goto fail;
	}

	terminationNotifier = IOService::addMatchingNotification(gIOWillTerminateNotification,
							IOService::serviceMatching("IONVMeController"),
							terminatedNotificationHandler,
						    this);
	if (!terminationNotifier) {
		SYSLOG("nvmef", "Failed to register for termination notification");
		goto fail;
	}

	DBGLOG("nvmef", "Registered for matching notifications");

	err = lilu.onKextLoad(&kextInfo, 1, NVMeFixPlugin::processKext, this);
	if (err != LiluAPI::Error::NoError) {
		SYSLOG("nvmef", "Failed to register kext load cb");
		goto fail;
	}

	return;
fail:
	if (lck)
		IOLockFree(lck);
	if (matchingNotifier)
		matchingNotifier->remove();
	if (terminationNotifier)
		terminationNotifier->remove();
}

void NVMeFixPlugin::deinit() {
	/* This kext is not unloadable */
	panic("nvmef: deinit called");
}

static const char *bootargOff[] {
	"-nvmefoff"
};

static const char *bootargDebug[] {
	"-nvmefdbg"
};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	nullptr,
	0,
	KernelVersion::Mojave,
	KernelVersion::Catalina,
	[]() {
		plugin.init();
	}
};