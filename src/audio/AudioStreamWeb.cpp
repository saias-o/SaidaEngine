#include "audio/AudioStream.hpp"
#include "net/HttpRequest.hpp"
#include <emscripten.h>
#include <algorithm>
#include <cmath>

namespace saida {
// Browser policy is part of this backend: asynchronous station discovery may
// finish outside a trusted gesture. Keep playback pending until the next one.
EM_JS(void,audio_boot,(),{
    if(Module.saidaAudio)return;
    const service=Module.saidaAudio={next:1,jobs:new Map(),context:null};
    service.play=job=>{
        if(service.jobs.get(job.id)!==job)return;
        try {
            if(!service.context){job.state=6;return;}
            if(!job.source){job.source=service.context.createMediaElementSource(job.audio);job.gain=service.context.createGain();job.gain.gain.value=job.volume;job.source.connect(job.gain);job.gain.connect(service.context.destination);}
            const resumed=service.context.resume();
            if(resumed)resumed.catch(()=>{if(service.jobs.get(job.id)===job)job.state=6;});
            job.audio.play().then(()=>{
                if(service.jobs.get(job.id)===job)job.state=service.context.state==='running'?2:6;
            }).catch(error=>{
                if(service.jobs.get(job.id)!==job)return;
                job.state=error.name==='NotAllowedError'?6:5;job.error=job.state===5?-2:0;
            });
        }catch(error){job.state=5;job.error=-3;}
    };
    const unlock=()=>{
        try {
            if(!service.context)service.context=new (window.AudioContext||window.webkitAudioContext)();
            const resumed=service.context.resume();if(resumed)resumed.catch(()=>{});
            for(const job of service.jobs.values())if(job.state!==4&&job.state!==5&&(job.audio.paused||service.context.state!=='running'))service.play(job);
        }catch(error){for(const job of service.jobs.values()){job.state=5;job.error=-3;}}
    };
    for(const event of ['pointerdown','mousedown','click','keydown','touchend'])window.addEventListener(event,unlock,{passive:true});
});
EM_JS(int,audio_open,(const char* url,float volume),{
    const service=Module.saidaAudio,id=service.next++;
    const audio=new Audio();audio.crossOrigin='anonymous';audio.preload='none';
    const job={id,audio,source:null,gain:null,state:1,error:0,volume};service.jobs.set(id,job);
    const set=state=>{if(service.jobs.get(id)===job&&job.state!==5)job.state=state;};
    audio.addEventListener('playing',()=>set(service.context&&service.context.state==='running'?2:6));
    audio.addEventListener('waiting',()=>set(3));
    audio.addEventListener('stalled',()=>set(3));
    audio.addEventListener('ended',()=>set(4));
    audio.addEventListener('error',()=>{if(service.jobs.get(id)===job){job.error=audio.error?audio.error.code:-2;job.state=5;}});
    audio.src=UTF8ToString(url);service.play(job);return id;
});
EM_JS(void,audio_stop,(int id),{
    const service=Module.saidaAudio,job=service.jobs.get(id);if(!job)return;
    service.jobs.delete(id);job.audio.pause();job.audio.removeAttribute('src');job.audio.load();
    if(job.source)job.source.disconnect();if(job.gain)job.gain.disconnect();
});
EM_JS(void,audio_volume,(int id,float volume),{const job=Module.saidaAudio.jobs.get(id);if(job){job.volume=volume;if(job.gain)job.gain.gain.value=volume;}});
EM_JS(int,audio_state,(int id),{
    const service=Module.saidaAudio,job=service.jobs.get(id);if(!job)return 0;
    if(job.state!==4&&job.state!==5&&service.context?.state!=='running')return 6;
    return job.state;
});
EM_JS(int,audio_error,(int id),{const job=Module.saidaAudio.jobs.get(id);return job?job.error:0;});
EM_JS(int,audio_insecure_allowed,(),{return location.protocol!=="https:";});
struct AudioStream::Impl {int id=0;float volume=.35f;bool invalid=false;};
AudioStream::AudioStream():impl_(std::make_unique<Impl>()){audio_boot();}
AudioStream::~AudioStream(){stop();}
bool AudioStream::supported(){return true;}
bool AudioStream::allowsInsecureHttp(){return audio_insecure_allowed();}
bool AudioStream::open(const std::string& url){stop();if(!validHttpUrl(url)){impl_->invalid=true;return false;}impl_->id=audio_open(url.c_str(),impl_->volume);return true;}
void AudioStream::stop(){if(impl_->id)audio_stop(impl_->id);impl_->id=0;impl_->invalid=false;}
void AudioStream::setVolume(float volume){impl_->volume=std::isfinite(volume)?std::clamp(volume,0.f,1.f):0;if(impl_->id)audio_volume(impl_->id,impl_->volume);}
AudioStream::Status AudioStream::status() const {if(impl_->invalid)return {State::Failed,-1};if(!impl_->id)return {};return {State(audio_state(impl_->id)),audio_error(impl_->id)};}
}
