#include "net/HttpRequest.hpp"
#include <emscripten.h>
#include <cmath>

namespace saida {
EM_JS_DEPS(http_strings,"$lengthBytesUTF8,$stringToUTF8");
EM_JS(int,http_begin,(const char* url,int timeout,int budget),{
    const requests=Module.saidaHttp||(Module.saidaHttp={next:1,jobs:new Map()});
    const id=requests.next++,job={controller:new AbortController(),done:false,status:0,body:new Uint8Array(),error:""};
    requests.jobs.set(id,job);
    const timer=setTimeout(()=>{job.error='HTTP timeout';job.controller.abort();},timeout);
    (async()=>{
        try {
            const response=await fetch(UTF8ToString(url),{signal:job.controller.signal,mode:'cors',credentials:'omit'});
            job.status=response.status;
            const reader=response.body.getReader(),parts=[];let size=0;
            while(true){
                const part=await reader.read();if(part.done)break;
                size+=part.value.length;
                if(size>budget){job.error='HTTP response exceeds its byte budget';await reader.cancel();throw Error(job.error);}
                parts.push(part.value);
            }
            const body=new Uint8Array(size);let offset=0;
            for(const part of parts){body.set(part,offset);offset+=part.length;}job.body=body;
        }catch(error){if(!job.error)job.error=String(error.message||error);}
        finally{clearTimeout(timer);if(requests.jobs.get(id)===job)job.done=true;}
    })();
    return id;
});
EM_JS(void,http_cancel,(int id),{
    const jobs=Module.saidaHttp&&Module.saidaHttp.jobs,job=jobs&&jobs.get(id);
    if(job){jobs.delete(id);job.controller.abort();}
});
EM_JS(int,http_done,(int id),{const job=Module.saidaHttp.jobs.get(id);return job&&job.done?1:0;});
EM_JS(int,http_status,(int id),{return Module.saidaHttp.jobs.get(id).status;});
EM_JS(int,http_size,(int id),{return Module.saidaHttp.jobs.get(id).body.length;});
EM_JS(void,http_copy,(int id,char* out),{HEAPU8.set(Module.saidaHttp.jobs.get(id).body,out);});
EM_JS(int,http_error_size,(int id),{return lengthBytesUTF8(Module.saidaHttp.jobs.get(id).error);});
EM_JS(void,http_error_copy,(int id,char* out,int capacity),{stringToUTF8(Module.saidaHttp.jobs.get(id).error,out,capacity);});
struct HttpRequest::Impl {int id=0;std::optional<HttpResponse> invalid;};
HttpRequest::HttpRequest():impl_(std::make_unique<Impl>()){}
HttpRequest::~HttpRequest(){cancel();}
void HttpRequest::cancel(){if(impl_->id)http_cancel(impl_->id);impl_->id=0;impl_->invalid.reset();}
void HttpRequest::get(const std::string& url,double timeout,size_t maximum) {
    cancel();
    if(!validHttpUrl(url)||!std::isfinite(timeout)||timeout<=0||maximum==0||maximum>size_t(0x7fffffff)){
        impl_->invalid=HttpResponse{0,{},"Invalid HTTP request"};return;
    }
    impl_->id=http_begin(url.c_str(),int(std::fmin(timeout,300.)*1000.),int(maximum));
}
std::optional<HttpResponse> HttpRequest::take() {
    if(impl_->invalid){auto result=std::move(impl_->invalid);impl_->invalid.reset();return result;}
    const int id=impl_->id;if(!id||!http_done(id))return {};
    HttpResponse result;result.status=http_status(id);result.body.resize(http_size(id));
    if(!result.body.empty())http_copy(id,result.body.data());
    result.error.resize(http_error_size(id));
    if(!result.error.empty())http_error_copy(id,result.error.data(),int(result.error.size()+1));
    cancel();return result;
}
}
