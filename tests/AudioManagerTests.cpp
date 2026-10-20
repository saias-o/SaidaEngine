#include "audio/AudioManager.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

// AudioManager's control of one playing sound: what a menu's looping music
// needs -- pause when the game takes over, resume where it stopped, fade.
using saida::AudioManager;
namespace fs = std::filesystem;
namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL audio-manager: %s\n", message); std::exit(1); }
}
void writeTone(const fs::path& path) {
    const uint32_t rate = 22050, samples = rate / 2;
    std::vector<int16_t> pcm(samples);
    for (uint32_t i = 0; i < samples; ++i) pcm[i] = int16_t(3000 * std::sin(i * 2 * 3.14159265 * 440 / rate));
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + samples * 2); out.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    out.write("data", 4); u32(samples * 2);
    out.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(samples * 2));
}
}

int main() {
    auto& audio = AudioManager::get();
    if (!audio.init()) {
        std::printf("[audio-manager] SKIP: no audio device to open\n");
        return 0;
    }
    const fs::path tone = fs::temp_directory_path() / "saida_audio_manager_tone.wav";
    writeTone(tone);
    saida::AudioSettings settings;
    settings.loop = true;
    settings.volume = .5f;
    const auto id = audio.play(tone.string(), settings);
    require(id != saida::kInvalidAudioID, "a WAV plays");
    require(audio.isPlaying(id), "a started sound is playing");
    audio.pause(id);
    require(!audio.isPlaying(id), "a paused sound is not playing");
    audio.resume(id);
    require(audio.isPlaying(id), "resume continues a paused sound");
    audio.setVolume(id, .1f);
    audio.setVolume(id, 4.f); // clamped, not an error
    require(audio.isPlaying(id), "changing the volume keeps it playing");
    audio.stop(id);
    require(!audio.isPlaying(id), "a stopped sound is gone");
    audio.resume(id);
    require(!audio.isPlaying(id), "resuming a stopped sound does nothing");
    require(!audio.isPlaying(saida::kInvalidAudioID), "an unknown id is not playing");
    audio.shutdown();
    fs::remove(tone);
    std::printf("[audio-manager] PASS: pause, resume, per-sound volume, stop\n");
}
