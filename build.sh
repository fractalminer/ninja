#!/bin/bash
set -e

if [[ ! -d build ]]; then
  mkdir build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release
else
  cd build
fi

if [[ -x ninja ]]; then
  cp ninja ninja.old
fi

if [[ -x ninja.old ]]; then
  ./ninja.old clean
  clear
  exec ./ninja.old all
fi

exec ninja all
