#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
configuration=${1:?Usage: Scripts/package.sh Debug|Release [version]}
case "$configuration" in
  Debug) variant=DEBUG ;;
  Release) variant=RELEASE ;;
  *) echo "Expected Debug or Release" >&2; exit 1 ;;
esac

settings=(ARCHS=x86_64 ONLY_ACTIVE_ARCH=NO "SYMROOT=$PWD/build")
if [[ $# -gt 1 ]]; then
  if [[ ! $2 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Expected a numeric version such as 1.4.2" >&2
    exit 1
  fi
  settings+=("MODULE_VERSION=$2" "CURRENT_PROJECT_VERSION=$2")
fi

xcodebuild -project AsusSMC.xcodeproj -target AsusSMC \
  -configuration "$configuration" "${settings[@]}" build
xcrun clang -std=c11 -Wall -Wextra -arch x86_64 -mmacosx-version-min=10.11 \
  Tools/fanctl.c -framework IOKit -framework CoreFoundation \
  -o "build/$configuration/fanctl"

cd "build/$configuration"
version=$(plutil -extract CFBundleVersion raw AsusSMC.kext/Contents/Info.plist)
[[ $version == "${2-$version}" ]]
codesign --verify --strict AsusSMC.kext
for binary in AsusSMC.kext/Contents/MacOS/AsusSMC AsusSMCDaemon fanctl; do
  lipo "$binary" -verify_arch x86_64
done

mkdir -p ../packages
archive="../packages/AsusSMC-$version-$variant.zip"
files=(AsusSMC.kext AsusSMCDaemon com.hieplpvip.AsusSMCDaemon.plist install_daemon.sh fanctl)
for symbols in AsusSMC.kext.dSYM AsusSMCDaemon.dSYM; do
  if [[ -d $symbols ]]; then files+=("$symbols"); fi
done
zip -qry -MM -FS "$archive" "${files[@]}"
unzip -tq "$archive"
