// Bencoding (BitTorrent BEP 3), the format of DHT messages: integers, byte
// strings, lists and dictionaries with sorted keys. The decoder is bounded in
// depth and item count and refuses anything malformed or trailing.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace saida::p2p {

struct BValue {
    enum Type : uint8_t { Int, Str, List, Dict } type = Str;
    int64_t i = 0;
    std::string s;
    std::vector<BValue> list;
    std::vector<std::pair<std::string, BValue>> dict;  // kept sorted by encode()

    static BValue integer(int64_t v) { BValue b; b.type = Int, b.i = v; return b; }
    static BValue string(std::string v) { BValue b; b.type = Str, b.s = std::move(v); return b; }
    static BValue bytes(const void* p, size_t n) { return string(std::string(static_cast<const char*>(p), n)); }
    static BValue makeList() { BValue b; b.type = List; return b; }
    static BValue makeDict() { BValue b; b.type = Dict; return b; }

    BValue& set(std::string key, BValue v) {
        for (auto& kv : dict)
            if (kv.first == key) return kv.second = std::move(v);
        dict.emplace_back(std::move(key), std::move(v));
        return dict.back().second;
    }
    const BValue* get(std::string_view key) const {
        if (type != Dict) return nullptr;
        for (const auto& kv : dict)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const std::string* str(std::string_view key) const {
        const BValue* v = get(key);
        return v && v->type == Str ? &v->s : nullptr;
    }
    const BValue* dictAt(std::string_view key) const {
        const BValue* v = get(key);
        return v && v->type == Dict ? v : nullptr;
    }
    bool integerAt(std::string_view key, int64_t& out) const {
        const BValue* v = get(key);
        if (!v || v->type != Int) return false;
        out = v->i;
        return true;
    }

    void encode(std::string& out) const {
        switch (type) {
        case Int:
            out += 'i', out += std::to_string(i), out += 'e';
            break;
        case Str:
            out += std::to_string(s.size()), out += ':', out += s;
            break;
        case List:
            out += 'l';
            for (const BValue& v : list) v.encode(out);
            out += 'e';
            break;
        case Dict: {
            std::vector<const std::pair<std::string, BValue>*> sorted;
            for (const auto& kv : dict) sorted.push_back(&kv);
            for (size_t a = 1; a < sorted.size(); ++a)
                for (size_t b = a; b > 0 && sorted[b]->first < sorted[b - 1]->first; --b) std::swap(sorted[b], sorted[b - 1]);
            out += 'd';
            for (const auto* kv : sorted) {
                out += std::to_string(kv->first.size()), out += ':', out += kv->first;
                kv->second.encode(out);
            }
            out += 'e';
            break;
        }
        }
    }
    std::string encode() const {
        std::string out;
        encode(out);
        return out;
    }

    // The whole buffer must be exactly one value.
    static bool decode(const uint8_t* data, size_t size, BValue& out) {
        Parser p{reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + size, 0};
        return p.value(out, 0) && p.at == p.end;
    }

private:
    struct Parser {
        const char* at;
        const char* end;
        size_t items;
        bool number(int64_t& v, char stop) {
            bool negative = false;
            if (at < end && *at == '-') negative = true, ++at;
            const char* start = at;
            uint64_t n = 0;
            while (at < end && *at >= '0' && *at <= '9') {
                if (n > (uint64_t(1) << 62)) return false;
                n = n * 10 + uint64_t(*at++ - '0');
            }
            if (at == start || at >= end || *at != stop || (at - start > 1 && *start == '0')) return false;
            ++at;
            v = negative ? -int64_t(n) : int64_t(n);
            return !(negative && n == 0);
        }
        bool value(BValue& v, int depth) {
            if (at >= end || depth > 16 || ++items > 4096) return false;
            const char c = *at;
            if (c == 'i') {
                ++at;
                v.type = Int;
                return number(v.i, 'e');
            }
            if (c == 'l' || c == 'd') {
                ++at;
                v.type = c == 'l' ? List : Dict;
                while (at < end && *at != 'e') {
                    if (v.type == List) {
                        v.list.emplace_back();
                        if (!value(v.list.back(), depth + 1)) return false;
                    } else {
                        BValue key;
                        if (!value(key, depth + 1) || key.type != Str) return false;
                        v.dict.emplace_back(std::move(key.s), BValue{});
                        if (!value(v.dict.back().second, depth + 1)) return false;
                    }
                }
                if (at >= end) return false;
                ++at;
                return true;
            }
            int64_t n;
            if (!number(n, ':') || n < 0 || n > end - at) return false;
            v.type = Str;
            v.s.assign(at, size_t(n));
            at += n;
            return true;
        }
    };
};

} // namespace saida::p2p
