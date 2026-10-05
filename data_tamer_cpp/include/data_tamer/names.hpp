#pragma once

#include <string>
#include <string_view>

namespace DataTamer
{
namespace details
{
// Appends the non-empty '/'-separated components of `part` to `out`.
inline void AppendNameComponents(std::string& out, std::string_view part)
{
  size_t pos = 0;
  while(pos < part.size())
  {
    if(part[pos] == '/')
    {
      ++pos;
      continue;
    }
    size_t end = part.find('/', pos);
    if(end == std::string_view::npos)
    {
      end = part.size();
    }
    if(!out.empty())
    {
      out += '/';
    }
    out.append(part.data() + pos, end - pos);
    pos = end;
  }
}
}  // namespace details

/**
 * @brief Builds a hierarchical value name from its components.
 *
 * Components are joined with a single '/'. Leading, trailing and repeated
 * slashes are collapsed and empty components dropped, so namespaces may be
 * passed with or without a trailing slash:
 *
 *   JoinNames("/loco/", "LF/", "x")  == "loco/LF/x"
 *   JoinNames("loco", "", "torso")   == "loco/torso"
 *   JoinNames("loco//torso/")        == "loco/torso"
 *
 * LogChannel::registerValue() accepts names with empty components (leading,
 * trailing or repeated '/'), but PlotJuggler shows them as empty path
 * elements. The result of this helper is always a canonical name (see
 * IsCanonicalName()), unless it is empty or a component contains a space.
 *
 * @param parts anything convertible to std::string_view (std::string, const char*, ...).
 *              Passing a null `const char*` is undefined behaviour, as for
 *              std::string_view itself.
 */
template <typename... Parts>
[[nodiscard]] std::string JoinNames(const Parts&... parts)
{
  std::string out;
  (details::AppendNameComponents(out, std::string_view(parts)), ...);
  return out;
}

/**
 * @brief True if `name` is a canonical hierarchical name: non-empty, without
 * spaces and without empty '/'-separated components (no leading, trailing or
 * repeated '/').
 *
 * Registration only rejects spaces; this is an opt-in check for code that
 * wants to enforce canonical names, e.g. in a debug build:
 *
 *   assert(DataTamer::IsCanonicalName(name));
 *   channel->registerValue(name, &value);
 *
 *   IsCanonicalName("loco/LF/x")   == true
 *   IsCanonicalName("/loco//LF/x") == false
 */
[[nodiscard]] inline bool IsCanonicalName(std::string_view name)
{
  return !name.empty() && name.find(' ') == std::string_view::npos &&
         name.front() != '/' && name.back() != '/' &&
         name.find("//") == std::string_view::npos;
}

}  // namespace DataTamer
