#pragma once

/**
 * Forward declarations of the main DataTamer types, in the spirit of <iosfwd>.
 *
 * Include this header, instead of declaring the classes by hand, from headers
 * that only name these types (as pointers, references, std::shared_ptr<T> in
 * signatures, ...) and do not need their definitions. It has no includes and
 * always matches the real declarations, so it cannot drift from them.
 *
 * Include the full header (channel.hpp, data_sink.hpp, ...) wherever a type is
 * used by value or its members are called. This header and the full headers can
 * be included in any order.
 */

namespace DataTamer
{

class ChannelsRegistry;
class DataSink;
class LogChannel;
class RegistrationID;
class SinkWorker;
class SnapshotRef;

struct Schema;
struct Snapshot;

template <typename T>
class LoggedValue;

}  // namespace DataTamer
