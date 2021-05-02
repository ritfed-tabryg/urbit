# ensure required mingw packages are installed
mpkgs=(cmake curl gcc jq libsigsegv libuv make wslay)
pacman -S --needed autoconf automake-wrapper libtool patch ${mpkgs[@]/#/mingw-w64-x86_64-}

declare -a cdirs
declare -a ldirs
sources=(../../nix/sources.json ../../nix/sources-mingw.json)

hex2nixbase32 () {
  local digits='0123456789abcdfghijklmnpqrsvwxyz'
  local bits=0
  local left=0 # number of bits left in $bits
  local i=0
  while ((1))
  do
    while ((left>=5))
    do
      echo -n ${digits:$((bits&31)):1}
      bits=$((bits>>5))
      left=$((left-5))
    done
    if ((i == ${#1}))
    then
      break
    fi
    char=0x${1:i:2}
    i=$((i+2))
    bits=$((bits|(char<<(left))))
    left=$((left+8))
  done
  echo -n ${digits:$bits:1}
}

buildnixdep () {
  local cache=https://app.cachix.org/api/v1/cache/${CACHIX_CACHE-}
  local deriver=urbit-mingw-build
  local hash=
  local dir
  if [ -n "$url" ]
  then
    local patch=compat/mingw/$key.patch
    # create 'store hash' from sources.json data and patch
    read hash _ < <((
    jq -Sscj --arg key "$key" --arg deriver "$deriver" 'add|to_entries|.[]|select(.key==$key)|{($deriver):.value}' ${sources[@]}
    [ -e $patch ] && cat $patch)|sha256sum)
    hash=$(hex2nixbase32 $hash)
    dir=../$hash-$key
    if [ -e $dir/.mingw~ ]
    then
      # dependency present, don't reupload
      hash=
    else
      # dependency absent, check the binary cache if configured
      if [ -n "${CACHIX_CACHE-}" ]
      then
        echo Checking binary cache for $hash-$key...
        narinfo="$cache/${hash}.narinfo"
        if curl -fLI "$narinfo"
        then
          url="$cache/$(curl -fL -H "Accept: application/json" "$narinfo"|jq -r '.url')"
          echo Found $url
          strip=0
          hash=
        fi
      fi
      mkdir -p $dir
      pushd $dir
      curl -fL "$url"|(tar --strip $strip -xzf - || true)
      popd
    fi
  else
    # local dependency
    dir=../$key
  fi
  # patch and build the dependency if necessary
  # and append dependency paths to -I and -L arrays
  . <(jq -sr --arg key "$key" --arg dir "$dir" 'add|to_entries|.[]|select(.key==$key)|"
pushd \($dir)
if [ ! -e .mingw~ ]
then" + ("../urbit/compat/mingw/\($key).patch"|"
  [ -e \(.) ] && patch -p 1 <\(.)") + "
  \(.value.mingw.prepare//"")
  make \(.value.mingw.make//"")
  touch .mingw~
fi
popd
\(.value.mingw.include//"."|if type == "array" then . else [.] end|map("cdirs+=(-I\($dir)/\(.))")|join("\n"))
\(.value.mingw.lib//"."|if type == "array" then . else [.] end|map("ldirs+=(-L\($dir)/\(.))")|join("\n"))"' ${sources[@]})

  # if configured, upload freshly built dependency to binary cache
  if [ -n "$hash" -a -n "${CACHIX_AUTH_TOKEN-}" ]
  then
    (
    echo Uploading freshly built $hash-$key to binary cache...
    tar -C $dir -czf $hash.tar .
    local size=$(stat -c '%s' $hash.tar)
    read filehash _ < <(sha256sum $hash.tar)
    curl -fL -H "Content-Type: application/gzip" -H "Authorization: Bearer $CACHIX_AUTH_TOKEN" --data-binary @"$hash.tar" "$cache/nar"
    curl -fL -H "Content-Type: application/json" -H "Authorization: Bearer $CACHIX_AUTH_TOKEN" --data-binary @- "$cache/${hash}.narinfo" <<EOF
{
  "cStoreHash": "$hash",
  "cStoreSuffix": "$key",
  "cNarHash": "sha256:$(hex2nixbase32 $filehash)",
  "cNarSize": $size,
  "cFileHash": "$filehash",
  "cFileSize": $size,
  "cReferences": [],
  "cDeriver": "$deriver"
}
EOF
    echo Done. ) || true
    rm $hash.tar || true
  fi
}

# I have to go over the sources files several times
# because jq does not have a way to invoke external programs
. <(jq -sr 'add|to_entries|.[]|select(.value.mingw)|"key=\(.key|@sh) url=\(.value.url//""|@sh) strip=\(.value.mingw.strip+1) buildnixdep"' ${sources[@]})
