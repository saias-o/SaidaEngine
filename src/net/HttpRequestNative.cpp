#include "net/HttpRequest.hpp"
#include "net/HttpTransport.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <cmath>
#include <climits>
namespace saida {
struct HttpRequest::Impl {
    struct Job{std::atomic<bool> cancelled{false};std::mutex mutex;std::optional<HttpResponse> response;};
    std::shared_ptr<Job> job;
};
HttpRequest::HttpRequest():impl_(std::make_unique<Impl>()){}
HttpRequest::~HttpRequest(){cancel();}
void HttpRequest::cancel(){if(impl_->job)impl_->job->cancelled=true;impl_->job.reset();}
void HttpRequest::get(const std::string& url,double timeout,size_t maximum) {
    cancel();auto job=std::make_shared<Impl::Job>();impl_->job=job;
    if(!validHttpUrl(url)||!std::isfinite(timeout)||timeout<=0||maximum==0||maximum>INT_MAX){job->response=HttpResponse{0,{},"Invalid HTTP request"};return;}
    try {std::thread([job,url,timeout,maximum]{
        HttpResponse response;
        try {
            std::string body;bool tooLarge=false;
            response=http::receive(url,timeout,job->cancelled,[&](const char* data,size_t size){
                if(size>maximum-body.size()){tooLarge=true;return false;}body.append(data,size);return true;
            });
            response.body=std::move(body);if(tooLarge)response.error="HTTP response exceeds its byte budget";
        }catch(...){response.error="HTTP failure";}
        std::lock_guard<std::mutex> lock(job->mutex);job->response=std::move(response);
    }).detach();}catch(...){job->response=HttpResponse{0,{},"HTTP failure"};}
}
std::optional<HttpResponse> HttpRequest::take() {
    if(!impl_->job)return {};
    std::lock_guard<std::mutex> lock(impl_->job->mutex);
    auto result=std::move(impl_->job->response);impl_->job->response.reset();return result;
}
}
