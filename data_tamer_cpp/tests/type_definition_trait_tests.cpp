// DataTamer::TypeDefinitionTrait: describing types without reopening their
// namespace (PickNikRobotics/data_tamer#94).

#include "data_tamer/channel.hpp"
#include "data_tamer/custom_types.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

// Pretend this is a library we can not (or do not want to) modify:
// nothing below is declared inside namespace third_party.
namespace third_party
{
struct Point
{
  double x = 0;
  double y = 0;
};

// class template: one name per instantiation ("Vec2", "Vec3", ...)
template <int N>
struct Vec
{
  std::array<float, N> data{};
};

// variable-size type, selected through enable_if (see below)
struct Polyline
{
  using is_tagged = std::true_type;
  int32_t id = 0;
  std::vector<Point> points;
};

template <typename T, typename = void>
struct is_tagged : std::false_type
{
};
template <typename T>
struct is_tagged<T, std::void_t<typename T::is_tagged>> : T::is_tagged
{
};

// class template whose trait define() returns an owning std::string
template <int N>
struct Mat
{
  std::array<double, N * N> data{};
};

// A type that has BOTH an ADL overload and a trait specialization.
struct Both
{
  int32_t a = 0;
  int32_t b = 0;
};
template <class AddField>
std::string_view TypeDefinition(Both& v, AddField& add)
{
  add("a", &v.a);
  return "BothADL";
}
}  // namespace third_party

namespace DataTamer
{
// full specialization
template <>
struct TypeDefinitionTrait<third_party::Point>
{
  template <class AddField>
  static std::string_view define(third_party::Point& p, AddField& add)
  {
    add("x", &p.x);
    add("y", &p.y);
    return "Point";
  }
};

// partial specialization for a class template; the name is built at runtime
template <int N>
struct TypeDefinitionTrait<third_party::Vec<N>>
{
  static std::string name() { return "Vec" + std::to_string(N); }

  template <class AddField>
  static void define(third_party::Vec<N>& v, AddField& add)
  {
    add("data", &v.data);
  }
};

// partial specialization through the second (void) parameter
template <typename T>
struct TypeDefinitionTrait<T, std::enable_if_t<third_party::is_tagged<T>::value>>
{
  template <class AddField>
  static std::string_view define(T& v, AddField& add)
  {
    add("id", &v.id);
    add("points", &v.points);
    return "Polyline";
  }
};

// define() returning a std::string built at runtime: the name is cached by
// the library (returning it as a string_view would otherwise dangle).
template <int N>
struct TypeDefinitionTrait<third_party::Mat<N>>
{
  template <class AddField>
  static std::string define(third_party::Mat<N>& m, AddField& add)
  {
    add("data", &m.data);
    return "Mat" + std::to_string(N) + "x" + std::to_string(N);
  }
};

// A specialization whose define() can not be called with a generic AddField
// is a compile error, not silently ignored (it would otherwise fall back to an
// ADL TypeDefinition). For example, this fails to compile on first use:
//
//   template <>
//   struct TypeDefinitionTrait<third_party::Point>
//   {
//     static std::string_view define(third_party::Point&, int&);
//   };
//   // error: "DataTamer::TypeDefinitionTrait<T> is specialized, but it has no
//   //         static define(T&, AddField&) callable with a generic AddField"

// the trait wins over the ADL overload
template <>
struct TypeDefinitionTrait<third_party::Both>
{
  template <class AddField>
  static std::string_view define(third_party::Both& v, AddField& add)
  {
    add("a", &v.a);
    add("b", &v.b);
    return "BothTrait";
  }
};
}  // namespace DataTamer

using namespace DataTamer;
using SerializeMe::has_TypeDefinition;
using SerializeMe::has_TypeDefinitionADL;
using SerializeMe::has_TypeDefinitionTrait;

struct NotDescribed
{
  int x;
};

