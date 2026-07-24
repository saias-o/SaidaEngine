#include "rhi/webgpu/Texture.hpp"

#include "rhi/webgpu/Device.hpp"
#include "rhi/webgpu/Format.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace saida::rhi::webgpu {

namespace {

float srgbToLinear(uint8_t v) {
    const float s = v / 255.0f;
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
uint8_t linearToSrgb(float l) {
    l = std::min(std::max(l, 0.0f), 1.0f);
    const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return uint8_t(s * 255.0f + 0.5f);
}

// Box downsample. For sRGB textures the RGB channels are averaged in LINEAR
// space (decode → mean → re-encode) — averaging the encoded bytes directly
// over-brightens every mip, which shows up badly in IBL diffuse (it samples the
// blurred high mips as the ambient sky colour). Alpha is always linear. This
// matches how Vulkan's sRGB blit filtering builds the desktop mips.
std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h,
                                uint32_t nw, uint32_t nh, bool srgb) {
    std::vector<uint8_t> dst(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
        for (uint32_t x = 0; x < nw; ++x) {
            const uint32_t sx = std::min(x * 2, w - 1), sy = std::min(y * 2, h - 1);
            const uint32_t sx1 = std::min(sx + 1, w - 1), sy1 = std::min(sy + 1, h - 1);
            const size_t p00 = (size_t(sy) * w + sx) * 4, p10 = (size_t(sy) * w + sx1) * 4;
            const size_t p01 = (size_t(sy1) * w + sx) * 4, p11 = (size_t(sy1) * w + sx1) * 4;
            uint8_t* out = &dst[(size_t(y) * nw + x) * 4];
            for (int c = 0; c < 3; ++c) {
                if (srgb) {
                    const float mean = (srgbToLinear(src[p00 + c]) + srgbToLinear(src[p10 + c]) +
                                        srgbToLinear(src[p01 + c]) + srgbToLinear(src[p11 + c])) * 0.25f;
                    out[c] = linearToSrgb(mean);
                } else {
                    out[c] = uint8_t((src[p00 + c] + src[p10 + c] + src[p01 + c] + src[p11 + c]) / 4);
                }
            }
            out[3] = uint8_t((src[p00 + 3] + src[p10 + 3] + src[p01 + 3] + src[p11 + 3]) / 4);
        }
    }
    return dst;
}

} // namespace

Texture::Texture(Device& device, const std::string& path, bool srgb)
    : device_(device) {
    int texWidth = 0, texHeight = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &channels, STBI_rgb_alpha);
    bool freeStb = pixels != nullptr;
    // A missing file degrades to a 2x2 magenta placeholder, never a crash.
    static const stbi_uc kMagenta[2 * 2 * 4] = {
        255, 0, 255, 255,  24, 24, 24, 255,
        24, 24, 24, 255,  255, 0, 255, 255,
    };
    if (!pixels) {
        std::printf("webgpu: failed to load texture '%s' — magenta placeholder\n", path.c_str());
        pixels = const_cast<stbi_uc*>(kMagenta);
        texWidth = texHeight = 2;
    }

    Texture loaded(device, pixels, static_cast<uint32_t>(texWidth),
                   static_cast<uint32_t>(texHeight),
                   srgb ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm);
    if (freeStb) stbi_image_free(pixels);

    texture_ = loaded.texture_;
    view_ = loaded.view_;
    sampler_ = loaded.sampler_;
    width_ = loaded.width_;
    height_ = loaded.height_;
    mipLevels_ = loaded.mipLevels_;

    loaded.texture_ = nullptr;
    loaded.view_ = nullptr;
    loaded.sampler_ = nullptr;
}

Texture::Texture(Device& device, const uint8_t* pixels, uint32_t width, uint32_t height,
                 rhi::Format format, bool generateMipmaps)
    : device_(device), width_(width), height_(height) {
    mipLevels_ = 1;
    if (generateMipmaps && (width > 1 || height > 1))
        mipLevels_ = uint32_t(std::floor(std::log2(std::max(width, height)))) + 1;

    WGPUTextureDescriptor desc = {};
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {width_, height_, 1};
    desc.format = toWgpu(format);
    desc.mipLevelCount = mipLevels_;
    desc.sampleCount = 1;
    texture_ = wgpuDeviceCreateTexture(device_.device(), &desc);
    if (!texture_) throw std::runtime_error("webgpu: failed to create texture");

    srgb_ = format == rhi::Format::RGBA8Srgb || format == rhi::Format::BGRA8Srgb;
    upload(pixels, srgb_);

    WGPUTextureViewDescriptor vd = {};
    vd.format = desc.format;
    vd.dimension = WGPUTextureViewDimension_2D;
    vd.mipLevelCount = mipLevels_;
    vd.arrayLayerCount = 1;
    view_ = wgpuTextureCreateView(texture_, &vd);

    WGPUSamplerDescriptor sd = {};
    sd.addressModeU = WGPUAddressMode_Repeat;
    sd.addressModeV = WGPUAddressMode_Repeat;
    sd.addressModeW = WGPUAddressMode_Repeat;
    sd.magFilter = WGPUFilterMode_Linear;
    sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sd.lodMaxClamp = float(mipLevels_);
    sd.maxAnisotropy = 1;
    sampler_ = wgpuDeviceCreateSampler(device_.device(), &sd);
}

void Texture::upload(const uint8_t* pixels, bool srgb) {
    std::vector<uint8_t> level(pixels, pixels + size_t(width_) * height_ * 4);
    uint32_t w = width_, h = height_;
    for (uint32_t mip = 0; mip < mipLevels_; ++mip) {
        WGPUTexelCopyTextureInfo dst = {};
        dst.texture = texture_;
        dst.mipLevel = mip;
        WGPUTexelCopyBufferLayout layout = {};
        layout.bytesPerRow = w * 4;
        layout.rowsPerImage = h;
        WGPUExtent3D extent = {w, h, 1};
        wgpuQueueWriteTexture(device_.queue(), &dst, level.data(), level.size(), &layout, &extent);

        if (mip + 1 < mipLevels_) {
            const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            level = downsample(level, w, h, nw, nh, srgb);
            w = nw;
            h = nh;
        }
    }
}

void Texture::updatePixels(const uint8_t* pixels, size_t size) {
    (void)size;
    upload(pixels, srgb_);
}

Texture::~Texture() {
    if (sampler_) wgpuSamplerRelease(sampler_);
    if (view_) wgpuTextureViewRelease(view_);
    if (texture_) {
        wgpuTextureDestroy(texture_);
        wgpuTextureRelease(texture_);
    }
}

} // namespace saida::rhi::webgpu
