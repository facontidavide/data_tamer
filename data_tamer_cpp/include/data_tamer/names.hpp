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
 * @brief Builds a hierarchical value name for LogChannel::registerValue(): joins the
 * components with '/', dropping empty components and leading, trailing and repeated
 * slashes.
 *
 *   JoinNames("/loco/", "LF/", "x")  == "loco/LF/x"
 *   JoinNames("loco", "", "torso")   == "loco/torso"
 *   JoinNames("loco//torso/")        == "loco/torso"
 *
 * The result is canonical (see IsCanonicalName()) unless it is empty or a component
 * contains a space.
 *
 * @param parts anything convertible to std::string_view (std::string, const char*,
 *              ...). A null `const char*` is undefined behaviour, as for
 *              std::string_view.
 */
template <typename... Parts>
[[nodiscard]] std::string JoinNames(const Parts&... parts)
{
  std::string out;
  (details::AppendNameComponents(out, std::string_view(parts)), ...);
  return out;
}

/**
 * @brief True if `name` is non-empty, has no spaces and no empty '/'-separated
 * component (no leading, trailing or repeated '/'). Registration only rejects
 * spaces: use this to assert on names, e.g. in a debug build.
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
