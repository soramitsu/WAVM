#!/bin/sh

set -e -v

if [ "$CXX" = "g++" ]; then
  export CXXFLAGS="-fuse-ld=gold";
fi

echo $CXX
$CXX --version

cmake --version

if [ "$TRAVIS_OS_NAME" = "osx" ]; then
  export LLVM_URL="http://releases.llvm.org/6.0.0/clang+llvm-6.0.0-x86_64-apple-darwin.tar.xz";
else
  export LLVM_URL="http://releases.llvm.org/6.0.1/clang+llvm-6.0.1-x86_64-linux-gnu-ubuntu-16.04.tar.xz";
fi

# Download a binary build of LLVM6 (also not available in Travis's whitelisted apt sources)
mkdir llvm6
cd llvm6
wget --no-check-certificate --quiet -O ./llvm.tar.xz ${LLVM_URL}
tar --strip-components=1 -xf ./llvm.tar.xz
export LLVM_DIR=`pwd`/lib/cmake/llvm
cd ..

echo $ENABLE_RELEASE
if [ "$ENABLE_RELEASE" = "YES" ]; then
# Build and test a release build of WAVM.
mkdir release
cd release
  cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \
           -DLLVM_DIR=${LLVM_DIR} \
           -DWAVM_ENABLE_RUNTIME=${ENABLE_RUNTIME} \
           -DWAVM_ENABLE_STATIC_LINKING=${ENABLE_STATIC_LINKING} \
           -DWAVM_ENABLE_RELEASE_ASSERTS=${ENABLE_RELEASE_ASSERTS} \
           -DWAVM_ENABLE_ASAN=${ENABLE_ASAN} \
           -DWAVM_ENABLE_UBSAN=${ENABLE_UBSAN} \
           -DWAVM_ENABLE_LIBFUZZER=${ENABLE_LIBFUZZER} \
           -DWAVM_ENABLE_TSAN=${ENABLE_TSAN} \
           -DWAVM_ENABLE_UNWIND=${ENABLE_UNWIND}
  make -j2
  ctest -V -j2
  cd ..
fi

echo $ENABLE_DEBUG
if [ "$ENABLE_DEBUG" = "YES" ]; then
  # Build and test a debug build of WAVM.
  mkdir debug
  cd debug
  cmake .. -DCMAKE_BUILD_TYPE=Debug \
           -DLLVM_DIR=${LLVM_DIR} \
           -DWAVM_ENABLE_RUNTIME=${ENABLE_RUNTIME} \
           -DWAVM_ENABLE_STATIC_LINKING=${ENABLE_STATIC_LINKING} \
           -DWAVM_ENABLE_RELEASE_ASSERTS=${ENABLE_RELEASE_ASSERTS} \
           -DWAVM_ENABLE_ASAN=${ENABLE_ASAN} \
           -DWAVM_ENABLE_UBSAN=${ENABLE_UBSAN} \
           -DWAVM_ENABLE_LIBFUZZER=${ENABLE_LIBFUZZER} \
           -DWAVM_ENABLE_TSAN=${ENABLE_TSAN} \
           -DWAVM_ENABLE_UNWIND=${ENABLE_UNWIND}
  make -j2
  ctest -V -j2
  cd ..
fi