#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
VERSION_NUMBER=0.3.28

if [ "$1" == "init" ]; then

    cd ${REPO_ROOT}/submodules

    # clean
    rm -rf OpenBLAS-${VERSION_NUMBER}

    # acquire src
    if ! wget https://github.com/OpenMathLib/OpenBLAS/archive/refs/tags/v${VERSION_NUMBER}.tar.gz; then
        exit 1
    fi
    tar -xvf v${VERSION_NUMBER}.tar.gz
    rm v${VERSION_NUMBER}.tar.gz

    # serial build
    sed -i "s|# USE_THREAD = 0|USE_THREAD = 0|" ${REPO_ROOT}/submodules/OpenBLAS-${VERSION_NUMBER}/Makefile.rule

else

    ccompiler="$1"

    shift
    options="$*"

    cd ${REPO_ROOT}/submodules/OpenBLAS-${VERSION_NUMBER}

    sed -i "/^COMMON_OPT = -O2/c\COMMON_OPT = -O2 -g ${options}" Makefile.system
    if [[ "${options}" == *"-fp-model=fast"* ]]; then
        sed -i "/CCOMMON_OPT += -fp-model=consistent/c\#CCOMMON_OPT += -fp-model=consistent" Makefile.x86_64
    else
        sed -i "/CCOMMON_OPT += -fp-model=consistent/c\CCOMMON_OPT += -fp-model=consistent" Makefile.x86_64
    fi

    if [[ "$ccompiler" == "icx" ]]; then
        fcompiler=ifx
    else
        fcompiler=gfortran
    fi

    make clean    
    make -j 8 TARGET=SANDYBRIDGE FC=${fcompiler} CC=${ccompiler} NO_LAPACK=1
fi