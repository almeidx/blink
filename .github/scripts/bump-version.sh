#!/usr/bin/env bash
# Bumps the release version in CMakeLists.txt, packaging/PKGBUILD and packaging/blink-capture.spec,
# then prints the new version.
#
# Usage: bump-version.sh <patch|minor|major|prerelease> [alpha|beta|rc]
#
# The semver in CMakeLists.txt is the source of truth. A prerelease such as 0.2.0-rc.1 becomes
# pkgver 0.2.0rc.1 (pacman forbids '-'; a letter right after the number sorts before 0.2.0) and
# RPM Version 0.2.0~rc.1 ('~' sorts before 0.2.0).
set -euo pipefail

bump="${1:?usage: bump-version.sh <patch|minor|major|prerelease> [alpha|beta|rc]}"
prerelease_tag="${2:-alpha}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cmake="$root/CMakeLists.txt"
pkgbuild="$root/packaging/PKGBUILD"
spec="$root/packaging/blink-capture.spec"

semver='^([0-9]+)\.([0-9]+)\.([0-9]+)(-(alpha|beta|rc)\.([0-9]+))?$'

current="$(sed -n 's/^set(BLINK_VERSION \(.*\))$/\1/p' "$cmake")"
if [[ ! "$current" =~ $semver ]]; then
  echo "Unsupported current version in CMakeLists.txt: '$current'" >&2
  exit 1
fi
major="${BASH_REMATCH[1]}"
minor="${BASH_REMATCH[2]}"
patch="${BASH_REMATCH[3]}"
current_tag="${BASH_REMATCH[5]}"
current_number="${BASH_REMATCH[6]}"

case "$prerelease_tag" in
  alpha | beta | rc) ;;
  *)
    echo "Unsupported prerelease tag: '$prerelease_tag'" >&2
    exit 1
    ;;
esac

# Like npm version, a major/minor/patch bump of a prerelease releases it when that satisfies the
# bump: 0.1.1-rc.2 becomes 0.1.1 on patch, and 0.2.0-rc.1 becomes 0.2.0 on patch or minor.
case "$bump" in
  major)
    if [ -n "$current_tag" ] && [ "$minor" -eq 0 ] && [ "$patch" -eq 0 ]; then
      version="$major.0.0"
    else
      version="$((major + 1)).0.0"
    fi
    ;;
  minor)
    if [ -n "$current_tag" ] && [ "$patch" -eq 0 ]; then
      version="$major.$minor.0"
    else
      version="$major.$((minor + 1)).0"
    fi
    ;;
  patch)
    if [ -n "$current_tag" ]; then
      version="$major.$minor.$patch"
    else
      version="$major.$minor.$((patch + 1))"
    fi
    ;;
  prerelease)
    if [ -z "$current_tag" ]; then
      version="$major.$minor.$((patch + 1))-$prerelease_tag.1"
    elif [ "$current_tag" = "$prerelease_tag" ]; then
      version="$major.$minor.$patch-$prerelease_tag.$((current_number + 1))"
    else
      version="$major.$minor.$patch-$prerelease_tag.1"
    fi
    ;;
  *)
    echo "Unsupported bump type: '$bump'" >&2
    exit 1
    ;;
esac

pkgver="${version/-/}"
rpm_version="${version/-/\~}"

# Replaces the single line matching an ERE; fails if the line is missing or ambiguous.
replace_line() {
  local file="$1" pattern="$2" replacement="$3" tmp
  tmp="$(mktemp)"
  awk -v pattern="$pattern" -v replacement="$replacement" '
    $0 ~ pattern { print replacement; hits++; next }
    { print }
    END { if (hits != 1) exit 1 }
  ' "$file" > "$tmp" || {
    rm -f "$tmp"
    echo "Expected exactly one line matching '$pattern' in $file" >&2
    exit 1
  }
  cat "$tmp" > "$file"
  rm -f "$tmp"
}

replace_line "$cmake" '^set[(]BLINK_VERSION ' "set(BLINK_VERSION $version)"
replace_line "$pkgbuild" '^pkgver=' "pkgver=$pkgver"
replace_line "$pkgbuild" '^pkgrel=' 'pkgrel=1'
replace_line "$spec" '^Version:' "Version:        $rpm_version"
replace_line "$spec" '^Release:' 'Release:        1%{?dist}'

# Prepend a %changelog entry, reusing the packager of the latest entry.
packager="$(awk '
  found && /^\* / { sub(/^\* [A-Za-z]+ [A-Za-z]+ [0-9]+ [0-9]+ /, ""); sub(/ - [^ ]+$/, ""); print; exit }
  /^%changelog$/ { found = 1 }
' "$spec")"
if [ -z "$packager" ]; then
  echo "Could not find a %changelog entry in $spec" >&2
  exit 1
fi
entry="* $(LC_ALL=C date -u '+%a %b %d %Y') $packager - $rpm_version-1"
tmp="$(mktemp)"
awk -v entry="$entry" -v version="$version" '
  { print }
  /^%changelog$/ { print entry; print "- Update to " version; print "" }
' "$spec" > "$tmp"
cat "$tmp" > "$spec"
rm -f "$tmp"

echo "$version"
