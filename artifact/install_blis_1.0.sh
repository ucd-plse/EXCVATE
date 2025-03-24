#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
VERSION_NUMBER=1.0

if [ "$1" == "init" ]; then

    cd ${REPO_ROOT}/submodules

    # clean
    rm -rf blis-${VERSION_NUMBER}

    # acquire src
    if ! wget https://github.com/flame/blis/archive/refs/tags/${VERSION_NUMBER}.tar.gz; then
        exit 1
    fi
    tar -xvf ${VERSION_NUMBER}.tar.gz
    rm ${VERSION_NUMBER}.tar.gz

    # replace icc compiler with icx
    sed -i "s|,icc)|,icx)|" ${REPO_ROOT}/submodules/blis-${VERSION_NUMBER}/config/sandybridge/make_defs.mk

else
    compiler="$1"

    shift
    options="$*"

    cd ${REPO_ROOT}/submodules/blis-${VERSION_NUMBER}
    make clean

    CC=${compiler} ./configure sandybridge

    # add debug info and options
    sed -i "/COPTFLAGS      := -O2/c\COPTFLAGS      := -O2 -g ${options}" config/sandybridge/make_defs.mk

    make -j 8 V=1

    cd blastest
    make -j 8 V=1
fi