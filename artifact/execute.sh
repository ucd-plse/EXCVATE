#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)

source ${REPO_ROOT}/artifact/install_blis_1.0.sh init
source ${REPO_ROOT}/artifact/install_openblas_0.3.28.sh init
source ${REPO_ROOT}/artifact/install_lapack_3.12.sh init

cd ${REPO_ROOT}
rm -rf submodules/compiled_blas_libs
mkdir submodules/compiled_blas_libs

rm -rf artifact/results
mkdir artifact/results

mkdir artifact/results/reference_blas
./artifact/test_reference_blas.sh > artifact/results/reference_blas/out.txt 2>&1
mv submodules/lapack-3.12.0/BLAS/TESTING/__EXCVATE* artifact/results/reference_blas/

mkdir artifact/results/blis
./artifact/test_blis.sh > artifact/results/blis/out.txt 2>&1
mv submodules/blis-1.0/blastest/__EXCVATE* artifact/results/blis/

mkdir artifact/results/openblas
./artifact/test_openblas.sh > artifact/results/openblas/out.txt 2>&1
mv submodules/OpenBLAS-0.3.28/test/__EXCVATE* artifact/results/openblas/

python3 ./scripts/reduce.py artifact/results
mv equivalence_classes.txt artifact/results

python3 ./artifact/generate_figure6_and_table1.py