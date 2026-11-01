#!/bin/sh
# Clone the upstream projects this port builds on into refs/, at pinned commits. refs/ is read-only reference
# material and ignored by git.
set -eu

cd "$(dirname "$0")/.."
mkdir -p refs

fetch() {
    name=$1 url=$2 commit=$3
    dir=refs/$name
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
        echo "$name: up to date"
        return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
    echo "$name: $commit"
}

fetch LASRecomp      https://github.com/peiikai/LASRecomp.git        da35b83a9611d046a9a681aae668d7085954d37f
fetch MacNativeGames https://github.com/ly05010419/MacNativeGames.git b75be8cfa704df3701e86130c8ced268e79d6b11
fetch d2r-mobile     https://github.com/jhaizhou-ops/d2r-mobile.git  53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53
fetch nxnative       https://github.com/seedds/nxnative.git          d5b380b3a80495609c2e751bb994911a638935ad
fetch SwitchRecomp   https://github.com/bgyss/SwitchRecomp.git       0f99e1c5c1e78fdc8e01e58a0f8dc8622e82976b
fetch deko3d           https://github.com/devkitPro/deko3d.git         373bfd43d8b2d805fa2eb7d406dbb823032df958
fetch SavageWindRecomp https://github.com/Revivedx/SavageWindRecomp.git 575ad88e02f9a89a9a885d90039d48af33c982f4
