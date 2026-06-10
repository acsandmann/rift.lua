#!/bin/sh

pkg_config=$1
target_arch=$2
shift 2

command -v "$pkg_config" >/dev/null 2>&1 || exit 0

for pc in "$@"; do
  "$pkg_config" --exists "$pc" || continue

  compatible=1
  lib_dirs=$($pkg_config --libs-only-L "$pc" | sed 's/-L//g')
  lib_names=$($pkg_config --libs-only-l "$pc" | sed 's/-l//g')

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

  [ "$compatible" -eq 1 ] && printf '%s' "$pc" && exit 0
done
