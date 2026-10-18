#pragma once
#include <memory>
#include <optional>
#include <string>
#include <cstddef>

namespace saida {
bool validHttpUrl(const std::string& url);
struct HttpResponse {
    int status=0;
    std::string body,error;
    bool ok()const{return error.empty()&&status>=200&&status<300;}
};
// Pollable GET. Native I/O runs on a worker; Web uses Fetch without pthreads.
// Cancellation/late completion never access a destroyed request or its host.
class HttpRequest {
public:
    HttpRequest();
    ~HttpRequest();
    HttpRequest(const HttpRequest&)=delete;
    HttpRequest& operator=(const HttpRequest&)=delete;
    void get(const std::string& url,double timeoutSeconds=6.,size_t maximumBytes=16*1024*1024);
    void cancel();
    std::optional<HttpResponse> take();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
