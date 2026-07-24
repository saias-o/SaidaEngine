#pragma once

// One backend per build, selected at compile time to keep the frame path direct.

#include "rhi/Capabilities.hpp"
#include "rhi/BindGroup.hpp"
#include "rhi/BufferUsage.hpp"
#include "rhi/CommandTypes.hpp"
#include "rhi/Format.hpp"
#include "rhi/PipelineState.hpp"
#include "rhi/Sampler.hpp"
#include "rhi/ShaderStages.hpp"
#include "rhi/TextureUsage.hpp"

#ifdef SAIDA_RHI_WEBGPU

#include "rhi/webgpu/RhiWeb.hpp"

#else

#include "rhi/vulkan/BindGroup.hpp"
#include "rhi/vulkan/CommandEncoder.hpp"
#include "rhi/vulkan/Handles.hpp"
#include "rhi/vulkan/RenderTexture.hpp"
#include "rhi/vulkan/Sampler.hpp"
#include "graphics/Buffer.hpp"   // Vulkan backend's Buffer (aliased below)
#include "graphics/ComputePipeline.hpp"
#include "graphics/Pipeline.hpp" // Vulkan backend's Pipeline (aliased below)
#include "graphics/Texture.hpp"  // Vulkan backend's Texture (aliased below)

namespace saida {
class VulkanDevice;  // Vulkan backend's Device (aliased below, fwd-decl only)
class Swapchain;     // Vulkan backend's Surface (aliased below, fwd-decl only)
}

namespace saida::rhi {

// Vulkan aliases for the desktop/XR build.
using Buffer = saida::Buffer;

using Texture = saida::Texture;

using Pipeline = saida::Pipeline;
using ComputePipeline = saida::ComputePipeline;
using BindGroupLayout = vulkan::BindGroupLayout;
using BindGroup = vulkan::BindGroup;
using BindGroupEntry = vulkan::BindGroupEntry;

using CommandEncoder = vulkan::CommandEncoder;
using RenderPassEncoder = vulkan::RenderPassEncoder;
using ComputePassEncoder = vulkan::ComputePassEncoder;
using RenderPassDesc = vulkan::RenderPassDesc;
using ColorAttachment = vulkan::ColorAttachment;
using DepthAttachment = vulkan::DepthAttachment;

using Device = saida::VulkanDevice;
using Surface = saida::Swapchain;
using RenderTexture = vulkan::RenderTexture;
using RenderTextureDesc = vulkan::RenderTextureDesc;
using Sampler = vulkan::Sampler;

using TextureHandle = vulkan::TextureHandle;
using TextureView = vulkan::TextureView;
using SamplerHandle = vulkan::SamplerHandle;
using Extent2D = vulkan::Extent2D;
using Rect2D = vulkan::Rect2D;
using SampleCount = vulkan::SampleCount;

} // namespace saida::rhi

#endif // SAIDA_RHI_WEBGPU
