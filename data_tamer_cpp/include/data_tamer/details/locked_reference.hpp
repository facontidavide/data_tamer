#pragma once

#include "data_tamer/details/write_mutex.hpp"

#include <atomic>
#include <mutex>
#include <utility>

/// The channel's transaction lock. Exclusive and priority-inheriting: a
/// writer holding it is boosted to the priority of a waiting snapshot thread.
/// (Was std::shared_mutex; lock_shared() is no longer available.)
using Mutex = DataTamer::WriteMutex;

namespace DataTamer
{
using ::Mutex;

/**
 * @brief Const pointer that holds the mutex for its lifetime. Move-only; the
 * lock travels with the object and is released exactly once.
 */
template <typename T>
class ConstPtr
{
public:
  ConstPtr(const T* obj, Mutex* mutex)
    : obj_(obj)
    , lock_(mutex ? std::unique_lock<Mutex>(*mutex) : std::unique_lock<Mutex>())
  {}
  ConstPtr(ConstPtr&& other) noexcept
    : obj_(std::exchange(other.obj_, nullptr)), lock_(std::move(other.lock_))
  {}
  ConstPtr& operator=(ConstPtr&& other) noexcept
  {
    obj_ = std::exchange(other.obj_, nullptr);
    lock_ = std::move(other.lock_);  // releases any lock this object held
    return *this;
  }

  explicit operator bool() const { return obj_ != nullptr; }
  [[deprecated("the lock is held for the lifetime of this object; there is nothing to "
               "lock manually")]]
  Mutex* mutex()
  {
    return lock_.mutex();
  }
  const T& operator*() const { return *obj_; }
  const T* operator->() const { return obj_; }

private:
  const T* obj_ = nullptr;
  std::unique_lock<Mutex> lock_;
};

/**
 * @brief Mutable pointer that holds the mutex for its lifetime. Move-only.
 */
template <typename T>
class MutablePtr
{
public:
  MutablePtr(T* obj, Mutex* mutex)
    : obj_(obj)
    , lock_(mutex ? std::unique_lock<Mutex>(*mutex) : std::unique_lock<Mutex>())
  {}
  MutablePtr(MutablePtr&& other) noexcept
    : obj_(std::exchange(other.obj_, nullptr)), lock_(std::move(other.lock_))
  {}
  MutablePtr& operator=(MutablePtr&& other) noexcept
  {
    obj_ = std::exchange(other.obj_, nullptr);
    lock_ = std::move(other.lock_);  // releases any lock this object held
    return *this;
  }

  explicit operator bool() const { return obj_ != nullptr; }
  [[deprecated("the lock is held for the lifetime of this object; there is nothing to "
               "lock manually")]]
  Mutex* mutex()
  {
    return lock_.mutex();
  }
  T& operator*() { return *obj_; }
  T* operator->() { return obj_; }

private:
  T* obj_ = nullptr;
  std::unique_lock<Mutex> lock_;
};

/**
 * @brief Write-back proxy for an atomic scalar: holds a local copy, and the
 * destructor stores it back with one atomic store. No lock is involved, so
 * two overlapping proxies on the same value are last-writer-wins.
 */
template <typename T>
class AtomicProxy
{
public:
  explicit AtomicProxy(std::atomic<T>* target)
    : target_(target), copy_(target ? target->load(std::memory_order_relaxed) : T{})
  {}
  AtomicProxy(const AtomicProxy&) = delete;
  AtomicProxy& operator=(const AtomicProxy&) = delete;
  AtomicProxy(AtomicProxy&& other) noexcept : target_(other.target_), copy_(other.copy_)
  {
    other.target_ = nullptr;
  }
  AtomicProxy& operator=(AtomicProxy&& other) noexcept
  {
    if(this != &other)
    {
      commit();
      target_ = other.target_;
      copy_ = other.copy_;
      other.target_ = nullptr;
    }
    return *this;
  }
  ~AtomicProxy() { commit(); }

  explicit operator bool() const { return target_ != nullptr; }
  T& operator*() { return copy_; }
  T* operator->() { return &copy_; }

private:
  void commit()
  {
    if(target_)
    {
      target_->store(copy_, std::memory_order_relaxed);
    }
  }
  std::atomic<T>* target_;
  T copy_;
};

/// Read-only counterpart of AtomicProxy: a copy taken at construction.
template <typename T>
class AtomicConstProxy
{
public:
  explicit AtomicConstProxy(const std::atomic<T>* target)
    : valid_(target != nullptr)
    , copy_(target ? target->load(std::memory_order_relaxed) : T{})
  {}
  explicit operator bool() const { return valid_; }
  const T& operator*() const { return copy_; }
  const T* operator->() const { return &copy_; }

private:
  bool valid_;
  T copy_;
};

}  // namespace DataTamer
