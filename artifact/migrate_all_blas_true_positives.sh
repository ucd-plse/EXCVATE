#! /bin/bash

path=$(realpath $1)

REPO_ROOT=$(git rev-parse --show-toplevel)
rm -rf ${REPO_ROOT}/test_migration
mkdir ${REPO_ROOT}/test_migration
cd ${REPO_ROOT}/test_migration

SECONDS=0

# create driver for with dummy calls to all BLAS functions
echo program main > driver.f90
for prototype_path in $(find ../artifact/blas_prototypes_single_precision_level12/ -name "*.prototype")
do
    func_name=$(basename ${prototype_path})
    func_name=${func_name%.*}
    echo "call ${func_name%?}()" >> driver.f90
done
echo end program main >> driver.f90

# for each compiled library...
for lib_path in $(find ../submodules/compiled_blas_libs -name "*.a")
do
    echo "================== ${lib_path}" >> ${REPO_ROOT}/test_migration_results.txt

    # compile the driver linked to the library
    if echo $(basename ${lib_path}) | grep -q -e "g"
    then
        gfortran driver.f90 ${lib_path} > /dev/null 2>&1
    else
        ifx driver.f90 ${lib_path} > /dev/null 2>&1
    fi

    # for each function...
    for prototype_path in $(find ../artifact/blas_prototypes_single_precision_level12/ -name "*.prototype")
    do
        func_name=$(basename ${prototype_path})
        func_name=${func_name%.*}

        # ...and for each set of extracted io_vars for that function...
        for io_var_path in $(find ${path} -name "*.io_vars" | grep -e "${func_name}")
        do            

            # set up a fresh directory containing the io_vars file and all smt-generated inputs 
            rm -rf __EXCVATE/${func_name}
            mkdir -p __EXCVATE/${func_name}
            cp ${io_var_path} __EXCVATE/${func_name}
            for smt_generated_input_path in $(find $(dirname ${io_var_path}) -name "*.smt2.out*" | grep -v "event_trace")
            do
                echo ${smt_generated_input_path} >> __EXCVATE/${func_name}/__input_file_list
            done

            if [ -f __EXCVATE/${func_name}/__input_file_list ]
            then
                # call to taint tracker plugin
                ../submodules/pin-external-3.31-98861-g71afcc22f-gcc-linux/pin -t ../submodules/pin-external-3.31-98861-g71afcc22f-gcc-linux/source/tools/EXCVATE/obj-intel64/taint_tracker.so -f ../artifact/blas_prototypes_single_precision_level12/${func_name}.prototype -- ./a.out    
                
                # then, check which ones worked, which ones didn't
                for input_file_path in $(cat __EXCVATE/${func_name}/__input_file_list)
                do
                    if [ -f __EXCVATE/${func_name}/$(basename ${input_file_path}).event_trace ]
                    then
                        if grep -qe "**EXCVATE: TIMEOUT" __EXCVATE/${func_name}/$(basename ${input_file_path}).event_trace
                        then
                            echo "    [#] ${input_file_path}" >> ${REPO_ROOT}/test_migration_results.txt
                        else
                            echo "    [x] ${input_file_path}" >> ${REPO_ROOT}/test_migration_results.txt
                        fi
                        grep -e "(out)" __EXCVATE/${func_name}/$(basename ${input_file_path}).event_trace >> ${REPO_ROOT}/test_migration_results.txt
                    else
                        echo "    [ ] ${input_file_path}" >> ${REPO_ROOT}/test_migration_results.txt
                    fi
                done
            fi
        done

    done
done

date -ud "@$SECONDS" "+Time elapsed: %H:%M:%S"

cd ${REPO_ROOT}
rm -rf test_migration