#pragma once

#include "data_tamer/details/shared_state.hpp"

namespace DataTamer
{

/**
 * @brief Read-only access to a non-scalar LoggedValue. Holds the channel's write
 * transaction while it lives, so it nests inside scopedWrite() and other guards, and
 * must be destroyed on the thread that created it. Not movable:
 * `auto p = value->getConstPtr();` is fine, storing it is not.
 */
template <typename T>
class ConstPtr
{
public:
  ConstPtr(const T* obj, ChannelSharedState& state) : obj_(obj), tx_(state) {}
  ConstPtr(const ConstPtr&) = delete;
  ConstPtr& operator=(const ConstPtr&) = delete;

  const T& operator*() const { return *obj_; }
  const T* operator->() const { return obj_; }

private:
  const T* obj_;
  ChannelSharedState::Transaction tx_;
};

/// Mutable counterpart of ConstPtr. The snapshot thread waits while it lives: keep the
/// scope short and allocation-free.
template <typename T>
class MutablePtr
{
public:
  MutablePtr(T* obj, ChannelSharedState& state) : obj_(obj), tx_(state) {}
  MutablePtr(const MutablePtr&) = delete;
  MutablePtr& operator=(const MutablePtr&) = delete;

  T& operator*() { return *obj_; }
  T* operator->() { return obj_; }

private:
  T* obj_;
  ChannelSharedState::Transaction tx_;
};

}  // namespace DataTamer
