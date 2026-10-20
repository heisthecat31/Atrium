// nvdisplays: every display an NVIDIA GPU has connected, including ones it keeps from Windows
// (headsets reserved for NVIDIA's direct mode), with the maker code from each EDID. Read-only.
// Uses the public NVAPI (github.com/NVIDIA/nvapi).
#include <windows.h>
#include <stdio.h>
#include "nvapi.h"

#pragma comment(lib, "nvapi64.lib")

// nvdisplays acquire <displayId hex>: try to take a display through the public dedicated-display
// API, report the result, release it again.
static int TryAcquire(NvU32 displayId)
{
	NvU64 source = 0;
	NvAPI_Status st = NvAPI_DISP_AcquireDedicatedDisplay(displayId, &source);
	NvAPI_ShortString msg = {};
	NvAPI_GetErrorMessage(st, msg);
	printf("AcquireDedicatedDisplay(0x%08x): %d %s, DisplaySource handle 0x%llx\n", displayId, (int)st, msg, (unsigned long long)source);
	if (st == NVAPI_OK) {
		st = NvAPI_DISP_ReleaseDedicatedDisplay(displayId);
		printf("ReleaseDedicatedDisplay: %d\n", (int)st);
		if (source) CloseHandle((HANDLE)source);
	}
	return 0;
}

// nvdisplays setedid <displayId hex>: override the display's EDID with a copy whose maker code is
// "ATR" instead of "OVR", so the driver no longer recognises the headset and hands it to Windows
// as a plain monitor. Public NVAPI, needs Administrator; lasts until reboot.
// nvdisplays clearedid <displayId hex>: remove the override again.
static int SetEdid(NvU32 displayId, bool clear)
{
	NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS];
	NvU32 gpuCount = 0;
	if (NvAPI_EnumPhysicalGPUs(gpus, &gpuCount) != NVAPI_OK || !gpuCount) return 1;
	NV_EDID edid = {};
	edid.version = NV_EDID_VER;
	if (!clear) {
		NvAPI_Status st = NvAPI_GPU_GetEDID(gpus[0], displayId, &edid);
		if (st != NVAPI_OK) { printf("GetEDID(0x%08x): %d\n", displayId, (int)st); return 1; }
		NvU8* e = edid.EDID_Data;
		const char* atr = "ATR";                       // 5-bit letters, A = 1
		NvU16 m = (NvU16)((atr[0] - 'A' + 1) << 10 | (atr[1] - 'A' + 1) << 5 | (atr[2] - 'A' + 1));
		e[8] = (NvU8)(m >> 8);
		e[9] = (NvU8)m;
		NvU8 sum = 0;
		for (int i = 0; i < 127; i++) sum += e[i];
		e[127] = (NvU8)(256 - sum);
		edid.edidId = 0;
		edid.offset = 0;
	} else {
		edid.sizeofEDID = 0;
	}
	NvAPI_Status st = NvAPI_GPU_SetEDID(gpus[0], displayId, &edid);
	NvAPI_ShortString msg = {};
	NvAPI_GetErrorMessage(st, msg);
	printf("SetEDID(0x%08x, %s): %d %s\n", displayId, clear ? "clear" : "maker ATR", (int)st, msg);
	return st == NVAPI_OK ? 0 : 1;
}

