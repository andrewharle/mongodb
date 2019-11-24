#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Please supply an arch: x86_64, i386, etc and a platform: osx, linux, windows, etc"
    exit 0;
fi

_Path=platform/$1/$2
shift
shift

_CONFIG_OPTS=""

_xcode_setup() {
    local sdk=$1; shift
    local arch=$1; shift
    local target=$1; shift
    export SDKROOT=`xcrun --sdk $sdk --show-sdk-path`
    export HOST_CC=/usr/bin/gcc
    export HOST_CXX=/usr/bin/c++
    export CC=`xcrun -f clang`" -arch $arch -isysroot $SDKROOT -m$target"
    export CXX=`xcrun -f clang++`" -arch $arch -isysroot $SDKROOT -m$target"
}

# the two files we need are js-confdefs.h which get used for the build and
# js-config.h for library consumers.  We also get different unity source files
# based on configuration, so save those too.

cd mozilla-release/js/src
rm config.cache

PYTHON=python CROSS_COMPILE=1 CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ ./configure --without-x --without-intl-api --enable-posix-nspr-emulation --disable-trace-logging --target=arm-none-linux


cd ../../..

rm -rf $_Path/

mkdir -p $_Path/build
mkdir $_Path/include

cp mozilla-release/js/src/js/src/js-confdefs.h $_Path/build
cp mozilla-release/js/src/js/src/*.cpp $_Path/build
cp mozilla-release/js/src/js/src/js-config.h $_Path/include

for unified_file in $(ls -1 $_Path/build/*.cpp) ; do
	sed 's/#include ".*\/js\/src\//#include "/' < $unified_file > t1
	sed 's/#error ".*\/js\/src\//#error "/' < t1 > $unified_file
	rm t1
done


