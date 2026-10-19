#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) noexcept {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(const size_t count) noexcept {
  using Element = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}
