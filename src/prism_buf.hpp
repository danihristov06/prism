#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace prism {

template <typename T>
class DefaultInitAlloc : public std::allocator<T> {
public:
    using Base = std::allocator<T>;
    using value_type = T;

    template <typename U>
    struct rebind {
        using other = DefaultInitAlloc<U>;
    };

    DefaultInitAlloc() noexcept = default;
    DefaultInitAlloc(const DefaultInitAlloc&) noexcept = default;

    template <typename U>
    DefaultInitAlloc(const DefaultInitAlloc<U>&) noexcept {}

    template <typename U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(p)) U;
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    bool operator==(const DefaultInitAlloc<U>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const DefaultInitAlloc<U>&) const noexcept {
        return false;
    }
};

using ByteVec = std::vector<uint8_t, DefaultInitAlloc<uint8_t>>;

} // namespace prism
