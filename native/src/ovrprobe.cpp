// ovrprobe: what does a normal Oculus app see? Prints the active headset as LibOVR reports it.
//   ovrprobe          one report
//   ovrprobe track    also print the head and hand poses for a few seconds
#include <OVR_CAPI.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <windows.h>

static const char* TypeName(ovrHmdType t) {
    switch (t) {
    case ovrHmd_None: return "None";
    case ovrHmd_DK2: return "DK2";
    case ovrHmd_CV1: return "CV1";
    case ovrHmd_RiftS: return "RiftS";
    case ovrHmd_Quest: return "Quest";
    case ovrHmd_Quest2: return "Quest2";
    default: return "other";
    }
}

static double Deg(float tan) { return atan(tan) * 180.0 / 3.14159265358979; }

int main(int argc, char** argv) {
    ovrInitParams ip = {};
    ip.Flags = ovrInit_Invisible;  // no VR focus, no Dash: just read
    ovrResult r = ovr_Initialize(&ip);
    if (OVR_FAILURE(r)) { ovrErrorInfo e; ovr_GetLastErrorInfo(&e); printf("ovr_Initialize failed %d %s\n", r, e.ErrorString); return 1; }
    ovrSession s; ovrGraphicsLuid luid;
    r = ovr_Create(&s, &luid);
    if (OVR_FAILURE(r)) { ovrErrorInfo e; ovr_GetLastErrorInfo(&e); printf("ovr_Create failed %d %s\n", r, e.ErrorString); return 1; }

    ovrHmdDesc d = ovr_GetHmdDesc(s);
    if (argc > 1 && !strcmp(argv[1], "json")) {  // one line for Atrium
        unsigned int c = ovr_GetConnectedControllerTypes(s);
        ovrSessionStatus st = {};
        ovr_GetSessionStatus(s, &st);
        printf("{\"type\":%d,\"typeName\":\"%s\",\"product\":\"%s\",\"manufacturer\":\"%s\",\"vid\":%d,\"pid\":%d,\"serial\":\"%s\","
               "\"firmware\":\"%d.%d\",\"width\":%d,\"height\":%d,\"refresh\":%.2f,\"ltouch\":%s,\"rtouch\":%s,\"remote\":%s,"
               "\"trackers\":%d,\"hmdPresent\":%s,\"mounted\":%s}\n",
               d.Type, TypeName(d.Type), d.ProductName, d.Manufacturer, d.VendorId, d.ProductId, d.SerialNumber,
               d.FirmwareMajor, d.FirmwareMinor, d.Resolution.w, d.Resolution.h, d.DisplayRefreshRate,
               (c & ovrControllerType_LTouch) ? "true" : "false", (c & ovrControllerType_RTouch) ? "true" : "false",
               (c & ovrControllerType_Remote) ? "true" : "false", ovr_GetTrackerCount(s),
               st.HmdPresent ? "true" : "false", st.HmdMounted ? "true" : "false");
        ovr_Destroy(s);
        ovr_Shutdown();
        return 0;
    }
    printf("Type          %s (%d)\n", TypeName(d.Type), d.Type);
    printf("Product       %s\n", d.ProductName);
    printf("Manufacturer  %s\n", d.Manufacturer);
    printf("VID/PID       %04x/%04x\n", d.VendorId, d.ProductId);
    printf("Serial        %s\n", d.SerialNumber);
    printf("Firmware      %d.%d\n", d.FirmwareMajor, d.FirmwareMinor);
    printf("Resolution    %d x %d\n", d.Resolution.w, d.Resolution.h);
    printf("Refresh       %.2f Hz\n", d.DisplayRefreshRate);
    for (int e = 0; e < 2; e++) {
        ovrFovPort f = d.DefaultEyeFov[e];
        ovrSizei tex = ovr_GetFovTextureSize(s, (ovrEyeType)e, f, 1.0f);
        ovrEyeRenderDesc rd = ovr_GetRenderDesc(s, (ovrEyeType)e, f);
        printf("%s eye FOV  up %.1f down %.1f left %.1f right %.1f  (h %.1f, v %.1f) tex %dx%d, offset x %+.4f m\n", e ? "Right" : "Left ",
               Deg(f.UpTan), Deg(f.DownTan), Deg(f.LeftTan), Deg(f.RightTan),
               Deg(f.LeftTan) + Deg(f.RightTan), Deg(f.UpTan) + Deg(f.DownTan), tex.w, tex.h, rd.HmdToEyePose.Position.x);
    }
    unsigned int ct = ovr_GetConnectedControllerTypes(s);
    printf("Controllers   0x%x%s%s\n", ct, (ct & ovrControllerType_LTouch) ? " LTouch" : "", (ct & ovrControllerType_RTouch) ? " RTouch" : "");
    int n = ovr_GetTrackerCount(s);
    printf("Trackers      %d\n", n);

    if (argc > 1 && !strcmp(argv[1], "track")) {
        for (int i = 0; i < 20; i++) {
            ovrTrackingState ts = ovr_GetTrackingState(s, ovr_GetTimeInSeconds(), ovrFalse);
            ovrInputState in = {};
            ovr_GetInputState(s, ovrControllerType_Touch, &in);
            ovrPosef h = ts.HeadPose.ThePose, rh = ts.HandPoses[1].ThePose;
            printf("head %+.3f %+.3f %+.3f q %+.3f %+.3f %+.3f %+.3f st %x | rhand %+.3f %+.3f %+.3f st %x | btn %x trig %.2f %.2f stick %.2f %.2f\n",
                   h.Position.x, h.Position.y, h.Position.z, h.Orientation.x, h.Orientation.y, h.Orientation.z, h.Orientation.w, ts.StatusFlags,
                   rh.Position.x, rh.Position.y, rh.Position.z, ts.HandStatusFlags[1],
                   in.Buttons, in.IndexTrigger[0], in.IndexTrigger[1], in.Thumbstick[1].x, in.Thumbstick[1].y);
            Sleep(250);
        }
    }
    ovr_Destroy(s);
    ovr_Shutdown();
    return 0;
}
