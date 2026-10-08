#include "data_tamer/channel.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/custom_types.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

// Return types of an ADL TypeDefinition(): the name may be a view, a C string or an
// owning string, as it may be for DataTamer::TypeDefinitionTrait<T>::define().

namespace names_app
{
struct ViewName
{
  double x = 0;
};
struct CStringName
{
  double x = 0;
};
struct OwnedName
{
  double x = 0;
};

template <typename AddField>
std::string_view TypeDefinition(ViewName& p, AddField& add)
{
  add("x", &p.x);
  return "ViewName";
}
template <typename AddField>
const char* TypeDefinition(CStringName& p, AddField& add)
{
  add("x", &p.x);
  return "CStringName";
}
template <typename AddField>
std::string TypeDefinition(OwnedName& p, AddField& add)
{
  add("x", &p.x);
  return std::string("Owned") + "Name";
}
}  // namespace names_app

TEST(SerializeMeApi, AdlTypeDefinitionMayReturnAnyNameType)
{
  using SerializeMe::has_TypeDefinition;
  EXPECT_TRUE(has_TypeDefinition<names_app::ViewName>::value);
  EXPECT_TRUE(has_TypeDefinition<names_app::CStringName>::value);
  EXPECT_TRUE(has_TypeDefinition<names_app::OwnedName>::value);
}

TEST(SerializeMeApi, ChannelRegistersTypesNamedByAnyAdlReturnType)
{
  auto channel = DataTamer::LogChannel::create("names");
  names_app::ViewName view;
  names_app::CStringName c_string;
  names_app::OwnedName owned;
  channel->registerValue("view", &view);
  channel->registerValue("c_string", &c_string);
  channel->registerValue("owned", &owned);

  const std::string schema = DataTamer::ToStr(channel->getSchema());
  EXPECT_NE(schema.find("ViewName view\n"), std::string::npos) << schema;
  EXPECT_NE(schema.find("CStringName c_string\n"), std::string::npos) << schema;
  EXPECT_NE(schema.find("OwnedName owned\n"), std::string::npos) << schema;
  EXPECT_NE(schema.find("MSG: OwnedName\nfloat64 x\n"), std::string::npos) << schema;
}
