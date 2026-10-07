#!/usr/bin/env bash
# Compare the ABI of libdata_tamer.so built from this checkout against a baseline
# build, with libabigail (abidiff). Fails on an incompatible change.
#
#   tools/abi_check.sh                         # baseline chosen automatically
#   tools/abi_check.sh --baseline-ref 2.0.0    # any commit-ish, tag or branch
#   tools/abi_check.sh --baseline-dir DIR      # an exported source tree, no git needed
#   tools/abi_check.sh --allow-break           # report, but do not fail
#
# Baseline when none is given: the highest release tag with the same major version
# as this checkout (project VERSION in data_tamer_cpp/CMakeLists.txt); if no such
# tag exists (2.0.0 is not tagged yet), the merge base of HEAD with the base
# branch ($ABI_BASE_BRANCH, default origin/V2). See "ABI policy" in CLAUDE.md.
#
# A second comparison covers the consumer side: tools/abi_probe.cpp (derived
# interfaces, instantiated templates) is compiled against the baseline headers and
# against the current ones. If a change to the probe itself does not compile against
# the baseline headers, the baseline's own probe is used for both sides instead.
#
# Both sides are built as plain CMake (no ROS), shared, Debug, tests off: the
# non-ROS library holds the whole public surface except the ROS 2 sink, whose
# ABI also depends on the rclcpp release. libabigail needs DWARF, hence Debug.
#
# Exit status: 0 no incompatible change (or the SONAME differs, or --allow-break),
# 1 incompatible change, 2 usage or tool error.
#
# A baseline equal to HEAD (a push to the base branch itself) is replaced by HEAD^1.
#
# Needs: cmake, a C++20 compiler, git (unless --baseline-dir), libabigail
# (apt install abigail-tools: abidiff), libzstd-dev and liblz4-dev (bundled MCAP).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
baseline_ref=""
baseline_dir=""
allow_break="${ABI_ALLOW_BREAK:-0}"
work=""

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --baseline-ref|--baseline-dir|--work)
      [[ $# -ge 2 ]] || { echo "$1 needs a value" >&2; usage >&2; exit 2; }
      case "$1" in
        --baseline-ref) baseline_ref="$2" ;;
        --baseline-dir) baseline_dir="$2" ;;
        --work) work="$2" ;;
      esac
      shift 2 ;;
    --allow-break) allow_break=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

for tool in cmake abidiff readelf nm; do
  command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done

project_version() {  # $1: source tree
  sed -n 's/^[[:space:]]*project(data_tamer_cpp[[:space:]]*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' \
    "$1/data_tamer_cpp/CMakeLists.txt" | head -n1
}

current_version="$(project_version "$repo")"
current_major="${current_version%%.*}"

if [[ -z "$work" ]]; then
  work="$(mktemp -d "${TMPDIR:-/tmp}/data_tamer_abi.XXXXXX")"
  trap 'rm -rf "$work"' EXIT
fi
mkdir -p "$work"

released_tag() {  # highest release tag with this major version, if any
  git -C "$repo" tag --list '[0-9]*' --sort=-v:refname 2>/dev/null |
    awk -F. -v major="$current_major" '$1 == major { print; exit }' || true
}

# ---- choose the baseline ------------------------------------------------------
if [[ -z "$baseline_dir" ]]; then
  if [[ -z "$baseline_ref" ]]; then
    baseline_ref="$(released_tag)"
    if [[ -n "$baseline_ref" ]]; then
      echo "baseline: release tag $baseline_ref (same major version as $current_version)"
    else
      base_branch="${ABI_BASE_BRANCH:-origin/V2}"
      baseline_ref="$(git -C "$repo" merge-base HEAD "$base_branch")" || {
        echo "no release tag with major $current_major and no merge base with" \
             "$base_branch: pass --baseline-ref or --baseline-dir" >&2
        exit 2
      }
      echo "baseline: merge base with $base_branch ($baseline_ref), no $current_major.x tag yet"
    fi
  else
    echo "baseline: $baseline_ref"
  fi
  if [[ "$(git -C "$repo" rev-parse "$baseline_ref^{commit}")" == "$(git -C "$repo" rev-parse HEAD)" ]]; then
    echo "baseline is HEAD itself: using HEAD^1"
    baseline_ref="HEAD^1"
  fi
  baseline_dir="$work/baseline_src"
  mkdir -p "$baseline_dir"
  git -C "$repo" archive "$baseline_ref" | tar -x -C "$baseline_dir"
