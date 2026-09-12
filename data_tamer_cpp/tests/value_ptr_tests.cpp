#include "data_tamer/values.hpp"
#include "data_tamer/custom_types.hpp"
#include "../examples/geometry_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <vector>

using namespace DataTamer;

static std::vector<uint8_t> serialize(const ValuePtr& ptr)
{
  std::vector<uint8_t> out(ptr.getSerializedSize());
  SerializeMe::SpanBytes span(out);
  ptr.serialize(span);
  EXPECT_EQ(span.size(), 0u) << "serializer wrote fewer bytes than getSerializedSize()";
  return out;
}

TEST(ValuePtrGolden, Double)
{
  const double v = 1.5;  // 0x3FF8000000000000 little-endian
  ValuePtr ptr(&v);
  ASSERT_EQ(ptr.type(), BasicType::FLOAT64);
  ASSERT_FALSE(ptr.isVector());
  const std::vector<uint8_t> expected = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, Int16AndBoolAndEnum)
{
  const int16_t i = -2;  // 0xFFFE
  ValuePtr pi(&i);
  ASSERT_EQ(serialize(pi), (std::vector<uint8_t>{ 0xFE, 0xFF }));

  const bool b = true;
  ValuePtr pb(&b);
  ASSERT_EQ(pb.type(), BasicType::BOOL);
  ASSERT_EQ(serialize(pb), (std::vector<uint8_t>{ 0x01 }));

  enum Color : uint8_t { RED = 0, GREEN = 1, BLUE = 2 };
  const Color c = BLUE;
  ValuePtr pc(&c);
  ASSERT_EQ(pc.type(), BasicType::UINT8);
  ASSERT_EQ(serialize(pc), (std::vector<uint8_t>{ 0x02 }));
}

TEST(ValuePtrGolden, VectorOfFloat)
{
  const std::vector<float> v = { 1.0f, 2.0f };  // 0x3F800000, 0x40000000
  ValuePtr ptr(&v);
  ASSERT_EQ(ptr.type(), BasicType::FLOAT32);
  ASSERT_TRUE(ptr.isVector());
  ASSERT_EQ(ptr.vectorSize(), 0);  // dynamic
  const std::vector<uint8_t> expected = { 0x02, 0x00, 0x00, 0x00,  //
                                          0x00, 0x00, 0x80, 0x3F,  //
                                          0x00, 0x00, 0x00, 0x40 };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, ArrayOfUint16)
{
  const std::array<uint16_t, 3> a = { 1, 256, 0xABCD };
  ValuePtr ptr(&a);
  ASSERT_TRUE(ptr.isVector());
  ASSERT_EQ(ptr.vectorSize(), 3);
  // fixed-size arrays carry no length prefix
  const std::vector<uint8_t> expected = { 0x01, 0x00, 0x00, 0x01, 0xCD, 0xAB };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, CustomTypeThroughSerializer)
{
  TypesRegistry registry;
  auto serializer = registry.getSerializer<TestTypes::Point3D>();
  const TestTypes::Point3D p{ 1.5, -0.0, 2.0 };
  ValuePtr ptr(&p, serializer);
  ASSERT_EQ(ptr.type(), BasicType::OTHER);
  const std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F,  // 1.5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,  // -0.0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40   // 2.0
  };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, VectorOfCustomType)
{
  TypesRegistry registry;
  auto serializer = registry.getSerializer<TestTypes::Point3D>();
  const std::vector<TestTypes::Point3D> v = { { 1.5, 0, 0 } };
  ValuePtr ptr(&v, serializer);
  ASSERT_TRUE(ptr.isVector());
  std::vector<uint8_t> expected = { 0x01, 0x00, 0x00, 0x00 };  // count
  const uint8_t one_point[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F, 0, 0, 0, 0,
                                0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0, 0 };
  expected.insert(expected.end(), std::begin(one_point), std::end(one_point));
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrAtomic, ScalarSerializesLikePlainValue)
{
  const std::atomic<double> a{ 1.5 };
  const double d = 1.5;
  ValuePtr pa(&a);
  ValuePtr pd(&d);
  ASSERT_EQ(pa.type(), BasicType::FLOAT64);
  ASSERT_EQ(pa.getSerializedSize(), sizeof(double));
  ASSERT_EQ(serialize(pa), serialize(pd));
  // same schema identity as the plain value
  ASSERT_TRUE(pa == pd);
}

