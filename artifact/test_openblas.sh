#!/bin/bash

run_test() {
    local compilers="$1"
    local option_tuples="$2"

    for compiler in ${compilers}; do
        echo ================= ${compiler} default

        ./artifact/install_openblas_0.3.28.sh ${compiler}
        cp submodules/OpenBLAS-0.3.28/libopenblas_sandybridge-r0.3.28.a submodules/compiled_blas_libs/libopenblas_${compiler}.a
        pushd submodules/OpenBLAS-0.3.28/test/ || exit

        rm -rf __EXCVATE_${compiler}

        SECONDS=0
        execution_selector -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat1 2>&1 | tee -a nohup.out
        execution_selector -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat2 < sblat2.dat 2>&1 | tee -a nohup.out
        date -ud "@$SECONDS" "+Time elapsed (execution_selector): %H:%M:%S"

        SECONDS=0
        exception_spoofer -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat1 2>&1 | tee -a nohup.out
        exception_spoofer -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat2 < sblat2.dat 2>&1 | tee -a nohup.out
        date -ud "@$SECONDS" "+Time elapsed (exception_spoofer): %H:%M:%S"

        SECONDS=0
        input_generator -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat1 2>&1 | tee -a nohup.out
        input_generator -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat2 < sblat2.dat 2>&1 | tee -a nohup.out
        date -ud "@$SECONDS" "+Time elapsed (input_generator): %H:%M:%S"

        mv __EXCVATE __EXCVATE_${compiler}
        mv nohup.out __EXCVATE_${compiler}

        popd || exit

        for option_tuple in ${option_tuples}; do
            echo ================= ${compiler} ${option_tuple//_/ }

            ./artifact/install_openblas_0.3.28.sh ${compiler} ${option_tuple//_/ }
            cp submodules/OpenBLAS-0.3.28/libopenblas_sandybridge-r0.3.28.a submodules/compiled_blas_libs/libopenblas_${compiler}_${option_tuple//=/-}.a

            pushd submodules/OpenBLAS-0.3.28/test/ || exit

            rm -rf __EXCVATE_${compiler}_${option_tuple//=/-}

            SECONDS=0
            execution_selector -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat1 2>&1 | tee -a nohup.out
            execution_selector -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat2 < sblat2.dat 2>&1 | tee -a nohup.out
            date -ud "@$SECONDS" "+Time elapsed (execution_selector): %H:%M:%S"

            SECONDS=0
            exception_spoofer -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat1 2>&1 | tee -a nohup.out
            exception_spoofer -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat2 < sblat2.dat 2>&1 | tee -a nohup.out
            date -ud "@$SECONDS" "+Time elapsed (exception_spoofer): %H:%M:%S"

            SECONDS=0
            input_generator -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat1 2>&1 | tee -a nohup.out
            input_generator -f ../../../artifact/blas_prototypes_single_precision_level12 -- ./sblat2 < sblat2.dat 2>&1 | tee -a nohup.out
            date -ud "@$SECONDS" "+Time elapsed (input_generator): %H:%M:%S"

            mv __EXCVATE __EXCVATE_${compiler}_${option_tuple//=/-}
            mv nohup.out __EXCVATE_${compiler}_${option_tuple//=/-}

            popd || exit
        done
    done
}

REPO_ROOT=$(git rev-parse --show-toplevel)
cd ${REPO_ROOT}

./artifact/install_openblas_0.3.28.sh init

COMPILERS="gcc"
OPTION_TUPLES='-O3 -ffast-math -O3_-ffast-math'

run_test "${COMPILERS}" "${OPTION_TUPLES}"

COMPILERS="icx"
OPTION_TUPLES='-O3 -O3_-fp-model=fast=1 -O3_-fp-model=fast=2'

run_test "${COMPILERS}" "${OPTION_TUPLES}"
