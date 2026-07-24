#pragma once

// Keep this JSON-heavy header out of widely shared engine headers.

#include "core/ReflectionFwd.hpp"
#include "core/Signal.hpp"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace saida::reflect {

using json = nlohmann::json;

template <typename M, typename Enable = void>
struct Traits {
    static const char* kind() {
        if constexpr (std::is_same_v<M, bool>) return "bool";
        else if constexpr (std::is_floating_point_v<M>) return "float";
        else if constexpr (std::is_integral_v<M>) return "int";
        else if constexpr (std::is_same_v<M, std::string>) return "string";
        else return "json";
    }
    static void to(json& j, const M& v) { j = v; }
    static void from(const json& j, M& v) { v = j.get<M>(); }
};

template <typename E>
struct Traits<E, std::enable_if_t<std::is_enum_v<E>>> {
    static const char* kind() { return "enum"; }
    static void to(json& j, const E& v) { j = static_cast<int>(v); }
    static void from(const json& j, E& v) { v = static_cast<E>(j.get<int>()); }
};

template <>
struct Traits<glm::vec3> {
    static const char* kind() { return "vec3"; }
    static void to(json& j, const glm::vec3& v) { j = json::array({v.x, v.y, v.z}); }
    static void from(const json& j, glm::vec3& v) {
        if (j.is_array() && j.size() == 3)
            v = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    }
};

template <>
struct Traits<glm::vec4> {
    static const char* kind() { return "vec4"; }
    static void to(json& j, const glm::vec4& v) { j = json::array({v.x, v.y, v.z, v.w}); }
    static void from(const json& j, glm::vec4& v) {
        if (j.is_array() && j.size() == 4)
            v = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
    }
};

template <>
struct Traits<glm::quat> {
    static const char* kind() { return "quat"; }
    static void to(json& j, const glm::quat& v) { j = json::array({v.x, v.y, v.z, v.w}); }
    static void from(const json& j, glm::quat& v) {
        if (j.is_array() && j.size() == 4)
            v = glm::quat(j[3].get<float>(), j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    }
};

template <typename V>
inline void pushArg(json& arr, const V& v) {
    if constexpr (std::is_same_v<V, bool>) arr.push_back(v);
    else if constexpr (std::is_arithmetic_v<V>) arr.push_back(v);
    else if constexpr (std::is_enum_v<V>) arr.push_back(static_cast<long long>(v));
    else if constexpr (std::is_same_v<V, std::string>) arr.push_back(v);
    else arr.push_back(nullptr);
}

template <typename A>
inline std::decay_t<A> getArg(const json& arr, std::size_t i) {
    using T = std::decay_t<A>;
    if (i >= arr.size()) return T{};
    const json& e = arr[i];
    if constexpr (std::is_same_v<T, bool>) return e.is_boolean() ? e.get<bool>() : T{};
    else if constexpr (std::is_arithmetic_v<T>) return e.is_number() ? e.get<T>() : T{};
    else if constexpr (std::is_enum_v<T>) return e.is_number() ? static_cast<T>(e.get<int>()) : T{};
    else if constexpr (std::is_same_v<T, std::string>) return e.is_string() ? e.get<std::string>() : T{};
    else return T{};
}

struct PropertyDesc {
    std::string name;
    std::string kind;
    std::string tooltip;
    bool hasRange = false;
    double min = 0.0;
    double max = 0.0;
    std::vector<std::string> enumLabels;
    std::function<void(const void*, json&)> get;
    std::function<void(void*, const json&)> set;
};

struct SignalDesc {
    std::string name;
    int arity = 0;
    std::function<Connection(void*, std::function<void(const json&)>)> connect;
    std::function<void(void*, const json&)> emit;
};

struct SlotDesc {
    std::string name;
    int arity = 0;
    std::function<void(void*, const json&)> invoke;
};

struct PropertyRef {
    PropertyDesc* d = nullptr;
    PropertyRef& range(double lo, double hi) { d->hasRange = true; d->min = lo; d->max = hi; return *this; }
    PropertyRef& tooltip(std::string s) { d->tooltip = std::move(s); return *this; }
    PropertyRef& asset() { d->kind = "asset"; return *this; }
    PropertyRef& enumValues(std::vector<std::string> labels) { d->enumLabels = std::move(labels); return *this; }
};

struct TypeDesc {
    std::string name;
    std::string category;
    std::string doc;
    std::vector<PropertyDesc> properties;
    std::vector<SignalDesc> signals;
    std::vector<SlotDesc> slots;

    const PropertyDesc* findProperty(const std::string& n) const {
        for (const auto& p : properties) if (p.name == n) return &p;
        return nullptr;
    }
    const SignalDesc* findSignal(const std::string& n) const {
        for (const auto& s : signals) if (s.name == n) return &s;
        return nullptr;
    }
    const SlotDesc* findSlot(const std::string& n) const {
        for (const auto& s : slots) if (s.name == n) return &s;
        return nullptr;
    }

