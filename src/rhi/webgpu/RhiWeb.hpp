#pragma once

// WebGPU aliases for builds that cannot include the desktop RHI entry point.

#include "rhi/BufferUsage.hpp"
#include "rhi/Capabilities.hpp"
#include "rhi/CommandTypes.hpp"
#include "rhi/Format.hpp"
#include "rhi/PipelineState.hpp"
#include "rhi/Sampler.hpp"
#include "rhi/ShaderStages.hpp"
#include "rhi/TextureUsage.hpp"
#include "rhi/webgpu/BindGroup.hpp"
#include "rhi/webgpu/Buffer.hpp"
#include "rhi/webgpu/CommandEncoder.hpp"
#include "rhi/webgpu/Device.hpp"
#include "rhi/webgpu/Format.hpp"
#include "rhi/webgpu/Handles.hpp"
#include "rhi/webgpu/Pipeline.hpp"
#include "rhi/webgpu/RenderTexture.hpp"
#include "rhi/webgpu/Sampler.hpp"
#include "rhi/webgpu/Surface.hpp"
#include "rhi/webgpu/Texture.hpp"

namespace saida::rhi {

using Device = webgpu::Device;
using Surface = webgpu::Surface;
using Buffer = webgpu::Buffer;
using MemoryUsage = webgpu::MemoryUsage;
using Texture = webgpu::Texture;
using RenderTexture = webgpu::RenderTexture;
using RenderTextureDesc = webgpu::RenderTextureDesc;
using Sampler = webgpu::Sampler;
using BindGroupLayout = webgpu::BindGroupLayout;
using BindGroup = webgpu::BindGroup;
using BindGroupEntry = webgpu::BindGroupEntry;
using Pipeline = webgpu::Pipeline;
using ComputePipeline = webgpu::ComputePipeline;
using CommandEncoder = webgpu::CommandEncoder;
using RenderPassEncoder = webgpu::RenderPassEncoder;
using ComputePassEncoder = webgpu::ComputePassEncoder;
using RenderPassDesc = webgpu::RenderPassDesc;
using ColorAttachment = webgpu::ColorAttachment;
using DepthAttachment = webgpu::DepthAttachment;

using TextureHandle = webgpu::TextureHandle;
using TextureView = webgpu::TextureView;
using SamplerHandle = webgpu::SamplerHandle;
using Extent2D = webgpu::Extent2D;
using Rect2D = webgpu::Rect2D;
using SampleCount = webgpu::SampleCount;

} // namespace saida::rhi
