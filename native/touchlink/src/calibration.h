// TouchLink calibration: the Touch tracking space (Rift sensors) -> SteamVR's raw space (base
// stations), from a Touch held still against the headset while the user looks around.
#pragma once

#include "common.h"

namespace tl {

// One moment: the headset (SteamVR raw space) and the Touch held to it (Touch space), with the
// Touch's velocities so other delays between the two trackers can be tried.
struct CalSample { XrPosef hmd, touch; XrVector3f angVel, linVel; };

struct CalResult {
	bool ok = false;
	XrPosef space = kIdentity;   // Touch space -> SteamVR raw space
	XrPosef onHead = kIdentity;  // the Touch in the headset's frame (a check: within reach)
	double rmsMm = 0, lagMs = 0, spread = 0, turnedDeg = 0;
	char why[160] = "";
};

// level: both spaces stand on gravity (Meta's source), so only the turn about the vertical is fitted.
CalResult Solve(const std::vector<CalSample>& track, bool level);

struct Calibration {
	bool valid = false;
	XrPosef space = kIdentity;
	double errorMm = 0;
	uint64_t universe = 0;   // SteamVR's tracking universe it was made in (0: unknown)
};

// Each source has its own tracking space, so its own calibration: calibration.json (Atrium's
// tracking service), calibration-meta.json (Meta's runtime). Switching keeps the other one.
inline std::wstring CalibrationFile()
{
	return DataFile(LoadConfig().meta ? L"calibration-meta.json" : L"calibration.json");
}

Calibration LoadCalibration();
bool SaveCalibration(const Calibration& c, const CalResult& r, const char* hand);

} // namespace tl
