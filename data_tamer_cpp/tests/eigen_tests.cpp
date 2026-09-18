#include "data_tamer_parser/data_tamer_parser.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <thread>
#include <variant>

#if DATA_TAMER_EIGEN_SUPPORT

#include <Eigen/Core>

namespace
{
DataTamerParser::SnapshotView ConvertSnapshot(const DataTamer::Snapshot& snapshot)
{
  return { snapshot.schema_hash,
           uint64_t(snapshot.timestamp.count()),
           { snapshot.active_mask.data(), snapshot.active_mask.size() },
           { snapshot.payload.data(), snapshot.payload.size() } };
}

std::map<std::string, double> ParseLatest(const DataTamer::LogChannel& channel,
                                          const DataTamer::DummySink& sink)
{
  const auto& schema_out =
      DataTamerParser::BuilSchemaFromText(ToStr(channel.getSchema()));
  const auto snapshot_view = ConvertSnapshot(sink.latest_snapshot);

  std::map<std::string, double> parsed;
  auto callback = [&](const std::string& name, const DataTamerParser::VarNumber& number) {
    parsed[name] = std::visit([](const auto& var) { return double(var); }, number);
  };
  DataTamerParser::ParseSnapshot(schema_out, snapshot_view, callback);
  return parsed;
}
}  // namespace

TEST(DataTamerEigen, SchemaFixedAndDynamic)
{
  auto channel = DataTamer::LogChannel::create("eigen_schema");

  Eigen::Vector3d fixed_vec = { 1, 2, 3 };
  Eigen::VectorXd dynamic_vec(2);
  Eigen::Array4f fixed_arr = { 1, 2, 3, 4 };
  Eigen::ArrayXi dynamic_arr(5);
  Eigen::RowVector2d row_vec = { 7, 8 };

  channel->registerValue("fixed_vec", &fixed_vec);
  channel->registerValue("dynamic_vec", &dynamic_vec);
  channel->registerValue("fixed_arr", &fixed_arr);
  channel->registerValue("dynamic_arr", &dynamic_arr);
  channel->registerValue("row_vec", &row_vec);

  const auto schema = DataTamerParser::BuilSchemaFromText(ToStr(channel->getSchema()));
  ASSERT_EQ(schema.fields.size(), 5);

  // A fixed-size Eigen type must be "type[N]", exactly like std::array,
  // and a dynamic one "type[]", exactly like std::vector.
  using DataTamerParser::BasicType;
  using DataTamerParser::TypeField;

  ASSERT_EQ(schema.fields[0],
            (TypeField{ "fixed_vec", BasicType::FLOAT64, "float64", true, 3 }));
  ASSERT_EQ(schema.fields[1],
            (TypeField{ "dynamic_vec", BasicType::FLOAT64, "float64", true, 0 }));
  ASSERT_EQ(schema.fields[2],
            (TypeField{ "fixed_arr", BasicType::FLOAT32, "float32", true, 4 }));
  ASSERT_EQ(schema.fields[3],
            (TypeField{ "dynamic_arr", BasicType::INT32, "int32", true, 0 }));
  ASSERT_EQ(schema.fields[4],
            (TypeField{ "row_vec", BasicType::FLOAT64, "float64", true, 2 }));
}

TEST(DataTamerEigen, RoundTrip)
{
  DataTamer::ChannelsRegistry registry;
  auto channel = registry.getChannel("eigen_roundtrip");
  auto sink = std::make_shared<DataTamer::DummySink>();
  channel->addDataSink(sink);

  Eigen::Vector3d fixed_vec = { 10, 11, 12 };
  Eigen::VectorXd dynamic_vec(2);
  dynamic_vec << 20, 21;
  Eigen::Array3f arr = { 30, 31, 32 };

  channel->registerValue("fixed_vec", &fixed_vec);
  channel->registerValue("dynamic_vec", &dynamic_vec);
  channel->registerValue("arr", &arr);

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  const auto parsed = ParseLatest(*channel, *sink);

  ASSERT_EQ(parsed.at("fixed_vec[0]"), 10);
  ASSERT_EQ(parsed.at("fixed_vec[1]"), 11);
  ASSERT_EQ(parsed.at("fixed_vec[2]"), 12);

  ASSERT_EQ(parsed.at("dynamic_vec[0]"), 20);
  ASSERT_EQ(parsed.at("dynamic_vec[1]"), 21);

  ASSERT_EQ(parsed.at("arr[0]"), 30);
  ASSERT_EQ(parsed.at("arr[1]"), 31);
  ASSERT_EQ(parsed.at("arr[2]"), 32);
}

