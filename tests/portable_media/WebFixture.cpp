#include "audio/AudioStream.hpp"
#include "net/HttpRequest.hpp"
#include <emscripten.h>
#include <memory>
#include <optional>
static std::unique_ptr<saida::AudioStream> stream;
static std::unique_ptr<saida::HttpRequest> request;
static std::optional<saida::HttpResponse> response;
extern "C" {
EMSCRIPTEN_KEEPALIVE int media_open(const char* url){return stream->open(url);}
EMSCRIPTEN_KEEPALIVE int media_state(){return int(stream->status().state);}
EMSCRIPTEN_KEEPALIVE int media_error(){return int(stream->status().error);}
EMSCRIPTEN_KEEPALIVE void media_stop(){stream->stop();}
EMSCRIPTEN_KEEPALIVE void media_volume(float volume){stream->setVolume(volume);}
EMSCRIPTEN_KEEPALIVE void http_get(const char* url,float timeout,int bytes){response.reset();request->get(url,timeout,size_t(bytes));}
EMSCRIPTEN_KEEPALIVE void http_abort(){request->cancel();response.reset();}
EMSCRIPTEN_KEEPALIVE int http_poll(){if(!response)response=request->take();return bool(response);}
EMSCRIPTEN_KEEPALIVE int http_code(){return response?response->status:0;}
EMSCRIPTEN_KEEPALIVE const char* http_body(){return response?response->body.c_str():"";}
EMSCRIPTEN_KEEPALIVE const char* http_failure(){return response?response->error.c_str():"";}
}
int main(){stream=std::make_unique<saida::AudioStream>();request=std::make_unique<saida::HttpRequest>();return 0;}
