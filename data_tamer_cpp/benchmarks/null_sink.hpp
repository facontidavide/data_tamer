#pragma once

#include "data_tamer/data_sink.hpp"

namespace DataTamer
{

/// Sink that accepts every snapshot and does nothing with it. Used by the
/// benchmarks to measure the front end without any backend cost.
class NullSink : public DataSinkBase
{
public:
  ~NullSink() override { stopThread(); }
  void addChannel(std::string const&, Schema const&) override {}
  bool storeSnapshot(const Snapshot&) override { return true; }
};

}  // namespace DataTamer
