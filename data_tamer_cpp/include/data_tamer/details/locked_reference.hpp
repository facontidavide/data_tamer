#pragma once

#include "data_tamer/details/shared_state.hpp"

namespace DataTamer
{

/**
 * @brief Read-only access to a non-scalar LoggedValue. Holds the channel's
 * write transaction for its lifetime, so it nests inside scopedWrite() and
 * inside another guard on the same channel. Not movable, like std::lock_guard:
 * `auto p = value->getConstPtr();` is fine (elided), storing it is not.
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

/// Mutable counterpart of ConstPtr: the snapshot thread waits while it lives,
/// so keep the scope short and allocation-free.
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
