#pragma once

/**
 * Forward declarations of the main DataTamer types, in the spirit of <iosfwd>.
 * Include it from headers that only name these types (pointers, references,
 * std::shared_ptr<T>); include the full header (channel.hpp, data_sink.hpp, ...)
 * wherever a type is used by value or its members are called. It has no includes and
 * can be combined with the full headers in any order.
 *
 * Declare each type with the same class or struct key as its definition (the
 * fwd_header_* build targets check it).
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
