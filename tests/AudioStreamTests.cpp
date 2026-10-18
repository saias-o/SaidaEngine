#include "audio/AudioStream.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using saida::AudioStream;
namespace {
void require(bool condition,const char* message) {
    if(!condition){std::fprintf(stderr,"FAIL audio-stream: %s\n",message);std::exit(1);}
}
}
int main(int argc,char** argv) {
    AudioStream stream;
    require(stream.status().state==AudioStream::State::Stopped,"new stream is stopped");
    for(const char* url:{"file:///private.wav","javascript:alert(1)","https://a\r\nInjected: yes",""}) {
        require(!stream.open(url),"reject non-HTTP sources and header injection");
        require(stream.status().state==AudioStream::State::Failed,"invalid source reports an error");
    }
    stream.stop();stream.setVolume(-1);stream.setVolume(2);
    require(stream.status().state==AudioStream::State::Stopped,"stop clears a previous error");
    if(argc>1) {
        const auto start=std::chrono::steady_clock::now();
        require(stream.open(argv[1]),"real HTTP stream accepted");
        require(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<2.,"network open does not block frame thread");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        auto state=stream.status().state;
        while(state!=AudioStream::State::Playing&&state!=AudioStream::State::Failed&&std::chrono::steady_clock::now()<deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));state=stream.status().state;
        }
        std::printf("audio-stream state=%d error=%ld\n",int(state),stream.status().error);
        require(state==AudioStream::State::Playing,"HTTP stream reaches real backend playback");
        if(argc>2&&std::string(argv[2])=="--finite") {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(stream.status().state==AudioStream::State::Playing&&std::chrono::steady_clock::now()<end)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            require(stream.status().state==AudioStream::State::Ended,"finite stream reports the drained end");
        }
        stream.setVolume(0);stream.setVolume(.2f);
        stream.stop();require(stream.status().state==AudioStream::State::Stopped,"playing stream can be stopped");
    }
    std::puts("PASS audio-stream validation and lifecycle");
}