int main(int argc, char** argv)
{
	if (argc > 2 && (!strcmp(argv[1], "setedid") || !strcmp(argv[1], "clearedid"))) {
		if (NvAPI_Initialize() != NVAPI_OK) return 1;
		int r = SetEdid((NvU32)strtoul(argv[2], nullptr, 16), !strcmp(argv[1], "clearedid"));
		NvAPI_Unload();
		return r;
	}
	if (argc > 2 && !strcmp(argv[1], "acquire")) {
		if (NvAPI_Initialize() != NVAPI_OK) return 1;
		int r = TryAcquire((NvU32)strtoul(argv[2], nullptr, 16));
		NvAPI_Unload();
		return r;
	}
	if (NvAPI_Initialize() != NVAPI_OK) {
		printf("NVAPI unavailable (no NVIDIA driver?)\n");
		return 1;
	}
	NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS];
	NvU32 gpuCount = 0;
	NvAPI_EnumPhysicalGPUs(gpus, &gpuCount);
	for (NvU32 g = 0; g < gpuCount; g++) {
		NvAPI_ShortString name = {};
		NvAPI_GPU_GetFullName(gpus[g], name);
		printf("GPU %u: %s\n", g, name);
		NvU32 count = 0;
		NvAPI_GPU_GetConnectedDisplayIds(gpus[g], nullptr, &count, NV_GPU_CONNECTED_IDS_FLAG_UNCACHED);
		if (!count) continue;
		NV_GPU_DISPLAYIDS* ids = new NV_GPU_DISPLAYIDS[count]();
		for (NvU32 i = 0; i < count; i++) ids[i].version = NV_GPU_DISPLAYIDS_VER;
		NvAPI_GPU_GetConnectedDisplayIds(gpus[g], ids, &count, NV_GPU_CONNECTED_IDS_FLAG_UNCACHED);
		for (NvU32 i = 0; i < count; i++) {
			NV_EDID edid = {};
			edid.version = NV_EDID_VER;
			char maker[4] = "???";
			char product[14] = "";
			if (NvAPI_GPU_GetEDID(gpus[g], ids[i].displayId, &edid) == NVAPI_OK) {
				const NvU8* e = edid.EDID_Data;
				NvU16 m = (NvU16)(e[8] << 8 | e[9]);
				maker[0] = (char)('A' - 1 + ((m >> 10) & 31));
				maker[1] = (char)('A' - 1 + ((m >> 5) & 31));
				maker[2] = (char)('A' - 1 + (m & 31));
				for (int d = 54; d <= 108; d += 18)           // descriptor blocks: 0xFC = product name
					if (e[d] == 0 && e[d + 1] == 0 && e[d + 3] == 0xFC) {
						int n = 0;
						for (int k = 5; k < 18 && e[d + k] != 0x0A; k++) product[n++] = (char)e[d + k];
						product[n] = 0;
					}
			}
			printf("  display 0x%08x  connector %d  %s%s%s  EDID maker %s  product \"%s\"\n", ids[i].displayId,
				(int)ids[i].connectorType, ids[i].isConnected ? "connected" : "not connected",
				ids[i].isOSVisible ? ", visible to Windows" : ", HIDDEN from Windows", ids[i].isActive ? ", active" : "",
				maker, product);
		}
		delete[] ids;

		// Every display id the GPU has, connected or not (hidden headset outputs included, if any).
		NvU32 all = 0;
		NvAPI_GPU_GetAllDisplayIds(gpus[g], nullptr, &all);
		NV_GPU_DISPLAYIDS* allIds = new NV_GPU_DISPLAYIDS[all ? all : 1]();
		for (NvU32 i = 0; i < all; i++) allIds[i].version = NV_GPU_DISPLAYIDS_VER;
		NvAPI_GPU_GetAllDisplayIds(gpus[g], allIds, &all);
		printf("  all display ids: %u\n", all);
		for (NvU32 i = 0; i < all; i++) {
			printf("    0x%08x connector %d%s%s%s", allIds[i].displayId, (int)allIds[i].connectorType,
				allIds[i].isConnected ? " connected" : "", allIds[i].isPhysicallyConnected ? " physically" : "",
				allIds[i].isOSVisible ? " os-visible" : "");
			NV_EDID edid = {};
			edid.version = NV_EDID_VER;
			NvAPI_Status es = NvAPI_GPU_GetEDID(gpus[g], allIds[i].displayId, &edid);
			if (es == NVAPI_OK) {
				const NvU8* e = edid.EDID_Data;
				NvU16 m = (NvU16)(e[8] << 8 | e[9]);
				printf("  EDID %c%c%c product 0x%04x, %u bytes", 'A' - 1 + ((m >> 10) & 31), 'A' - 1 + ((m >> 5) & 31),
					'A' - 1 + (m & 31), e[10] | e[11] << 8, edid.sizeofEDID);
			} else {
				printf("  (no EDID: %d)", (int)es);
			}
			printf("\n");
		}
		delete[] allIds;
	}
	// Displays the driver keeps for one application ("dedicated displays", NVAPI R495+); one of
	// these can be acquired with NvAPI_DISP_AcquireDedicatedDisplay and presented to through
	// Windows.Devices.Display.Core.
	NvU32 dcount = 0;
	NvAPI_Status st = NvAPI_DISP_GetNvManagedDedicatedDisplays(&dcount, nullptr);
	printf("NV-managed dedicated displays: %u (status %d)\n", dcount, (int)st);
	if (st == NVAPI_OK && dcount) {
		NV_MANAGED_DEDICATED_DISPLAY_INFO* dd = new NV_MANAGED_DEDICATED_DISPLAY_INFO[dcount]();
		for (NvU32 i = 0; i < dcount; i++) dd[i].version = NV_MANAGED_DEDICATED_DISPLAY_INFO_VER;
		NvAPI_DISP_GetNvManagedDedicatedDisplays(&dcount, dd);
		for (NvU32 i = 0; i < dcount; i++) {
			NV_MANAGED_DEDICATED_DISPLAY_METADATA meta = {};
			meta.version = NV_MANAGED_DEDICATED_DISPLAY_METADATA_VER;
			meta.displayId = dd[i].displayId;
			NvAPI_DISP_GetNvManagedDedicatedDisplayMetadata(&meta);
			printf("  dedicated display 0x%08x%s%s  name \"%s\"\n", dd[i].displayId, dd[i].isAcquired ? ", acquired" : "",
				dd[i].isMosaic ? ", mosaic" : "", meta.name);
		}
		delete[] dd;
	}
	NvAPI_Unload();
	return 0;
}