TEST(ValuePtrAtomic, EnumAndInt)
{
  enum Mode : int32_t { A = 7 };
  const std::atomic<Mode> m{ A };
  ValuePtr pm(&m);
  ASSERT_EQ(pm.type(), BasicType::INT32);
  ASSERT_EQ(serialize(pm), (std::vector<uint8_t>{ 0x07, 0x00, 0x00, 0x00 }));

  const std::atomic<uint8_t> u{ 200 };
  ValuePtr pu(&u);
  ASSERT_EQ(serialize(pu), (std::vector<uint8_t>{ 0xC8 }));
}

TEST(ValuePtrAtomic, SeesLatestStore)
{
  std::atomic<int32_t> a{ 1 };
  ValuePtr ptr(&a);
  a.store(42, std::memory_order_relaxed);
  ASSERT_EQ(serialize(ptr), (std::vector<uint8_t>{ 0x2A, 0x00, 0x00, 0x00 }));
}

TEST(ValuePtrAtomic, NumericAtomicResolvesToAtomicConstructor)
{
  // if the generic ctor were chosen for these, the static_assert in it would fire
  const std::atomic<int8_t> a{ 1 };
  const std::atomic<uint64_t> b{ 2 };
  const std::atomic<float> c{ 3.0f };
  ValuePtr pa(&a);
  ValuePtr pb(&b);
  ValuePtr pc(&c);
  ASSERT_EQ(pa.type(), BasicType::INT8);
  ASSERT_EQ(pb.type(), BasicType::UINT64);
  ASSERT_EQ(pc.type(), BasicType::FLOAT32);
}

TEST(ValuePtr, IsMoveOnlyAndSmall)
{
  static_assert(!std::is_copy_constructible_v<ValuePtr>);
  static_assert(std::is_nothrow_move_constructible_v<ValuePtr>);
  // two function pointers + serializer shared_ptr + data pointer + small fields
  static_assert(sizeof(ValuePtr) <= 64, "ValuePtr grew; std::function crept back?");
}

TEST(ValuePtr, DefaultConstructedIsInertNotUB)
{
  ValuePtr empty;
  ASSERT_EQ(empty.getSerializedSize(), 0u);
  uint8_t buffer[8];
  SerializeMe::SpanBytes span(buffer, 8);
  empty.serialize(span);            // must not crash and must not consume bytes
  ASSERT_EQ(span.size(), 8u);
  ASSERT_EQ(empty.type(), BasicType::OTHER);
}

TEST(ValuePtr, DetachSerializesNothingButKeepsTypeIdentity)
{
  const double d = 1.5;
  ValuePtr detached(&d);
  detached.detach();
  ASSERT_EQ(detached.getSerializedSize(), 0u);
  uint8_t buffer[8];
  SerializeMe::SpanBytes span(buffer, 8);
  detached.serialize(span);
  ASSERT_EQ(span.size(), 8u);
  ASSERT_TRUE(detached == ValuePtr(&d));  // re-registration type check still works
  ASSERT_EQ(detached.type(), BasicType::FLOAT64);
}

TEST(ValuePtr, DifferentShapesCompareUnequal)
{
  const double d = 0;
  const float f = 0;
  const std::vector<double> v;
  const std::array<double, 3> a3{};
  const std::array<double, 4> a4{};
  ASSERT_TRUE(ValuePtr(&d) != ValuePtr(&f));
  ASSERT_TRUE(ValuePtr(&d) != ValuePtr(&v));
  ASSERT_TRUE(ValuePtr(&a3) != ValuePtr(&a4));
  ASSERT_TRUE(ValuePtr(&v) != ValuePtr(&a3));
}
