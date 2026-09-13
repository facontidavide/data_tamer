#pragma once

#include "data_tamer/data_sink.hpp"

namespace DataTamer
{

/// Sink that accepts every snapshot and does nothing with it. Used by the
/// benchmarks to measure the front end without any backend cost.
class NullSink : public DataSink
{
public:
  static std::shared_ptr<SinkWorker> create() { return SinkWorker::create<NullSink>(); }
  void onSchema(Schema const&) override {}
  void onSnapshot(const SnapshotRef&) override {}
};

}  // namespace DataTamer
