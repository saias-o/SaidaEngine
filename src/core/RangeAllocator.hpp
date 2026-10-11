#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace saida {
// First-fit arena ranges in caller-defined units, coalesced on release.
class RangeAllocator {
public:
    explicit RangeAllocator(uint64_t capacity = 0) : capacity_(capacity) {
        if(capacity) free_.push_back({0,capacity});
    }
    std::optional<uint64_t> allocate(uint64_t count) {
        if(!count)return std::nullopt;
        for(auto it=free_.begin();it!=free_.end();++it)if(it->count>=count) {
            const auto start=it->start;it->start+=count;it->count-=count;
            if(!it->count)free_.erase(it);
            used_+=count;return start;
        }
        return std::nullopt;
    }
    void release(uint64_t start,uint64_t count) {
        if(!count||start>capacity_||count>capacity_-start||count>used_)
            throw std::invalid_argument("invalid arena release");
        auto it=std::lower_bound(free_.begin(),free_.end(),start,[](const Range& range,uint64_t value){return range.start<value;});
        if((it!=free_.end()&&start+count>it->start)||
           (it!=free_.begin()&&(it-1)->start+(it-1)->count>start))
            throw std::invalid_argument("overlapping arena release");
        it=free_.insert(it,{start,count});used_-=count;
        if(it!=free_.begin()&&(it-1)->start+(it-1)->count==it->start) {
            (it-1)->count+=it->count;it=free_.erase(it)-1;
        }
        if(it+1!=free_.end()&&it->start+it->count==(it+1)->start) {
            it->count+=(it+1)->count;free_.erase(it+1);
        }
    }
    uint64_t used() const{return used_;}
    uint64_t largestFree() const {
        uint64_t result=0;for(const auto& range:free_)result=std::max(result,range.count);return result;
    }
private:
    struct Range{uint64_t start,count;};
    uint64_t capacity_=0,used_=0;
    std::vector<Range> free_;
};
} // namespace saida
