// TouchLink's Meta source: the Touches as Meta's runtime tracks them (touchlink.exe meta, meta.cpp),
// handed to the driver (source.cpp) in shared memory Local\TouchLinkMeta. The helper writes `hands`
// (bumping `sequence` before and after: odd while writing); the driver writes `haptic`.
#pragma once

#include <stdint.h>

namespace tl {

constexpr uint32_t kMetaMagic = 0x4D4C5454;   // "TTLM"
constexpr uint32_t kMetaVersion = 1;
constexpr wchar_t kMetaMapping[] = L"Local\\TouchLinkMeta";
// Held by the one running helper. Meta's runtime gives the Rift's display (and with it the Touches'
// buttons) to the app that takes it first, before SteamVR starts, so the Atrium app may start the
// helper early; the driver then uses that one instead of starting another.
constexpr wchar_t kMetaHelperMutex[] = L"Local\\TouchLinkMetaHelper";

// Bits of MetaHand::buttons / touches.
enum MetaButton : uint32_t {
	kMetaLower = 1 << 0,       // A / X
	kMetaUpper = 1 << 1,       // B / Y
	kMetaStick = 1 << 2,       // stick click
	kMetaMenu = 1 << 3,        // left only
	kMetaTrigger = 1 << 4,     // touches: trigger
	kMetaThumbrest = 1 << 5,   // touches: thumb rest
	kMetaOculus = 1 << 6,      // right only: the Oculus button (read from OVRServer's log: LibOVR keeps it from apps)
};

struct MetaHand {
	uint32_t connected;
	uint32_t positionTracked;   // the sensors see it now
	uint32_t valid;             // orientation and position known
	float orientation[4];       // x y z w, Meta's floor-level tracking space
	float position[3];
	float linearVelocity[3];    // in that space
	float angularVelocity[3];   // in that space (turned from the controller's axes)
	uint32_t buttons, touches;
	float trigger, grip, stick[2];
};

struct MetaHaptic {
	uint32_t sequence;   // bumped for each new request
	float amplitude;     // 0..1
	float seconds;
};

struct MetaShared {
	uint32_t magic, version;
	volatile int64_t sequence;   // odd while the helper writes
	int64_t qpc;                 // QueryPerformanceCounter when the poses were taken
	uint32_t hmdMounted;         // Meta's runtime thinks the Rift is worn (it only tracks the Touches then)
	uint32_t focus;              // the helper has input focus (rumble needs it)
	MetaHand hands[2];
	MetaHaptic haptic[2];        // written by the driver
	char status[128];            // a line for touchlink status / Atrium
};

} // namespace tl
