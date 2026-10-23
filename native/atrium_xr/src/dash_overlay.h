// The Atrium dash inside every game (dash_overlay.cpp), called from runtime.cpp's wrappers.
#pragma once
#include <vector>
#include <openxr/openxr.h>

void DashSessionCreated(XrInstance instance, const XrSessionCreateInfo* info, XrSession session);
void DashSessionDestroying(XrSession session);
// Adds the dash's layer after the game's when it's open; false when nothing changes.
bool DashEndFrame(XrSession session, const XrFrameEndInfo* info, std::vector<const XrCompositionLayerBaseHeader*>& layers);

XRAPI_ATTR XrResult XRAPI_CALL Dash_xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* b);
XRAPI_ATTR XrResult XRAPI_CALL Dash_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info);
XRAPI_ATTR XrResult XRAPI_CALL Dash_xrSyncActions(XrSession session, const XrActionsSyncInfo* info);
XRAPI_ATTR XrResult XRAPI_CALL Dash_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* state);
XRAPI_ATTR XrResult XRAPI_CALL Dash_xrPollEvent(XrInstance instance, XrEventDataBuffer* event);
