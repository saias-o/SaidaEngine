#include "core/Window.hpp"
#include "core/Paths.hpp"
#include "core/PngWriter.hpp"
#include "core/Time.hpp"
#include "graphics/VulkanDevice.hpp"
#include "nodes/WebCanvasNode.hpp"
#include "rhi/Rhi.hpp"
#include "ui/RmlUiRuntime.hpp"
#include "ui/RmlUiGpuRenderer.hpp"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Input.h>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace saida;

std::vector<uint8_t> gpuPixels(VulkanDevice& device, const RmlUiRenderInterface::DrawList& list) {
    std::vector<uint8_t> zero(size_t(list.width) * list.height * 4);
    Texture target(device, zero.data(), list.width, list.height, rhi::Format::RGBA8Srgb,
                   false, rhi::AddressMode::ClampToEdge, false, true);
    Buffer read(device, zero.size(), rhi::BufferUsage::TransferDst, MemoryUsage::HostVisible);
    RmlUiGpuRenderer renderer(device);
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        renderer.render(list, enc, target, 0);
        enc.transition(target.image(), rhi::ResourceState::ShaderRead, rhi::ResourceState::CopySrc);
        enc.copyTextureToBuffer(target.image(), read, list.width, list.height);
        enc.transition(target.image(), rhi::ResourceState::CopySrc, rhi::ResourceState::ShaderRead);
    });
    std::memcpy(zero.data(), read.mapped(), zero.size());
    return zero;
}

