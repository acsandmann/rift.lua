#!/bin/sh

pkg_config=$1
target_arch=$2
lua_bin=$3
shift 3

command -v "$lua_bin" >/dev/null 2>&1 || exit 0

lua_path=$(command -v "$lua_bin")
lua_version=$(
  "$lua_bin" -e 'local v = _VERSION:match("(%d+%.%d+)"); if v then print(v) end' 2>/dev/null
)

[ -n "$lua_version" ] || exit 0

header_matches_version() {
  header=$1
  major=${lua_version%.*}
  minor=${lua_version#*.}

  [ -f "$header" ] || return 1

  if grep -q "#define LUA_VERSION_MAJOR[[:space:]]*\"$major\"" "$header" && \
     grep -q "#define LUA_VERSION_MINOR[[:space:]]*\"$minor\"" "$header"; then
    return 0
  fi

  if grep -q "#define LUA_VERSION_MAJOR_N[[:space:]]*$major" "$header" && \
     grep -q "#define LUA_VERSION_MINOR_N[[:space:]]*$minor" "$header"; then
    return 0
  fi

  return 1
}

pc_is_compatible() {
  pc=$1

  command -v "$pkg_config" >/dev/null 2>&1 || return 1
  "$pkg_config" --exists "$pc" || return 1

  pc_version=$("$pkg_config" --modversion "$pc" 2>/dev/null)
  case "$pc_version" in
    "$lua_version"|"$lua_version".*) ;;
    *) return 1 ;;
  esac

  compatible=1
  lib_dirs=$("$pkg_config" --libs-only-L "$pc" | sed 's/-L//g')
  lib_names=$("$pkg_config" --libs-only-l "$pc" | sed 's/-l//g')

  for lib_name in $lib_names; do
    case "$lib_name" in
      lua*) ;;
      *) continue ;;
    esac

    for lib_dir in $lib_dirs /opt/homebrew/lib /usr/local/lib; do
      for ext in dylib a; do
        lib_path="$lib_dir/lib$lib_name.$ext"
        [ -f "$lib_path" ] || continue

        lipo -archs "$lib_path" 2>/dev/null | grep -qw "$target_arch" || compatible=0
        break 2
      done
    done
  done

  [ "$compatible" -eq 1 ] || return 1
  return 0
}

for pc in "$@"; do
  if pc_is_compatible "$pc"; then
    "$pkg_config" --cflags "$pc"
    exit 0
  fi
done

case "$lua_path" in
  */bin/*) lua_prefix=${lua_path%/bin/*} ;;
  *) lua_prefix= ;;
esac

for include_dir in \
  "$lua_prefix/include/lua$lua_version" \
  "$lua_prefix/include/lua" \
  "$lua_prefix/opt/lua/include/lua$lua_version" \
  "$lua_prefix/opt/lua/include/lua" \
  "/opt/homebrew/include/lua$lua_version" \
  "/opt/homebrew/include/lua" \
  "/opt/homebrew/opt/lua/include/lua$lua_version" \
  "/opt/homebrew/opt/lua/include/lua" \
  "/usr/local/include/lua$lua_version" \
  "/usr/local/include/lua"
do
  [ -n "$include_dir" ] || continue
  if header_matches_version "$include_dir/lua.h"; then
    printf '%s\n' "-I$include_dir"
    exit 0
  fi
done

exit 0
