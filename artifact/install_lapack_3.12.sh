#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
VERSION_NUMBER=3.12.0

if [ "$1" == "init" ]; then

    cd ${REPO_ROOT}/submodules

    # clean
    rm -rf lapack-${VERSION_NUMBER}

    # acquire src
    if ! wget https://github.com/Reference-LAPACK/lapack/archive/refs/tags/v${VERSION_NUMBER}.tar.gz; then
        exit 1
    fi
    tar -xvf v${VERSION_NUMBER}.tar.gz
    rm v${VERSION_NUMBER}.tar.gz

else

    compiler="$1"

    shift
    options="$*"

    cd ${REPO_ROOT}/submodules/lapack-${VERSION_NUMBER}

    make clean

    if [ ${compiler} = "ifx" ]; then
        cp INSTALL/make.inc.ifort make.inc
    else
        cp INSTALL/make.inc.${compiler} make.inc
    fi
    
    sed -i "/FC =/c\FC = ${compiler}" make.inc 

    # add debugging information and ISA specification
    sed -i "/FFLAGS = -O3 -fp-model strict -assume protect_parens -recursive/c\FFLAGS = -O3 -fp-model strict -assume protect_parens -recursive ${options} -g -xSANDYBRIDGE" make.inc
    sed -i "/FFLAGS = -O2 -frecursive/c\FFLAGS = -O2 -frecursive ${options} -g -march=sandybridge" make.inc

    # minimal build of LAPACK/BLAS
    make -j 8 blaslib blas_testing # lapacklib tmglib lapack_testing
fi