#include "net/HttpRequest.hpp"
namespace saida {
bool validHttpUrl(const std::string& url) {
    if(url.size()>8192||(url.rfind("https://",0)!=0&&url.rfind("http://",0)!=0))return false;
    const auto start=url.find("://")+3;
    if(start>=url.size()||url[start]=='/')return false;
    for(unsigned char c:url)if(c<32||c==127)return false;
    return true;
}
}
