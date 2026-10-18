#include "audio/AudioStream.hpp"
#include "net/HttpRequest.hpp"
#include <memory>
// Force a final Android shared-library link, including the network, TLS,
// decoder and device symbols. A static archive alone cannot prove this.
extern "C" __attribute__((visibility("default"))) void* saida_media_create(){return new saida::AudioStream;}
extern "C" __attribute__((visibility("default"))) bool saida_media_open(void* stream,const char* url){return static_cast<saida::AudioStream*>(stream)->open(url);}
extern "C" __attribute__((visibility("default"))) void saida_media_destroy(void* stream){delete static_cast<saida::AudioStream*>(stream);}
extern "C" __attribute__((visibility("default"))) void* saida_http_create(const char* url){auto request=std::make_unique<saida::HttpRequest>();request->get(url);return request.release();}
extern "C" __attribute__((visibility("default"))) void saida_http_destroy(void* request){delete static_cast<saida::HttpRequest*>(request);}
