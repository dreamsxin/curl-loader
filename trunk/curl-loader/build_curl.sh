#!/bin/bash
#
# curl_build.sh script builds libcurl library

CURL_BUILD=`pwd`/build/curl
CURL_VER=7.16.0

if [ ! -d ./packages/curl-${CURL_VER} ]; then  

    pushd ./packages || exit 1

    if [ ! -f ./packages/curl-${CURL_VER}.tar.gz ]; then
        echo "downloading curl tarball from the official curl site"
        wget --tries=5 -c http://curl.haxx.se/download/curl-${CURL_VER}.tar.gz || exit 1
    fi

    echo "extracting curl sources"
    tar zxf curl-${CURL_VER}.tar.gz || exit 1
    ln -sf curl-${CURL_VER} curl || exit 1 

    echo "patching curl"
    patch -d curl -p1 < ../patches/patch-curl-${CURL_VER} || exit 1
    patch -d curl -p1 < ../patches/patch-fdset-protection

    popd # back from ./packages
    
    echo "creating curl build directory"
    mkdir -p ${CURL_BUILD}

    pushd ${CURL_BUILD}

    # If you wish to build libcurl to support SSL (https, ftps), please, make sure
    # --with-ssl= to point to the path, where crypto.h from openssl library is located.
    # Install on debian package libssl-dev (additionally to openssl binaries).
    #
    # Place to CFLAGS=-g to enable the libcurl debugging. To use the most effective
    # optimized version of the library, place CFLAGS=-O2 instead of -g
    #
    ../../packages/curl/configure --prefix ${CURL_BUILD} \
        --enable-thread \
        --disable-ipv6 \
        --with-random=/dev/urandom \
        --with-ssl=/usr/include/openssl \
        --enable-shared=no \
        CFLAGS=-g \
        || exit 1

    popd # back from CURL_BUILD directory

    make -C ${CURL_BUILD} || exit 1
    make -C ${CURL_BUILD} install || exit 1
    
    # copy includes and lib to respective directories
    mkdir -p ./inc
    mkdir -p ./lib

    cp -a ${CURL_BUILD}/include/curl ./inc/curl  || exit 1
    cp -pf ${CURL_BUILD}/lib/libcurl* ./lib
fi