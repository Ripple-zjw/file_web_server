#pragma once

#include <cstddef>
#include <vector>

template <typename T>
class ConnectionPool {
public:
    static constexpr size_t MAX_CONNECTIONS = 16384;

    ConnectionPool()
        : connections_(MAX_CONNECTIONS),
          free_list_(MAX_CONNECTIONS)
    {
        for (size_t i = 0; i < MAX_CONNECTIONS; ++i)
            free_list_[i] = i;
    }

    T* acquire() noexcept {
        if (free_head_ >= MAX_CONNECTIONS)
            return nullptr;
        auto idx = free_list_[free_head_++];
        ++active_count_;
        return &connections_[idx];
    }

    void release(T* conn) noexcept {
        auto idx = static_cast<size_t>(conn - connections_.data());
        if (idx >= MAX_CONNECTIONS) return;
        free_list_[--free_head_] = idx;
        --active_count_;
    }

    size_t active_count() const noexcept { return active_count_; }
    size_t capacity()  const noexcept { return MAX_CONNECTIONS; }

    T*       begin()       noexcept { return connections_.data(); }
    T*       end()         noexcept { return connections_.data() + MAX_CONNECTIONS; }
    const T* begin() const noexcept { return connections_.data(); }
    const T* end()   const noexcept { return connections_.data() + MAX_CONNECTIONS; }

private:
    std::vector<T>        connections_;
    std::vector<size_t>   free_list_;
    size_t                free_head_ = 0;
    size_t                active_count_ = 0;
};