int main(int argc, char** argv) {
    Window window(64, 64, "Saida UI GPU verification", false);
    assert(!window.isFullscreen());
    window.setFullscreen(true);
    assert(window.isFullscreen());
    window.setFullscreen(false);
    assert(!window.isFullscreen());
    window.toggleFullscreen();
    assert(window.isFullscreen());
    window.toggleFullscreen();
    assert(!window.isFullscreen());

    VulkanDevice device(window);
    assert(RmlUiRuntime::ensureInitialized());
    auto* context = Rml::CreateContext("ui-gpu", {64, 64}, RmlUiRuntime::renderInterface());
    auto* document = RmlUiRuntime::loadDocumentFromMemory(*context, R"RML(
        <rml><head><style>
        body { margin: 0; }
        #back { position: absolute; left: 4px; top: 4px; width: 40px; height: 40px; background-color: #205080; }
        #over { position: absolute; left: 12px; top: 12px; width: 20px; height: 20px; background-color: rgba(240,80,20,128); }
        #clip { position: absolute; left: 45px; top: 4px; width: 10px; height: 20px; overflow: hidden; }
        #child { width: 30px; height: 30px; background-color: #80cc44; transform: translateY(4px); }
        </style></head><body><div id="back"></div><div id="over"></div><div id="clip"><div id="child"></div></div></body></rml>
    )RML", "ui-gpu.rml");
    assert(document); document->Show(); context->Update();
    auto* backend = RmlUiRuntime::renderer();
    backend->beginFrame(64, 64); context->Render(); backend->endFrame();
    const auto reference = backend->pixels();
    backend->beginCapture(64, 64); context->Render(); const auto list = backend->endCapture();
    const auto gpu = gpuPixels(device, list);
    for (const auto& point : {std::pair<int,int>{1,1}, {8,8}, {20,19}, {48,12}, {58,12}, {48,27}}) {
        const size_t offset = size_t(point.second * 64 + point.first) * 4;
        for (size_t c = 0; c < 4; ++c) {
            if (std::abs(int(gpu[offset+c]) - int(reference[offset+c])) > 2) {
                std::cerr << "GPU mismatch at " << point.first << ',' << point.second << " channel " << c
                          << ": " << int(gpu[offset+c]) << " != " << int(reference[offset+c]) << '\n';
                return 1;
            }
        }
    }
    context->UnloadAllDocuments(); context->Update(); Rml::RemoveContext("ui-gpu");

    {
        WebCanvasNode canvas;
        canvas.init(device, 160, 100);
        canvas.loadHTML(R"(<rml><head><style>body{margin:0;} #pulse{width:20px;height:20px;background-color:#ff6600;animation:2s linear-in-out infinite pulse;} @keyframes pulse{from{opacity:0.2;}to{opacity:1;}}</style></head><body><input id="name" type="text"/><div id="panel"></div><div id="pulse"></div></body></rml>)");
        canvas.executeJS("document.getElementById('name').value='Traveller'; document.getElementById('panel').hidden=true; document.getElementById('panel').setAttribute('data-place','coast');");
        assert(canvas.findElementById("name")->GetAttribute<Rml::String>("value", "") == "Traveller");
        assert(canvas.findElementById("panel")->HasAttribute("hidden"));
        assert(canvas.findElementById("panel")->GetAttribute<Rml::String>("data-place", "") == "coast");
        canvas.executeJS("const p=document.getElementById('panel'); p.innerHTML='A <b>quiet</b> coast'; p.setAttribute('data-text',p.textContent); p.style.opacity='0.5'; p.style.backgroundColor='rgba(20,80,160,0.5)'; p.setAttribute('data-opacity',p.style.getPropertyValue('opacity'));");
        assert(canvas.findElementById("panel")->GetAttribute<Rml::String>("data-text", "") == "A quiet coast");
        assert(canvas.findElementById("panel")->GetAttribute<Rml::String>("data-opacity", "") == "0.5");
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) { canvas.updateTextureIfNeededAsync(enc, 0); });
        auto* pulse = canvas.findElementById("pulse");
        Time::advance(1.f/60.f);
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) { canvas.updateTextureIfNeededAsync(enc, 1); });
        const float before = pulse->GetProperty<float>("opacity");
        Time::advance(0.5f);
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) { canvas.updateTextureIfNeededAsync(enc, 1); });
        assert(pulse->GetProperty<float>("opacity") > before);
        canvas.resize(200, 120);
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) { canvas.updateTextureIfNeededAsync(enc, 0); });
        canvas.loadHTML(R"(<rml><head><style>body{margin:0;} #hover{position:absolute;left:0;top:0;width:40px;height:30px;background-color:#ff0000;} #hover:hover{background-color:#0000ff;}</style></head><body><div id="hover" class="ui-hit"></div></body></rml>)");
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) { canvas.updateTextureIfNeededAsync(enc, 0); });
        canvas.fireMouseEvent(WebCanvasNode::MouseEvent::Move, 10, 10, WebCanvasNode::MouseButton::None);
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) { canvas.updateTextureIfNeededAsync(enc, 1); });
        assert(canvas.findElementById("hover")->GetProperty<Rml::Colourb>("background-color").blue == 255);
        canvas.loadHTML(R"(<html><head><style>body{margin:0;} #parent{width:100px;height:100px;} input{width:80px;height:25px;}</style></head><body><div id="parent"><input id="field" type="text"></div></body></html>)");
        canvas.executeJS(R"(
            var parent=document.getElementById('parent'), field=document.getElementById('field');
            var savedEvent=null;
            field.addEventListener('keydown',function(e){
                field.setAttribute('data-key',e.key);savedEvent=e;e.stopPropagation();
            });
            parent.addEventListener('keydown',function(e){parent.setAttribute('data-bubbled','yes');});
            function wheel(e){field.setAttribute('data-wheel',e.type+':'+e.deltaY+':'+e.deltaMode);}
            field.addEventListener('wheel',wheel);field.focus();
        )");
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc){canvas.updateTextureIfNeededAsync(enc,0);});
        canvas.fireKeyEvent(true,Rml::Input::KI_RETURN);
        assert(canvas.findElementById("field")->GetAttribute<Rml::String>("data-key","")=="Enter");
        assert(!canvas.findElementById("parent")->HasAttribute("data-bubbled"));
        canvas.executeJS("savedEvent.stopPropagation(); savedEvent.stopImmediatePropagation();");
        canvas.fireMouseEvent(WebCanvasNode::MouseEvent::Move,10,10,WebCanvasNode::MouseButton::None);
        canvas.fireScrollEvent(0,2);
        assert(canvas.findElementById("field")->GetAttribute<Rml::String>("data-wheel","")=="wheel:2:1");
        canvas.executeJS("field.removeEventListener('wheel',wheel);field.removeAttribute('data-wheel');");
        canvas.fireScrollEvent(0,1);
        assert(!canvas.findElementById("field")->HasAttribute("data-wheel"));
    }
    device.waitIdle();

    if (argc > 1) {
        auto* gallery = Rml::CreateContext("ui-gallery", {1280, 800}, RmlUiRuntime::renderInterface());
        auto* menu = RmlUiRuntime::loadDocument(*gallery, assetPath("templates/ui/menu.html"));
        assert(menu); menu->Show(); gallery->Update();
        for (int i = 0; i < 75; ++i) { Time::advance(1.f/60.f); gallery->Update(); }
        backend->beginCapture(1280, 800); gallery->Render(); auto menuDraws = backend->endCapture();
        auto image = gpuPixels(device, menuDraws);
        std::string error;
        if (!writePngRGBA8(argv[1], image.data(), 1280, 800, error)) { std::cerr << error; return 1; }
        std::vector<double> cpuTimes, gpuTimes;
        std::vector<uint8_t> zero(image.size());
        Texture target(device, zero.data(), 1280, 800, rhi::Format::RGBA8Srgb,
                       false, rhi::AddressMode::ClampToEdge, false, true);
        RmlUiGpuRenderer persistent(device);
        for (int i = 0; i < 20; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) { persistent.render(menuDraws, enc, target, i % 2); });
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (i >= 4) gpuTimes.push_back(ms);
        }
        for (int i = 0; i < 16; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            backend->beginFrame(1280, 800); gallery->Render(); backend->endFrame();
            cpuTimes.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        }
        std::sort(cpuTimes.begin(), cpuTimes.end()); std::sort(gpuTimes.begin(), gpuTimes.end());
        std::ofstream timings(std::string(argv[1]) + ".timings.json");
        timings << "{\"width\":1280,\"height\":800,\"samples\":16,\"cpuRasterMedianMs\":" << cpuTimes[8]
                << ",\"gpuSubmitAndFenceMedianMs\":" << gpuTimes[8]
                << ",\"gpuSubmitAndFenceP95Ms\":" << gpuTimes[15] << "}\n";
        std::cout << "[ui-gpu] 1280x800 CPU raster median=" << cpuTimes[8]
                  << " ms; GPU submit+fence median=" << gpuTimes[8] << " ms\n";
        gallery->UnloadAllDocuments(); gallery->Update(); Rml::RemoveContext("ui-gallery");
    }
    RmlUiRuntime::shutdown();
    std::cout << "[ui-gpu] PASS: GPU pixels match CPU samples, clipping, blend, transforms, DOM values, autonomous animation, resize\n";
}
