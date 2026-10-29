#include "Engine.hpp"
#include "scene/Scene.hpp"
#include "core/Window.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("saida-interactive-capture-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup {fs::path root;~Cleanup(){std::error_code error;fs::remove_all(root,error);}} cleanup{root};
    saida::Engine engine([](saida::Scene& scene,saida::ResourceManager&){scene.settings().giEnabled=false;});
    unsigned callbacks=0;bool success=false;std::string failure,error;
    auto completion=[&](bool ok,const std::string& message){++callbacks;success=ok;failure=message;};
    assert(!engine.captureFramePng("",completion,error)&&callbacks==0);
    auto capture=[&](const fs::path& path,bool expected) {
        const auto before=callbacks;
        assert(engine.captureFramePng(path.string(),completion,error));
        assert(!engine.captureFramePng((root/"overlap.png").string(),completion,error));
        for(int frame=0;callbacks==before&&frame<120;++frame)assert(engine.tick());
        assert(callbacks==before+1&&success==expected&&!engine.captureFailed());
        if(expected) {
            std::ifstream in(path,std::ios::binary);char signature[8]{};in.read(signature,8);
            assert(in.gcount()==8&&std::string(signature,8)==std::string("\x89PNG\r\n\x1a\n",8));
        } else assert(!failure.empty());
        // A success or a write error must leave the window and loop alive.
        assert(engine.tick()&&callbacks==before+1);
    };
    capture(root/"first.png",true);
    capture(root,false); // output is a directory, not a writable PNG
    capture(root/"second.png",true);
    const auto beforeResize=callbacks;
    bool resize=true;
    engine.setOnFrame([&](float){if(resize){engine.window().setSize(800,450);resize=false;}});
    assert(engine.captureFramePng((root/"resized.png").string(),completion,error));
    for(int frame=0;callbacks==beforeResize&&frame<120;++frame)assert(engine.tick());
    assert(callbacks==beforeResize+1&&!engine.captureFailed());
    engine.setOnFrame({});
    for(int frame=0;frame<4;++frame)assert(engine.tick());
    capture(root/"after-resize.png",true);
    assert(!fs::exists(root/"overlap.png"));
    std::cout<<"PASS interactive capture: repeat, overlap, failure, resize, callback and continued frames\n";
}
