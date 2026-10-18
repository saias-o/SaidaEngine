#include "net/HttpRequest.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace saida;
static void require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL HTTP: %s\n",message);std::exit(1);}}
static HttpResponse wait(HttpRequest& request){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(std::chrono::steady_clock::now()<deadline){if(auto response=request.take())return *response;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    require(false,"request exceeded its timeout");return {};
}
int main(int argc,char** argv){
    HttpRequest request;
    if(argc>2&&std::string(argv[1])=="--fetch"){
        request.get(argv[2]);const auto response=wait(request);
        std::printf("HTTP status=%d bytes=%zu error=%s\n",response.status,response.body.size(),response.error.c_str());
        require(response.ok(),"real HTTPS response");return 0;
    }
    for(const char* url:{"","file:///private","https://host\nInjected: yes","http:///missing"}){
        request.get(url);require(!wait(request).error.empty(),"invalid URL rejected");
    }
    require(!request.take(),"responses are consumed once");
    if(argc>1){
        const std::string base=argv[1];
        request.get(base+"/json");auto response=wait(request);require(response.ok()&&response.body=="{\"station\":\"local\"}","HTTP JSON body");
        request.get(base+"/404");response=wait(request);require(response.status==404&&!response.ok(),"HTTP status preserved");
        request.get(base+"/large",2,128);require(!wait(request).error.empty(),"body size budget enforced");
        request.get(base+"/slow",.05);require(!wait(request).error.empty(),"timeout enforced");
        request.get(base+"/file-redirect");require(!wait(request).ok(),"redirect cannot escape HTTP protocols");
        request.get(base+"/slow");request.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));require(!request.take(),"cancelled completion cannot reappear");
        request.get(base+"/slow");request.get(base+"/json");require(wait(request).ok(),"replacement returns the new request only");
    }
    std::puts("PASS HTTP URL validation, bounded response, timeout, redirect and cancellation");
}
