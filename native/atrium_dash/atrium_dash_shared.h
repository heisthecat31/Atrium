// What atrium-dash (the dashboard) and the Atrium Runtime (inside each game) share.
//
// The dash is a 3D scene (curved panel, toolbar, controller models, lasers) that atrium-dash
// renders in stereo. Each frame the game side (the runtime inside whatever game is running)
// writes where the eyes and controllers are, in a LOCAL reference space it owns; atrium-dash
// renders the scene from those views (two eyes side by side, premultiplied alpha, transparent
// where there's no dash) and publishes each finished picture into one of three shared textures
// (kSlots: Local\AtriumDashTexture0..2), with the views it was rendered from. The game side
// copies the latest finished one into a projection layer drawn over the game, posed with those
// views, so the runtime's reprojection keeps it steady. Three slots: the dash never draws into
// the picture the game is copying (one shared picture tore and jumped). Home (no game running) is atrium-dash's
// own OpenXR session filling the same block.
//
// Local\ names: one per Windows session. Each field is written by one side only (noted).
#pragma once
#include <stdint.h>

#define ATRIUM_DASH_SHARED L"Local\\AtriumDashShared"
#define ATRIUM_DASH_TEXTURE L"Local\\AtriumDashTexture" // + slot number
#define ATRIUM_DASH_SLOTS 3
#define ATRIUM_DASH_MAGIC 0x48534144u // "DASH"
#define ATRIUM_DASH_VERSION 4u

struct AtriumDashPose
{
	float px, py, pz;
	float qx, qy, qz, qw;
};

struct AtriumDashFov
{
	float left, right, up, down; // radians, as OpenXR's XrFovf
};

struct AtriumDashInput
{
	AtriumDashPose eye[2];
	AtriumDashFov fov[2];
	AtriumDashPose aim[2]; // controllers' pointing poses (0 left, 1 right)
	AtriumDashPose grip[2];
	int32_t handValid[2];
	float trigger[2];  // 0..1
	float squeeze[2];  // 0..1
	float thumbY[2];   // stick up/down (scrolling)
	int32_t valid;     // eyes are valid
};

struct AtriumDashShared
{
	uint32_t magic;   // dash: ATRIUM_DASH_MAGIC once the texture exists
	uint32_t version; // dash: ATRIUM_DASH_VERSION
	uint32_t eyeWidth;  // dash: one eye's size in pixels (the texture is 2 * eyeWidth wide)
	uint32_t eyeHeight;

	// Game side (the session showing the dash).
	volatile int32_t open;        // 1 while the dash shows (also set by the dash for home)
	volatile uint32_t gamePid;    // process showing the dash (0: none)
	volatile int32_t inputSeq;    // bumped after each write of `input`
	AtriumDashInput input;

	// Dash side: the latest finished picture and the views each slot was rendered from.
	volatile int32_t frameSeq;    // bumped after each published picture
	volatile int32_t latestSlot;  // the slot holding it
	AtriumDashPose slotEye[ATRIUM_DASH_SLOTS][2];
	AtriumDashFov slotFov[ATRIUM_DASH_SLOTS][2];
	volatile int32_t closeRequest; // bumped when the dash wants to close (Resume button)
	volatile int32_t hapticSeq;    // bumped for a short tick on hapticHand (hover/click)
	volatile int32_t hapticHand;
	volatile int32_t homeActive;   // 1 while atrium-dash's own session runs

	// Dash side: the Controls tab (the game side reads these every frame, no file reads).
	volatile int32_t openButton; // index into the buttons list (0 menu, 1 X, 2 Y, 3 A, 4 B, 5/6 sticks)
	volatile int32_t openHold;   // 1 hold, 0 press
	volatile int32_t pauseGame;  // 1: the game loses focus while the dash is open

	// Game side: the game running on the Atrium Runtime (the dash opens over it when the Oculus
	// button is pressed: Meta's runtime keeps that button, the dash learns of it from Meta's log).
	volatile uint32_t activeGamePid;
};
