//*********************************************************************************************************************************
//
// PROJECT:							BootloaderCherryUSB++
// FILE:								usb_memory.hpp
// SUBSYSTEM:						custom unique_ptr for usb.
// LANGUAGE:						C++
// TARGET OS:						None
// LIBRARY DEPENDANCE:	Standard
// NAMESPACE:						None
// AUTHOR:							Gavin Blakeman (GGB)
// LICENSE:             GPLv2
//
//                      Copyright 2026 Gavin Blakeman.                      
//
// OVERVIEW:            Proceses and dispatches incoming messages.
//
// HISTORY:             2026-11-03/GGB - File Created
//
//*********************************************************************************************************************************

#ifndef USB_MEMORY_HPP
#define USB_MEMORY_HPP

// Standard C++ header files
#include <cstddef>
#include <concepts>
#include <new>
#include <type_traits>
#include <utility>

namespace USB
{

  template<auto Allocator>
  concept AllocatorFunction = 
    requires(std::size_t size)
    {
      { Allocator(size) } -> std::same_as<void *>;
    };

  template<auto Deleter>
  concept DeallocatorFunction =
    requires(void* ptr)
    {
      { Deleter(ptr) } -> std::same_as<void>;
    };

  template<typename T, auto Deleter>
  requires DeallocatorFunction<Deleter>
  class unique_ptr
  {
  public:
    explicit unique_ptr(T* ptr = nullptr) noexcept : ptr_{ptr} {}
    ~unique_ptr() noexcept { reset(); }
    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;

    unique_ptr(unique_ptr&& other) noexcept : ptr_{other.release()} {}

    unique_ptr& operator=(unique_ptr&& other) noexcept
    {
      if (this != &other)
      {
        reset(other.release());
      }

      return *this;
    }

    [[nodiscard]] T* get() const noexcept
    {
      return ptr_;
    }

    [[nodiscard]] T* release() noexcept
    {
      return std::exchange(ptr_, nullptr);
    }

    void reset(T* ptr = nullptr) noexcept
    {
      if (ptr_ != ptr)
      {
        if (ptr_)
        {
          ptr_->~T();
          Deleter(ptr_);
        }

        ptr_ = ptr;
      }
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
      return ptr_ != nullptr;
    }

    T& operator*() const noexcept
    {
      return *ptr_;
    }

    T* operator->() const noexcept
    {
      return ptr_;
    }

    void swap(unique_ptr& other) noexcept
    {
      std::swap(ptr_, other.ptr_);
    }

    [[nodiscard]] bool operator==(const unique_ptr& other) const noexcept
    {
        return ptr_ == other.ptr_;
    }

  private:
    T* ptr_{};
    std::size_t size_{};
  };

  template<typename T, auto Deleter>
  requires DeallocatorFunction<Deleter>
  class unique_ptr<T[], Deleter>
  {
  public:
    explicit unique_ptr(T* ptr = nullptr, std::size_t size = 0) noexcept
      : ptr_{ptr}, size_{size} {}

    ~unique_ptr() noexcept { reset(); }

    // ... move operations ...

    [[nodiscard]] T* get() const noexcept { return ptr_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] T& operator[](std::size_t index) const noexcept
    {
      return ptr_[index];
    }

    [[nodiscard]] T* release() noexcept
    {
      return std::exchange(ptr_, nullptr);
    }

    void reset(T* ptr = nullptr, std::size_t size = 0) noexcept
    {
      if (ptr_ != ptr)
      {
        if (ptr_)
        {
          for (std::size_t i = size_; i > 0; --i)
          {
              ptr_[i - 1].~T();
          }

          Deleter(ptr_);
        }

        ptr_ = ptr;
        size_ = size;
      }
    }

    void swap(unique_ptr& other) noexcept
    {
      std::swap(ptr_, other.ptr_);
      std::swap(size_, other.size_);
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
      return ptr_ != nullptr;
    }

    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept
    {
      return ptr_ == nullptr;
    }

    [[nodiscard]] bool operator!=(std::nullptr_t) const noexcept
    {
        return ptr_ != nullptr;
    }


  private:
    T* ptr_{};
    std::size_t size_{};
  };

  template<typename T, auto Allocator, auto Deleter, typename... Args>
    requires (!std::is_array_v<T> && AllocatorFunction<Allocator> && DeallocatorFunction<Deleter>)
  [[nodiscard]] auto make_unique(Args&&... args) noexcept
  { 
    void* memory = Allocator(sizeof(T));

    if (!memory)
    {
      return unique_ptr<T, Deleter>{};
    }

    T* ptr = new (memory) T(std::forward<Args>(args)...);

     return unique_ptr<T, Deleter>{ptr};
  }

  template<typename T, auto Allocator, auto Deleter>
  requires std::is_unbounded_array_v<T> && AllocatorFunction<Allocator> && DeallocatorFunction<Deleter>
  [[nodiscard]]
  unique_ptr<T, Deleter> make_unique(std::size_t count) noexcept
  {
    using Element = std::remove_extent_t<T>;
    if (count == 0)
    {
      return {};
    }

    void* memory = Allocator(sizeof(Element) * count);

    if (!memory)
        return {};

    Element* ptr = static_cast<Element *>(memory);

    for (std::size_t i = 0; i < count; ++i)
    {
      new (&ptr[i]) Element{};
    }

    return unique_ptr<T, Deleter>{ptr, count};
  }

} // namespace USB

#endif // USB_MEMORY_HPP