static_assert(has_TypeDefinition<third_party::Point>::value);
static_assert(!has_TypeDefinitionADL<third_party::Point>::value);
static_assert(has_TypeDefinitionTrait<third_party::Point>::value);
static_assert(has_TypeDefinition<third_party::Vec<3>>::value);
static_assert(has_TypeDefinition<third_party::Polyline>::value);
static_assert(has_TypeDefinitionTrait<third_party::Both>::value);
static_assert(has_TypeDefinitionADL<third_party::Both>::value);
static_assert(!has_TypeDefinition<NotDescribed>::value);
static_assert(!SerializeMe::is_TypeDefinitionTrait_specialized<NotDescribed>::value);
static_assert(
    SerializeMe::is_TypeDefinitionTrait_specialized<third_party::Mat<2>>::value);
static_assert(!has_TypeDefinition<std::vector<third_party::Point>>::value);
static_assert(!has_TypeDefinition<std::array<third_party::Vec<2>, 2>>::value);

TEST(TypeDefinitionTrait, SerializeMeRoundTrip)
{
  third_party::Polyline in;
  in.id = 42;
  in.points = { { 1, 2 }, { 3, 4 }, { 5, 6 } };

  const size_t size = SerializeMe::BufferSize(in);
  EXPECT_EQ(size, sizeof(int32_t) + sizeof(uint32_t) + 3 * 2 * sizeof(double));

  std::vector<uint8_t> storage(size);
  SerializeMe::SpanBytes out_span(storage.data(), storage.size());
  SerializeMe::SerializeIntoBuffer(out_span, in);
  EXPECT_EQ(out_span.size(), 0u);

  third_party::Polyline decoded;
  SerializeMe::SpanBytesConst in_span(storage.data(), storage.size());
  SerializeMe::DeserializeFromBuffer(in_span, decoded);
  EXPECT_EQ(decoded.id, 42);
  ASSERT_EQ(decoded.points.size(), 3u);
  EXPECT_EQ(decoded.points[2].x, 5);
  EXPECT_EQ(decoded.points[2].y, 6);
}

TEST(TypeDefinitionTrait, TypeNames)
{
  EXPECT_EQ(CustomTypeName<third_party::Point>::get(), "Point");
  EXPECT_EQ(CustomTypeName<third_party::Vec<2>>::get(), "Vec2");
  EXPECT_EQ(CustomTypeName<third_party::Vec<7>>::get(), "Vec7");
  EXPECT_EQ(CustomTypeName<std::vector<third_party::Vec<7>>>::get(), "Vec7");
  // the runtime name is built once: same storage on every call
  EXPECT_EQ(CustomTypeName<third_party::Vec<2>>::get().data(),
            CustomTypeName<third_party::Vec<2>>::get().data());
  EXPECT_EQ(CustomTypeName<third_party::Polyline>::get(), "Polyline");
  // define() returns std::string: the view must stay valid
  const std::string_view mat3 = CustomTypeName<third_party::Mat<3>>::get();
  const std::string_view mat2 = CustomTypeName<third_party::Mat<2>>::get();
  EXPECT_EQ(mat3, "Mat3x3");
  EXPECT_EQ(mat2, "Mat2x2");
  EXPECT_EQ(mat3.data(), CustomTypeName<third_party::Mat<3>>::get().data());
  EXPECT_EQ(CustomTypeName<third_party::Both>::get(), "BothTrait");
}

