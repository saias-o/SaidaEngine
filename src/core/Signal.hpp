#pragma once

// Typed signal/slot. Connections hold weak control blocks, so dangling links
// become no-ops instead of calls into freed objects.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace saida {

namespace detail {
struct SignalBlockBase {
    virtual ~SignalBlockBase() = default;
    virtual void remove(uint64_t id) = 0;
};
} // namespace detail

// RAII handle to one connection. Move-only; disconnects on destruction.
class Connection {
public:
    Connection() = default;
    Connection(std::weak_ptr<detail::SignalBlockBase> block, uint64_t id)
        : block_(std::move(block)), id_(id) {}

    Connection(Connection&& other) noexcept { moveFrom(other); }
    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            disconnect();
            moveFrom(other);
        }
        return *this;
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() { disconnect(); }

    void disconnect() {
        if (auto b = block_.lock()) b->remove(id_);
        block_.reset();
        id_ = 0;
    }
    bool connected() const { return id_ != 0 && !block_.expired(); }

private:
    void moveFrom(Connection& o) {
        block_ = std::move(o.block_);
        id_ = o.id_;
        o.block_.reset();
        o.id_ = 0;
    }
    std::weak_ptr<detail::SignalBlockBase> block_;
    uint64_t id_ = 0;
};

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    Signal() : block_(std::make_shared<Block>()) {}
    Signal(Signal&&) noexcept = default;
    Signal& operator=(Signal&&) noexcept = default;
    Signal(const Signal&) = delete;             // sharing the slot list would surprise
    Signal& operator=(const Signal&) = delete;

    // Connect a slot; the returned Connection controls its lifetime.
    [[nodiscard]] Connection connect(Slot slot) const {
        uint64_t id = ++block_->nextId;
        block_->slots.emplace_back(id, std::move(slot));
        return Connection(std::static_pointer_cast<detail::SignalBlockBase>(block_), id);
    }

    // Notify every connected slot. Safe to (dis)connect from within a slot.
    void emit(Args... args) const {
        auto snapshot = block_->slots;  // tolerate mutation during dispatch
        for (auto& s : snapshot) s.second(args...);
    }

    size_t slotCount() const { return block_->slots.size(); }

private:
    struct Block : detail::SignalBlockBase {
        std::vector<std::pair<uint64_t, Slot>> slots;
        uint64_t nextId = 0;
        void remove(uint64_t id) override {
            slots.erase(std::remove_if(slots.begin(), slots.end(),
                                       [id](const auto& s) { return s.first == id; }),
                        slots.end());
        }
    };
    std::shared_ptr<Block> block_;
};

} // namespace saida
