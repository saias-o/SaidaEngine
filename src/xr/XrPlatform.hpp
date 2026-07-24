#pragma once

// Internal xr/*.cpp include: fixes Windows/Vulkan/OpenXR platform include order.
// Keep out of public headers so <windows.h> does not leak into the engine.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>   // IUnknown — referenced by openxr_platform.h's Win32 structs

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
