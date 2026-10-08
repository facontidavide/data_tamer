#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace DataTamer
{
namespace details
{
// Whitespace and control characters cannot be part of a name: the schema is text with
// one field per line. Bytes above 0x7f (UTF-8) are allowed.
inline bool IsForbiddenNameByte(unsigned char byte)
{
  return byte <= 0x20 || byte == 0x7F;
}

// Index of the first byte of `name` that IsForbiddenNameByte() rejects, or npos. With
// `allow_space` a space passes: a channel name fills its header line and can hold one.
inline size_t FindForbiddenNameByte(std::string_view name, bool allow_space = false)
{
  for(size_t i = 0; i < name.size(); ++i)
  {
    const auto byte = static_cast<unsigned char>(name[i]);
    if(IsForbiddenNameByte(byte) && !(allow_space && byte == ' '))
    {
      return i;
    }
  }
  return std::string_view::npos;
}

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
 * @brief True if `name` is non-empty, has no whitespace or control character and no
 * empty '/'-separated component (no leading, trailing or repeated '/'). Registration
 * rejects the first two kinds only: use this to assert on the last, e.g. in a debug
 * build.
 *
 *   IsCanonicalName("loco/LF/x")   == true
 *   IsCanonicalName("/loco//LF/x") == false
 */
[[nodiscard]] inline bool IsCanonicalName(std::string_view name)
{
  return !name.empty() &&
         details::FindForbiddenNameByte(name) == std::string_view::npos &&
         name.front() != '/' && name.back() != '/' &&
         name.find("//") == std::string_view::npos;
}

}  // namespace DataTamer