    void saveTo(const void* obj, json& j) const {
        for (const auto& p : properties) { json v; p.get(obj, v); j[p.name] = std::move(v); }
    }
    void loadFrom(void* obj, const json& j) const {
        for (const auto& p : properties) {
            auto it = j.find(p.name);
            if (it != j.end()) p.set(obj, *it);
        }
    }

    json manifest() const;
};

template <typename T>
class TypeBuilder {
public:
    explicit TypeBuilder(TypeDesc& d) : d_(d) {}

    TypeBuilder& doc(std::string s) { d_.doc = std::move(s); return *this; }

    template <typename M>
    PropertyRef property(std::string name, M T::*ptr) {
        PropertyDesc pd;
        pd.name = std::move(name);
        pd.kind = Traits<M>::kind();
        pd.get = [ptr](const void* o, json& j) { Traits<M>::to(j, static_cast<const T*>(o)->*ptr); };
        pd.set = [ptr](void* o, const json& j) { Traits<M>::from(j, static_cast<T*>(o)->*ptr); };
        d_.properties.push_back(std::move(pd));
        return PropertyRef{&d_.properties.back()};
    }

    template <typename... A>
    void signal(std::string name, Signal<A...> T::*ptr) {
        SignalDesc s;
        s.name = std::move(name);
        s.arity = static_cast<int>(sizeof...(A));
        s.connect = [ptr](void* o, std::function<void(const json&)> sink) -> Connection {
            T* t = static_cast<T*>(o);
            return (t->*ptr).connect([sink = std::move(sink)](A... args) {
                json arr = json::array();
                (pushArg(arr, args), ...);
                sink(arr);
            });
        };
        s.emit = [ptr](void* o, const json& arr) {
            T* t = static_cast<T*>(o);
            emitImpl(t, ptr, arr, std::index_sequence_for<A...>{});
        };
        d_.signals.push_back(std::move(s));
    }

    template <typename... A>
    void slot(std::string name, void (T::*method)(A...)) {
        SlotDesc s;
        s.name = std::move(name);
        s.arity = static_cast<int>(sizeof...(A));
        s.invoke = [method](void* o, const json& arr) {
            T* t = static_cast<T*>(o);
            invokeImpl(t, method, arr, std::index_sequence_for<A...>{});
        };
        d_.slots.push_back(std::move(s));
    }

private:
    template <typename... A, std::size_t... I>
    static void invokeImpl(T* t, void (T::*method)(A...), const json& arr, std::index_sequence<I...>) {
        (t->*method)(getArg<A>(arr, I)...);
    }

    template <typename... A, std::size_t... I>
    static void emitImpl(T* t, Signal<A...> T::*ptr, const json& arr, std::index_sequence<I...>) {
        (t->*ptr).emit(getArg<A>(arr, I)...);
    }

    TypeDesc& d_;
};

class TypeRegistry {
public:
    static TypeRegistry& instance();

    TypeDesc& add(const std::string& name);
    const TypeDesc* find(const std::string& name) const;

    json manifest(const std::string& categoryFilter = "") const;
    json manifestFor(const std::string& typeName) const;

private:
    std::unordered_map<std::string, std::unique_ptr<TypeDesc>> types_;
};

template <typename T>
const TypeDesc& localDesc() {
    static const TypeDesc desc = [] {
        TypeDesc d;
        TypeBuilder<T> b(d);
        T::describe(b);
        return d;
    }();
    return desc;
}

template <typename T>
void saveObject(const T& obj, json& j) { localDesc<T>().saveTo(&obj, j); }

template <typename T>
void loadObject(T& obj, const json& j) { localDesc<T>().loadFrom(&obj, j); }

} // namespace saida::reflect

#define SAIDA_REFLECT_BEHAVIOUR(Type, NameStr)                                        \
public:                                                                             \
    static constexpr const char* reflectName() { return NameStr; }                  \
    const char* typeName() const override { return NameStr; }                       \
    static void describe(saida::reflect::TypeBuilder<Type>& t);                         \
    void save(nlohmann::json& j) const override { saida::reflect::saveObject(*this, j); } \
    void load(const nlohmann::json& j) override { saida::reflect::loadObject(*this, j); }

#define SAIDA_REFLECT_NODE(Type, NameStr)                                              \
public:                                                                             \
    static constexpr const char* reflectName() { return NameStr; }                  \
    const char* typeName() const override { return NameStr; }                       \
    static void describe(saida::reflect::TypeBuilder<Type>& t);                         \
    void serialize(nlohmann::json& j, saida::ResourceManager& r) const override {       \
        saida::Node::serialize(j, r);                                                   \
        saida::reflect::saveObject(*this, j);                                           \
    }                                                                                \
    void deserialize(const nlohmann::json& j, saida::ResourceManager& r) override {     \
        saida::Node::deserialize(j, r);                                                 \
        saida::reflect::loadObject(*this, j);                                           \
    }