else
  echo "baseline: directory $baseline_dir"
fi

# ---- build both sides ------------------------------------------------------------
build() {  # $1: source tree, $2: build dir, $3: extra compiler flags
  # -w for the baseline: its sources build with -Werror, and a newer compiler on
  # the runner must not fail the check on a warning in code that already merged.
  cmake -S "$1/data_tamer_cpp" -B "$2" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="${3:-}" \
    -DBUILD_SHARED_LIBS=ON -DDATA_TAMER_BUILD_ROS=OFF -DDATA_TAMER_BUILD_TESTS=OFF \
    -DDATA_TAMER_BUILD_EXAMPLES=OFF -DDATA_TAMER_BUILD_BENCHMARKS=OFF >"$2.log" 2>&1 ||
    { tail -n 30 "$2.log" >&2; exit 2; }
  cmake --build "$2" --target data_tamer -j"$(nproc)" >>"$2.log" 2>&1 ||
    { tail -n 30 "$2.log" >&2; exit 2; }
}
echo "building baseline and current library ..."
build "$baseline_dir" "$work/build_baseline" -w
build "$repo" "$work/build_current"

library() {  # $1: build dir; prints the real file, not a symlink
  local file
  file="$(find "$1" -maxdepth 1 -name 'libdata_tamer.so*' -type f | head -n1)"
  [[ -n "$file" ]] || { echo "no libdata_tamer.so* in $1" >&2; exit 2; }
  echo "$file"
}
soname() { readelf -d "$1" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p'; }

lib_old="$(library "$work/build_baseline")"
lib_new="$(library "$work/build_current")"
soname_old="$(soname "$lib_old")"
soname_new="$(soname "$lib_new")"
echo "SONAME: baseline $soname_old, current $soname_new"

# ---- compare ---------------------------------------------------------------------------
# The public surface is what the installed headers declare, plus the MCAP headers
# that sinks/mcap_encoding.hpp includes (abidiff takes one header directory per
# side, so they are merged into one). Types defined elsewhere (the Pimpl structs,
# libstdc++) are private to abidiff.
merged_headers() {  # $1: source tree, $2: destination
  mkdir -p "$2"
  cp -r "$1/data_tamer_cpp/include/." "$2"
  [[ -d "$1/data_tamer_cpp/3rdparty/mcap/include" ]] &&
    cp -r "$1/data_tamer_cpp/3rdparty/mcap/include/." "$2"
  return 0
}
merged_headers "$baseline_dir" "$work/headers_baseline"
merged_headers "$repo" "$work/headers_current"

# Symbols the baseline library defines strongly (functions: T; data, vtables and
# typeinfo of classes with a key function: D, B, R). The library also exports a weak
# copy (W, V, u) of every inline or template function its .cpp files call out of
# line (std:: containers, SerializeMe::Span, ...): consumers never use those, each
# binary emits its own, so a refactor that stops calling one is not an ABI break.
nm -D --defined-only "$lib_old" | awk '$2 !~ /^[WwVvu]$/ { print $3 }' | sort -u >"$work/strong_symbols.txt"

# abidiff status bits: 1 tool error, 2 usage error, 4 ABI change, 8 incompatible change.
# Bit 8 also stands for the removal of those weak copies, so it is not used. A
# changed layout, e.g. a field added to an options struct that is passed by value,
# comes out as "Changed" with status 4, which also stands for harmless additions. The
# policy freezes layouts too, so this counts the functions and variables abidiff
# reports as changed and, for the library, the removed ones that were strong
# symbols of the baseline (including vtable and typeinfo symbols that have no debug
# info, listed as "[D] _ZTV...").
broken=0
compare() {  # $1: label, $2: old file, $3: new file, $4: also count removed symbols (1/0)
  local out="$work/abidiff_$1.txt" rc
  echo "---- abidiff: $1"
  set +e
  abidiff --drop-private-types --fail-no-debug-info --ignore-soname \
    --headers-dir1 "$work/headers_baseline" --headers-dir2 "$work/headers_current" \
    "$2" "$3" | tee "$out"
  rc=${PIPESTATUS[0]}
  set -e
  if (( rc & 3 )); then
    echo "abidiff failed on $1 (status $rc)" >&2
    exit 2
  fi
  local changed removed=0
  # "Functions changes summary: 0 Removed (3 filtered out), 2 Changed (6 filtered out), ..."
  changed=$(awk '/^(Functions|Variables) changes summary:/ {
                   if (match($0, /[0-9]+ Changed/)) n += substr($0, RSTART, RLENGTH - 8)
                 } END { print n + 0 }' "$out")
  if (( $4 )); then
    local symbol
    while read -r symbol; do
      if grep -qxF -- "$symbol" "$work/strong_symbols.txt"; then
        echo "removed exported symbol: $symbol"
        removed=$((removed + 1))
      fi
    done < <(sed -n 's/^[[:space:]]*\[D\] .*{\(.*\)}[[:space:]]*$/\1/p; s/^[[:space:]]*\[D\] \([^ '"'"']*\)$/\1/p' "$out")
  fi
  broken=$((broken + changed + removed))
}

compare library "$lib_old" "$lib_new" 1

# The consumer side: what is compiled into user binaries (tools/abi_probe.cpp).
# A probe that no longer compiles against the current headers is a source break.
probe() {  # $1: source file, $2: source tree whose headers to use, $3: output
  ${CXX:-c++} -std=c++17 -shared -fPIC -g -O0 \
    -I "$2/data_tamer_cpp/include" -I "$2/data_tamer_cpp/3rdparty/mcap/include" \
    "$1" -o "$3" 2>"$3.log"
}
probe_file="$here/abi_probe.cpp"
baseline_probe="$baseline_dir/tools/abi_probe.cpp"
consumer=""
if ! probe "$probe_file" "$repo" "$work/probe_current.so"; then
  cat "$work/probe_current.so.log" >&2
  echo "tools/abi_probe.cpp does not compile against the current headers:" \
       "consumer code written for the previous API breaks." >&2
  broken=$((broken + 1))
elif probe "$probe_file" "$baseline_dir" "$work/probe_baseline.so"; then
  consumer="the current probe"
elif [[ -f "$baseline_probe" ]] &&
     probe "$baseline_probe" "$baseline_dir" "$work/probe_baseline.so"; then
  # The probe changed in this checkout and uses API the baseline lacks: compare what
  # the baseline's own probe built against the baseline and the current headers.
  if probe "$baseline_probe" "$repo" "$work/probe_current.so"; then
    consumer="the baseline's probe (the current one uses new API)"
  else
    cat "$work/probe_current.so.log" >&2
    echo "The baseline's tools/abi_probe.cpp does not compile against the current" \
         "headers: consumer code written for the previous API breaks." >&2
    broken=$((broken + 1))
  fi
else
  echo "::warning::consumer-side ABI comparison skipped: no probe compiles against" \
       "the baseline headers (a baseline that predates tools/abi_probe.cpp)"
fi
if [[ -n "$consumer" ]]; then
  echo "consumer side compared with $consumer"
  compare consumer-side "$work/probe_baseline.so" "$work/probe_current.so" 0
fi

if (( broken > 0 )); then
  if [[ "$soname_old" != "$soname_new" ]]; then
    echo "Incompatible ABI change, announced by the SONAME change" \
         "($soname_old -> $soname_new): accepted."
    exit 0
  fi
  if [[ "$allow_break" == 1 ]]; then
    if [[ -z "$(released_tag)" ]]; then
      echo "Incompatible ABI change accepted by --allow-break / ABI_ALLOW_BREAK" \
           "(no $current_major.x release exists yet)."
      exit 0
    fi
    echo "--allow-break is not accepted once a $current_major.x release exists." >&2
  fi
  cat >&2 <<MSG

Incompatible ABI change without a SONAME change. Either undo it, or, if the
break is intended, bump the major version in data_tamer_cpp/CMakeLists.txt,
data_tamer_cpp/package.xml, data_tamer_msgs/package.xml,
data_tamer_cpp/conanfile.py and python/data_tamer_parser.py (tools/check_versions.py
lists them) and record it in CHANGELOG.rst. Before the first 2.0.0 tag, a layout
change to a pinned struct or class in the PR that owns it may instead be accepted with
the 'abi-break' label or --allow-break (not accepted once a 2.x tag exists, and never
for the DataSink and CustomSerializer vtables, which are frozen already). See
"Versioning and ABI policy" in CLAUDE.md.
MSG
  exit 1
fi
echo "No incompatible ABI change."
