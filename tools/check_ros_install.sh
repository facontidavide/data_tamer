#!/bin/bash
# Checks of a ROS 2 install of data_tamer_v2 that the unit tests cannot make. The ROS 2
# CI jobs run it in the industrial_ci container, after the workspace is built and with
# its install space sourced (AFTER_SCRIPT: rosenv tools/check_ros_install.sh); locally,
# source the install space of a colcon workspace that holds data_tamer_v2 and run it.
#
# 1. data_tamer_cpp/test_package, a consumer that calls find_package(data_tamer 2) and
#    links data_tamer::data_tamer, builds and runs against the install space.
# 2. The install shares no file with the released data_tamer_cpp 1.x package, which bloom
#    installs into the same prefix (/opt/ros/<distro>). The binary package is downloaded,
#    not installed, and its file list compared with the install space. data_tamer_msgs
#    is one package in 1.x and 2.x, released from the 2.x source: it replaces the old
#    files and is not part of the 1.x package, so only data_tamer_cpp is compared.
set -euo pipefail
export LC_ALL=C  # sort and comm must agree on the order

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
distro=${ROS_DISTRO:?source the ROS 2 install space first}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The prefix of an ament package, from the sourced environment.
prefix_of() {
  local dirs p
  IFS=: read -ra dirs <<<"${AMENT_PREFIX_PATH:?}"
  for p in "${dirs[@]}"; do
    if [ -e "$p/share/ament_index/resource_index/packages/$1" ]; then
      echo "$p"
      return
    fi
  done
  echo "error: package $1 is not in AMENT_PREFIX_PATH" >&2
  return 1
}

v2_prefix=$(prefix_of data_tamer_v2)
msgs_prefix=$(prefix_of data_tamer_msgs)

echo "== find_package(data_tamer 2) consumer, against $v2_prefix"
cmake -S "$repo/data_tamer_cpp/test_package" -B "$tmp/consumer"
grep -qx "data_tamer_DIR:PATH=$v2_prefix/share/data_tamer/cmake" "$tmp/consumer/CMakeCache.txt" ||
  { echo "error: find_package(data_tamer) did not find the ROS 2 install"; exit 1; }
cmake --build "$tmp/consumer"
"$tmp/consumer/test_package"

echo "== files shared with the released data_tamer_cpp 1.x package"
# Rolling no longer releases 1.x: nothing can collide there.
if ! apt-cache show "ros-$distro-data-tamer-cpp" >/dev/null 2>&1; then
  echo "ros-$distro-data-tamer-cpp is not in the apt sources: nothing to compare"
  exit 0
fi
(cd "$tmp" && apt-get download "ros-$distro-data-tamer-cpp")
deb=$(ls "$tmp"/ros-*.deb)
# The package lists /opt/ros/<distro>/...: keep the files and symlinks under the prefix.
dpkg-deb --fsys-tarfile "$deb" | tar -t | sed -n "s|^\./opt/ros/$distro/||p" |
  grep -E '[^/]$' | sort > "$tmp/v1"
for prefix in "$v2_prefix" "$msgs_prefix"; do
  (cd "$prefix" && find . \( -type f -o -type l \) | sed 's|^\./||')
done | sort -u > "$tmp/v2"

echo "$(wc -l < "$tmp/v1") files in $(basename "$deb"), $(wc -l < "$tmp/v2") in the install space"
[ -s "$tmp/v1" ] && [ -s "$tmp/v2" ] || { echo "error: empty file list"; exit 1; }
common=$(comm -12 "$tmp/v1" "$tmp/v2")
if [ -n "$common" ]; then
  echo "error: files installed by both data_tamer_cpp 1.x and data_tamer_v2:"
  echo "$common"
  exit 1
fi
echo "no file in common"
