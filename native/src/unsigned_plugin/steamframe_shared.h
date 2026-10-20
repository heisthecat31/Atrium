// Shared state between SteamFrameSim.exe (the headset) and SteamFrame.dll (inside OVRServer).
// Both run as the same user in the same session, so a Local\ mapping is enough.
#pragma once
#include <stdint.h>

#define SF_MAP_NAME L"Local\\SteamFrameSim"
#define SF_MAGIC 0x4D415246u  // 'FRAM'
#define SF_VERSION 1

// Steam Frame controller buttons, mapped onto Touch.
enum SfButton : uint32_t {
    SF_BTN_A = 1u << 0,      // A (right) / X (left)
    SF_BTN_B = 1u << 1,      // B (right) / Y (left)
    SF_BTN_MENU = 1u << 2,   // menu / home
    SF_BTN_STICK = 1u << 3,  // thumbstick click
};

struct SfPose {
    float qx, qy, qz, qw;  // orientation
    float px, py, pz;      // position, meters, eye-level origin
};

struct SfHand {
    SfPose pose;
    uint32_t buttons;  // SfButton bits
    float index_trigger;  // 0 released .. 1 pulled
    float grip;           // 0 .. 1
    float stick_x, stick_y;
    uint32_t connected;
};

struct SfShared {
    uint32_t magic, version;
    volatile uint32_t seq;        // bumped by the sim on every update
    volatile uint32_t heartbeat;  // bumped by the plugin every tick, so the sim can show "connected to OVRServer"
    uint32_t sim_pid;
    uint32_t worn;  // proximity sensor
    SfPose head;
    SfHand hand[2];  // 0 left, 1 right
    // Display: the plugin copies every presented frame into this shared texture (NT handle name below).
    volatile uint32_t frame_seq;
    uint32_t frame_w, frame_h;
    wchar_t frame_name[64];
};
