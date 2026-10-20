// riftdisplay: list the displays Windows keeps for headsets ("specialized displays", hidden from
// the desktop) through Windows.Devices.Display.Core. The Atrium Runtime will claim the Rift's
// panel this way to show frames itself, without Meta's compositor. Read-only: lists, claims nothing.
#include <windows.h>
#include <stdio.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Graphics.h>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Devices::Display;
using namespace winrt::Windows::Devices::Display::Core;

static const char* Usage(DisplayMonitorUsageKind k)
{
	switch (k) {
	case DisplayMonitorUsageKind::Standard: return "standard";
	case DisplayMonitorUsageKind::HeadMounted: return "head-mounted";
	case DisplayMonitorUsageKind::SpecialPurpose: return "special purpose";
	default: return "?";
	}
}

int main()
{
	init_apartment();
	DisplayManager manager = DisplayManager::Create(DisplayManagerOptions::None);
	auto targets = manager.GetCurrentTargets();
	printf("%u display targets\n", targets.Size());
	for (DisplayTarget t : targets) {
		DisplayMonitor m = t.TryGetMonitor();
		printf("- %s, %s%s\n", t.IsConnected() ? "connected" : "not connected", Usage(t.UsageKind()),
			t.IsStale() ? ", stale" : "");
		printf("  stable monitor id: %ls\n", t.StableMonitorId().c_str());
		if (m) {
			auto res = m.NativeResolutionInRawPixels();
			printf("  monitor: %ls (%ls), native %dx%d, connector %d\n", m.DisplayName().c_str(), m.DeviceId().c_str(),
				res.Width, res.Height, (int)m.PhysicalConnector());
		}
	}
	return 0;
}
