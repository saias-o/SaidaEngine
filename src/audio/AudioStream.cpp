#include "audio/AudioStream.hpp"
#include "net/HttpTransport.hpp"
#include <miniaudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <limits>
#include <thread>
#include <vector>

namespace saida {
namespace {
// Network and decoding stay off the device callback. Fixed budgets also apply
// to stations broadcasting indefinitely; no downloaded audio file is created.
struct StreamJob {
    static constexpr size_t byteCapacity=512*1024,frameCapacity=131072;
    std::atomic<bool> cancelled{false},decoded{false};
    std::atomic<AudioStream::State> state{AudioStream::State::Opening};
    std::atomic<long> error{0};
    std::atomic<float> volume{.35f};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<unsigned char> bytes=std::vector<unsigned char>(byteCapacity);
    uint64_t base=0,cursor=0,received=0;
    bool probing=true,finished=false;
    std::vector<float> pcm=std::vector<float>(frameCapacity*2);
    std::atomic<uint64_t> read{0},written{0};
    void fail(long code){error=code;state=AudioStream::State::Failed;}
    void cancel(){cancelled=true;changed.notify_all();}
    bool push(const char* data,size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        while(count&&!cancelled) {
            changed.wait(lock,[&]{return cancelled||received-base<byteCapacity;});
            if(cancelled)return false;
            const size_t at=received%byteCapacity;
            const size_t n=std::min({count,byteCapacity-size_t(received-base),byteCapacity-at});
            std::memcpy(bytes.data()+at,data,n);received+=n;data+=n;count-=n;
            changed.notify_all();
        }
        return !cancelled;
    }
    ma_result pull(void* output,size_t count,size_t* actual) {
        *actual=0;if(!count)return MA_SUCCESS;
        std::unique_lock<std::mutex> lock(mutex);
        const size_t wanted=std::min(count,byteCapacity);
        changed.wait(lock,[&]{return cancelled||finished||received-cursor>=wanted;});
        if(cancelled)return MA_AT_END;
        const size_t n=std::min(count,size_t(received-cursor));
        const size_t first=std::min(n,byteCapacity-size_t(cursor%byteCapacity));
        if(n){std::memcpy(output,bytes.data()+cursor%byteCapacity,first);if(n>first)std::memcpy(static_cast<char*>(output)+first,bytes.data(),n-first);}
        cursor+=n;if(!probing)base=cursor;*actual=n;changed.notify_all();
        return n?MA_SUCCESS:MA_AT_END;
    }
    ma_result seek(ma_int64 offset,ma_seek_origin origin) {
        std::unique_lock<std::mutex> lock(mutex);
        if(origin!=ma_seek_origin_start&&origin!=ma_seek_origin_current)return MA_BAD_SEEK;
        if(cursor>uint64_t(std::numeric_limits<ma_int64>::max()))return MA_BAD_SEEK;
        const auto current=origin==ma_seek_origin_start?0:ma_int64(cursor);
        if((offset<0&&offset< -current)||(offset>0&&offset>std::numeric_limits<ma_int64>::max()-current))return MA_BAD_SEEK;
        const auto target=current+offset;
        // A live source has no known end. Keep the initial format probe prefix
        // but never retain an unbounded history to implement backward seeking.
        if(target<ma_int64(base)||uint64_t(target)>received)return MA_BAD_SEEK;
        cursor=uint64_t(target);if(!probing)base=cursor;changed.notify_all();return MA_SUCCESS;
    }
};
void decode(const std::shared_ptr<StreamJob>& job) {
    ma_decoder decoder{};
    const auto config=ma_decoder_config_init(ma_format_f32,2,48000);
    const auto result=ma_decoder_init(
        +[](ma_decoder* d,void* out,size_t n,size_t* read){return static_cast<StreamJob*>(d->pUserData)->pull(out,n,read);},
        +[](ma_decoder* d,ma_int64 offset,ma_seek_origin origin){return static_cast<StreamJob*>(d->pUserData)->seek(offset,origin);},
        job.get(),&config,&decoder);
    if(result!=MA_SUCCESS){if(!job->cancelled)job->fail(result);job->cancel();return;}
    const auto closeDecoder=[](ma_decoder* decoder){ma_decoder_uninit(decoder);};
    std::unique_ptr<ma_decoder,decltype(closeDecoder)> decoderGuard(&decoder,closeDecoder);
    {std::lock_guard<std::mutex> lock(job->mutex);job->probing=false;job->base=job->cursor;job->changed.notify_all();}
    ma_device device{};
    auto deviceConfig=ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format=ma_format_f32;deviceConfig.playback.channels=2;deviceConfig.sampleRate=48000;
    deviceConfig.pUserData=job.get();
    deviceConfig.dataCallback=+[](ma_device* device,void* output,const void*,ma_uint32 frames){
        auto& j=*static_cast<StreamJob*>(device->pUserData);auto* out=static_cast<float*>(output);
        std::fill_n(out,size_t(frames)*2,0.f);
        if(j.cancelled||j.error.load()!=0)return;
        const auto start=j.read.load(std::memory_order_relaxed);
        const auto available=j.written.load(std::memory_order_acquire)-start;
        const auto n=std::min<uint64_t>(frames,available);const float gain=j.volume.load();
        for(uint64_t i=0;i<n;++i){const auto source=((start+i)%StreamJob::frameCapacity)*2;out[i*2]=j.pcm[source]*gain;out[i*2+1]=j.pcm[source+1]*gain;}
        j.read.store(start+n,std::memory_order_release);
        if(j.state!=AudioStream::State::Failed)j.state=n?AudioStream::State::Playing:j.decoded?AudioStream::State::Ended:AudioStream::State::Buffering;
    };
    const auto init=ma_device_init(nullptr,&deviceConfig,&device);
    if(init!=MA_SUCCESS){job->fail(init);job->cancel();return;}
    const auto closeDevice=[](ma_device* device){ma_device_uninit(device);};
    std::unique_ptr<ma_device,decltype(closeDevice)> deviceGuard(&device,closeDevice);
    const auto started=ma_device_start(&device);
    if(started!=MA_SUCCESS){job->fail(started);job->cancel();}
    std::vector<float> block(4096*2);
    while(!job->cancelled) {
        ma_uint64 count=0;const auto decoded=ma_decoder_read_pcm_frames(&decoder,block.data(),4096,&count);
        size_t copied=0;
        while(copied<count&&!job->cancelled) {
            const auto start=job->written.load(std::memory_order_relaxed);
            const auto free=StreamJob::frameCapacity-(start-job->read.load(std::memory_order_acquire));
            const auto n=std::min<uint64_t>(count-copied,free);
            for(uint64_t i=0;i<n;++i){const auto dest=((start+i)%StreamJob::frameCapacity)*2;job->pcm[dest]=block[(copied+i)*2];job->pcm[dest+1]=block[(copied+i)*2+1];}
            job->written.store(start+n,std::memory_order_release);copied+=n;
            if(!n)std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if(decoded!=MA_SUCCESS||!count){if(decoded!=MA_AT_END&&decoded!=MA_SUCCESS&&!job->cancelled)job->fail(decoded);break;}
    }
    job->decoded=true;
    while(!job->cancelled&&job->read.load()<job->written.load())std::this_thread::sleep_for(std::chrono::milliseconds(4));
    deviceGuard.reset();
    if(!job->cancelled&&job->error.load()==0)job->state=AudioStream::State::Ended;
    job->cancel();
}
}
struct AudioStream::Impl {std::shared_ptr<StreamJob> job;float volume=.35f;};
AudioStream::AudioStream():impl_(std::make_unique<Impl>()){}
AudioStream::~AudioStream(){stop();}
bool AudioStream::supported(){return true;}
bool AudioStream::allowsInsecureHttp(){return true;}
bool AudioStream::open(const std::string& url) {
    stop();auto job=std::make_shared<StreamJob>();job->volume=impl_->volume;impl_->job=job;
    if(!validHttpUrl(url)){job->fail(-1);return false;}
    try {
        std::thread([job,url]{
            try {
                auto response=http::receive(url,0,job->cancelled,[&](const char* data,size_t count){return job->push(data,count);});
                if(!job->cancelled&&!response.ok())job->fail(response.status?response.status:-2);
            }catch(...){if(!job->cancelled)job->fail(-2);}
            std::lock_guard<std::mutex> lock(job->mutex);job->finished=true;job->changed.notify_all();
        }).detach();
        std::thread([job]{try{decode(job);}catch(...){job->fail(-3);job->cancel();}}).detach();
    }catch(...){job->fail(-3);job->cancel();return false;}
    return true;
}
void AudioStream::stop(){if(impl_->job)impl_->job->cancel();impl_->job.reset();}
void AudioStream::setVolume(float volume){impl_->volume=std::isfinite(volume)?std::clamp(volume,0.f,1.f):0;if(impl_->job)impl_->job->volume=impl_->volume;}
AudioStream::Status AudioStream::status() const {
    if(!impl_->job)return {};
    const auto error=impl_->job->error.load();
    return {error?State::Failed:impl_->job->state.load(),error};
}
}