TEST(TypeDefinitionTrait, RegisterAndSnapshot)
{
  auto channel = LogChannel::create("chan");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  channel->addDataSink(sink);

  third_party::Point point{ 1, 2 };
  third_party::Vec<2> vec2;
  vec2.data = { 3, 4 };
  std::vector<third_party::Vec<3>> vecs3(2);
  vecs3[1].data = { 5, 6, 7 };
  third_party::Polyline line;
  line.id = 9;
  line.points = { { 10, 11 } };
  third_party::Both both{ 12, 13 };

  channel->registerValue("point", &point);
  channel->registerValue("vec2", &vec2);
  channel->registerValue("vecs3", &vecs3);
  channel->registerValue("line", &line);
  channel->registerValue("both", &both);

  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();

  const size_t expected_size = 2 * sizeof(double)                          // point
                               + 2 * sizeof(float)                         // vec2
                               + sizeof(uint32_t) + 2 * 3 * sizeof(float)  // vecs3
                               + sizeof(int32_t) + sizeof(uint32_t) +
                               2 * sizeof(double)      // line
                               + 2 * sizeof(int32_t);  // both (trait: a and b)
  ASSERT_EQ(sink->latestPayloadSize(), expected_size);

  const auto payload = sink->latestSnapshot().payload;
  SerializeMe::SpanBytesConst span(payload.data(), payload.size());
  third_party::Point point_out;
  third_party::Vec<2> vec2_out;
  std::vector<third_party::Vec<3>> vecs3_out;
  third_party::Polyline line_out;
  third_party::Both both_out;
  SerializeMe::DeserializeFromBuffer(span, point_out);
  SerializeMe::DeserializeFromBuffer(span, vec2_out);
  SerializeMe::DeserializeFromBuffer(span, vecs3_out);
  SerializeMe::DeserializeFromBuffer(span, line_out);
  SerializeMe::DeserializeFromBuffer(span, both_out);
  EXPECT_EQ(span.size(), 0u);
  EXPECT_EQ(point_out.y, 2);
  EXPECT_EQ(vec2_out.data[1], 4);
  ASSERT_EQ(vecs3_out.size(), 2u);
  EXPECT_EQ(vecs3_out[1].data[2], 7);
  EXPECT_EQ(line_out.id, 9);
  ASSERT_EQ(line_out.points.size(), 1u);
  EXPECT_EQ(line_out.points[0].y, 11);
  EXPECT_EQ(both_out.a, 12);
  EXPECT_EQ(both_out.b, 13);

  const auto schema = channel->getSchema();
  const std::string schema_txt = ToStr(schema);

  EXPECT_NE(schema_txt.find("Point point\n"
                            "Vec2 vec2\n"
                            "Vec3[] vecs3\n"
                            "Polyline line\n"
                            "BothTrait both\n"),
            std::string::npos)
      << schema_txt;
  EXPECT_NE(schema_txt.find("MSG: Point\n"
                            "float64 x\n"
                            "float64 y\n"),
            std::string::npos);
  EXPECT_NE(schema_txt.find("MSG: Vec2\n"
                            "float32[2] data\n"),
            std::string::npos);
  EXPECT_NE(schema_txt.find("MSG: Vec3\n"
                            "float32[3] data\n"),
            std::string::npos);
  EXPECT_NE(schema_txt.find("MSG: Polyline\n"
                            "int32 id\n"
                            "Point[] points\n"),
            std::string::npos);
  EXPECT_NE(schema_txt.find("MSG: BothTrait\n"
                            "int32 a\n"
                            "int32 b\n"),
            std::string::npos);
  EXPECT_EQ(schema_txt.find("BothADL"), std::string::npos);
}

TEST(TypeDefinitionTrait, DefineReturningStdString)
{
  auto channel = LogChannel::create("chan");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  channel->addDataSink(sink);

  third_party::Mat<2> mat;
  mat.data = { 1, 2, 3, 4 };
  std::vector<third_party::Mat<3>> mats(1);
  channel->registerValue("mat", &mat);
  channel->registerValue("mats", &mats);

  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  EXPECT_EQ(sink->latestPayloadSize(),
            4 * sizeof(double) + sizeof(uint32_t) + 9 * sizeof(double));

  const auto schema = channel->getSchema();
  const std::string schema_txt = ToStr(schema);
  EXPECT_EQ(schema.custom_types.count("Mat2x2"), 1u);
  EXPECT_EQ(schema.custom_types.count("Mat3x3"), 1u);
  EXPECT_NE(schema_txt.find("Mat2x2 mat\n"
                            "Mat3x3[] mats\n"),
            std::string::npos)
      << schema_txt;
  EXPECT_NE(schema_txt.find("MSG: Mat3x3\n"
                            "float64[9] data\n"),
            std::string::npos)
      << schema_txt;
}
