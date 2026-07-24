#pragma once

#include <vulkan/vulkan.h>

// Forward declarations of the VMA opaque handles, so headers can hold them
// without pulling in the full (large) vk_mem_alloc.h. These expand to the
// exact same typedefs VMA itself uses, so including both is harmless.
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaVirtualBlock)
VK_DEFINE_HANDLE(VmaVirtualAllocation)
