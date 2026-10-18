#pragma once

#include <memory>
#include <string>

namespace saida {

// Independent, non-spatial HTTP(S) audio. Network open/decode is asynchronous;
// callers inspect status without blocking their render thread. No project aliases.
class AudioStream {
public:
    enum class State { Stopped, Opening, Playing, Buffering, Ended, Failed, AwaitingGesture };
    struct Status { State state=State::Stopped; long error=0; };
    AudioStream();
    ~AudioStream();
    AudioStream(const AudioStream&)=delete;
    AudioStream& operator=(const AudioStream&)=delete;
    static bool supported();
    // HTTPS pages cannot load insecure remote programmes. Hosts can filter
    // their catalogues before selecting the nearest playable stream.
    static bool allowsInsecureHttp();
    bool open(const std::string& url);
    void stop();
    void setVolume(float volume);
    Status status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace saida