TEST(DataTamerEigen, DynamicVectorChangingSize)
{
  DataTamer::ChannelsRegistry registry;
  auto channel = registry.getChannel("eigen_resize");
  auto sink = std::make_shared<DataTamer::DummySink>();
  channel->addDataSink(sink);

  Eigen::VectorXd vec(2);
  vec << 1, 2;
  channel->registerValue("vec", &vec);

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  {
    const auto parsed = ParseLatest(*channel, *sink);
    ASSERT_EQ(parsed.size(), 2);
    ASSERT_EQ(parsed.at("vec[1]"), 2);
  }

  // The channel re-reads data() and size() at every snapshot, so a resize
  // must be picked up without re-registering.
  vec.resize(4);
  vec << 5, 6, 7, 8;

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  {
    const auto parsed = ParseLatest(*channel, *sink);
    ASSERT_EQ(parsed.size(), 4);
    ASSERT_EQ(parsed.at("vec[0]"), 5);
    ASSERT_EQ(parsed.at("vec[3]"), 8);
  }
}

TEST(DataTamerEigen, MatchesEquivalentStlContainer)
{
  DataTamer::ChannelsRegistry registry;
  auto channel = registry.getChannel("eigen_vs_stl");
  auto sink = std::make_shared<DataTamer::DummySink>();
  channel->addDataSink(sink);

  std::array<double, 3> stl_fixed = { 1, 2, 3 };
  Eigen::Vector3d eigen_fixed = { 1, 2, 3 };
  std::vector<double> stl_dynamic = { 4, 5 };
  Eigen::VectorXd eigen_dynamic(2);
  eigen_dynamic << 4, 5;

  channel->registerValue("stl_fixed", &stl_fixed);
  channel->registerValue("eigen_fixed", &eigen_fixed);
  channel->registerValue("stl_dynamic", &stl_dynamic);
  channel->registerValue("eigen_dynamic", &eigen_dynamic);

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  const auto parsed = ParseLatest(*channel, *sink);
  for(int i = 0; i < 3; i++)
  {
    const auto idx = "[" + std::to_string(i) + "]";
    ASSERT_EQ(parsed.at("stl_fixed" + idx), parsed.at("eigen_fixed" + idx));
  }
  for(int i = 0; i < 2; i++)
  {
    const auto idx = "[" + std::to_string(i) + "]";
    ASSERT_EQ(parsed.at("stl_dynamic" + idx), parsed.at("eigen_dynamic" + idx));
  }
}

// Compile-time boundary of the feature. Registering a view, an expression or a
// matrix must fail to compile; these checks assert the traits that reject them.
TEST(DataTamerEigen, UnsupportedTypesAreRejected)
{
  using DataTamer::IsEigenPlainObject;
  using DataTamer::IsEigenType;

  Eigen::MatrixXd m(3, 4);
  using Block = decltype(m.col(0));
  using Expr = decltype(m + m);
  using Map = Eigen::Map<Eigen::VectorXd>;

  static_assert(IsEigenType<Eigen::VectorXd> && IsEigenPlainObject<Eigen::VectorXd>);
  static_assert(IsEigenType<Eigen::ArrayXf> && IsEigenPlainObject<Eigen::ArrayXf>);

  static_assert(IsEigenType<Block> && !IsEigenPlainObject<Block>);
  static_assert(IsEigenType<Expr> && !IsEigenPlainObject<Expr>);
  static_assert(IsEigenType<Map> && !IsEigenPlainObject<Map>);

  // Matrices are plain objects, but rejected by ValuePtr because flattening
  // them would lose the shape.
  static_assert(IsEigenPlainObject<Eigen::MatrixXd>);
  static_assert(!Eigen::MatrixXd::IsVectorAtCompileTime);

  // Non-Eigen types are untouched by the Eigen overloads.
  static_assert(!IsEigenType<double>);
  static_assert(!IsEigenType<std::vector<double>>);
}

#endif  // DATA_TAMER_EIGEN_SUPPORT
