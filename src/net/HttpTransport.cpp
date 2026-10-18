#include "net/HttpTransport.hpp"
#include "saida/HttpCertificates.hpp"
#include <curl/curl.h>
#include <mutex>
#include <cstdlib>
#include <algorithm>

namespace saida::http {
HttpResponse receive(const std::string& url,double timeout,const std::atomic<bool>& cancelled,
    const std::function<bool(const char*,size_t)>& write) {
    HttpResponse response;
    if(!validHttpUrl(url)){response.error="Invalid HTTP URL";return response;}
    if(cancelled){response.error="Cancelled";return response;}
    static std::once_flag initialized;
    std::call_once(initialized,[]{curl_global_init(CURL_GLOBAL_DEFAULT);});
    auto* request=curl_easy_init();
    if(!request){response.error="HTTP allocation failed";return response;}
    struct Context{const std::atomic<bool>& cancel;const std::function<bool(const char*,size_t)>& write;};
    Context context{cancelled,write};
    char error[CURL_ERROR_SIZE]{};
    curl_easy_setopt(request,CURLOPT_URL,url.c_str());
    curl_easy_setopt(request,CURLOPT_USERAGENT,"SaidaEngine/1.0");
    curl_easy_setopt(request,CURLOPT_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(request,CURLOPT_REDIR_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(request,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(request,CURLOPT_MAXREDIRS,8L);
    curl_easy_setopt(request,CURLOPT_CONNECTTIMEOUT_MS,6000L);
    curl_easy_setopt(request,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(request,CURLOPT_LOW_SPEED_LIMIT,1L);
    curl_easy_setopt(request,CURLOPT_LOW_SPEED_TIME,15L);
    if(timeout>0)curl_easy_setopt(request,CURLOPT_TIMEOUT_MS,long(std::clamp(timeout,.05,300.)*1000.));
    curl_easy_setopt(request,CURLOPT_ERRORBUFFER,error);
    curl_easy_setopt(request,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(request,CURLOPT_XFERINFODATA,&context);
    curl_easy_setopt(request,CURLOPT_XFERINFOFUNCTION,+[](void* data,curl_off_t,curl_off_t,curl_off_t,curl_off_t){
        return static_cast<Context*>(data)->cancel.load()?1:0;
    });
    curl_easy_setopt(request,CURLOPT_WRITEDATA,&context);
    curl_easy_setopt(request,CURLOPT_WRITEFUNCTION,+[](char* data,size_t size,size_t count,void* user)->size_t{
        auto& c=*static_cast<Context*>(user);const auto bytes=size*count;
        if(c.cancel)return 0;
        try{return c.write(data,bytes)?bytes:0;}catch(...){return 0;}
    });
    curl_blob certificates{const_cast<char*>(kHttpCertificates),sizeof(kHttpCertificates)-1,CURL_BLOB_NOCOPY};
    curl_easy_setopt(request,CURLOPT_CAINFO_BLOB,&certificates);
    // Match the engine's offline-test convention on every native platform.
    if(const auto* proxy=std::getenv(url.rfind("https://",0)==0?"HTTPS_PROXY":"HTTP_PROXY"))
        curl_easy_setopt(request,CURLOPT_PROXY,proxy);
    const auto result=curl_easy_perform(request);long status=0;
    curl_easy_getinfo(request,CURLINFO_RESPONSE_CODE,&status);response.status=int(status);
    if(result!=CURLE_OK)response.error=cancelled?"Cancelled":error[0]?error:curl_easy_strerror(result);
    curl_easy_cleanup(request);
    return response;
}
}
