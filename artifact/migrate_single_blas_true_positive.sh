#! /bin/bash

infile_path=$(realpath $1)
lib_path=$(realpath $2)

REPO_ROOT=$(git rev-parse --show-toplevel)
rm -rf ${REPO_ROOT}/test_migration
mkdir ${REPO_ROOT}/test_migration
cd ${REPO_ROOT}/test_migration

func_name=$(basename $infile_path)
func_name=${func_name%.*.*.*.*}

echo program main > driver.f90
echo "call ${func_name%?}()" >> driver.f90
echo end program main >> driver.f90

mkdir -p __EXCVATE/${func_name}
cp $(dirname $infile_path)/${func_name}.io_vars __EXCVATE/${func_name}
echo ${infile_path} >> __EXCVATE/${func_name}/__input_file_list

if echo $(basename ${lib_path}) | grep -q -e "g"
then
    gfortran driver.f90 $lib_path
else
    ifx driver.f90 $lib_path
fi

../submodules/pin-external-3.31-98861-g71afcc22f-gcc-linux/pin -t ../submodules/pin-external-3.31-98861-g71afcc22f-gcc-linux/source/tools/EXCVATE/obj-intel64/taint_tracker.so -f ../artifact/blas_prototypes_single_precision_level12/${func_name}.prototype -x 1 -- ./a.out

cd ${REPO_ROOT}
rm -rf test_migration