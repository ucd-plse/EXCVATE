#!/bin/bash

run_test() {
    local compilers="$1"
    local option_tuples="$2"

    for compiler in ${compilers}; do
        echo ================= ${compiler} default

        ./artifact/install_lapack_3.12.sh ${compiler}
        cp submodules/lapack-3.12.0/librefblas.a submodules/compiled_blas_libs/librefblas_${compiler}.a
        pushd submodules/lapack-3.12.0/BLAS/TESTING/ || exit

        rm -rf __EXCVATE_${compiler}

        SECONDS=0
        execution_selector -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat1s 2>&1 | tee -a nohup.out
        execution_selector -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat2s < sblat2.in 2>&1 | tee -a nohup.out
        date -ud "@$SECONDS" "+Time elapsed (execution_selector): %H:%M:%S"

        SECONDS=0
        exception_spoofer -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat1s 2>&1 | tee -a nohup.out
        exception_spoofer -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat2s < sblat2.in 2>&1 | tee -a nohup.out
        date -ud "@$SECONDS" "+Time elapsed (exception_spoofer): %H:%M:%S"

        SECONDS=0
        input_generator -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat1s 2>&1 | tee -a nohup.out
        input_generator -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat2s < sblat2.in 2>&1 | tee -a nohup.out
        date -ud "@$SECONDS" "+Time elapsed (input_generator): %H:%M:%S"

        mv __EXCVATE __EXCVATE_${compiler}
        mv nohup.out __EXCVATE_${compiler}

        popd || exit

        for option_tuple in ${option_tuples}; do
            echo ================= ${compiler} ${option_tuple//_/ }

            ./artifact/install_lapack_3.12.sh ${compiler} ${option_tuple//_/ }
            cp submodules/lapack-3.12.0/librefblas.a submodules/compiled_blas_libs/librefblas_${compiler}_${option_tuple//=/-}.a

            pushd submodules/lapack-3.12.0/BLAS/TESTING/ || exit

            rm -rf __EXCVATE_${compiler}_${option_tuple//=/-}
            
            SECONDS=0
            execution_selector -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat1s 2>&1 | tee -a nohup.out
            execution_selector -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat2s < sblat2.in 2>&1 | tee -a nohup.out
            date -ud "@$SECONDS" "+Time elapsed (execution_selector): %H:%M:%S"

            SECONDS=0
            exception_spoofer -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat1s 2>&1 | tee -a nohup.out
            exception_spoofer -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat2s < sblat2.in 2>&1 | tee -a nohup.out
            date -ud "@$SECONDS" "+Time elapsed (exception_spoofer): %H:%M:%S"

            SECONDS=0
            input_generator -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat1s 2>&1 | tee -a nohup.out
            input_generator -f ../../../../artifact/blas_prototypes_single_precision_level12 -- ./xblat2s < sblat2.in 2>&1 | tee -a nohup.out
            date -ud "@$SECONDS" "+Time elapsed (input_generator): %H:%M:%S"

            mv __EXCVATE __EXCVATE_${compiler}_${option_tuple//=/-}
            mv nohup.out __EXCVATE_${compiler}_${option_tuple//=/-}

            popd || exit
        done
    done
}

REPO_ROOT=$(git rev-parse --show-toplevel)
cd ${REPO_ROOT}

./artifact/install_lapack_3.12.sh init

COMPILERS="gfortran"
OPTION_TUPLES='-O3 -ffast-math -O3_-ffast-math'

run_test "${COMPILERS}" "${OPTION_TUPLES}"

COMPILERS="ifx"
OPTION_TUPLES='-fp-model=fast=1 -fp-model=fast=2'

run_test "${COMPILERS}" "${OPTION_TUPLES}"
