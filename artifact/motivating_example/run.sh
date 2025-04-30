#!/bin/bash

timelimit=5
klee=/home/fptesting/klee-float
filecount=1

function_name=sgbmv

cd /home/fptesting/EXCVATE_motivating_example

rm -f test.out

total_parameterization_count=$(wc -l parameterizations.txt)
count=0

while read x
do
    count=$((count + 1))
    echo ============= $count/$total_parameterization_count >> test.out
    echo ============= $count/$total_parameterization_count >> nohup.out

    for y in $x
    do
        sed -i "s|${y%%.*} =;|${y%%.*} = ${y#*.};|" ${function_name}_f2c_check_harness.c
        sed -i "s|${y%%.*} =;|${y%%.*} = ${y#*.};|" ${function_name}_f2c_se_test_harness.c
    done

    /usr/bin/clang-3.4 -I$klee/include -emit-llvm -c -g ${function_name}_f2c_se_test_harness.c -o ${function_name}_f2c_se_test_harness.bc
    /home/fptesting/FPTesting/src/klee-float/build/bin/klee -allow-external-sym-calls --max-solver-time=$timelimit --max-time=$timelimit --search=nurs:covnew ${function_name}_f2c_se_test_harness.bc
    /home/fptesting/FPTesting/scripts/show_tests.sh | grep -oE "x\[[0-9]+\].* " > temp.txt

    while read z
    do
        if [[ $z == x\[1\]* ]] && [[ -f __input_${filecount} ]]
        then
            sed -i "s|nan|0.0/0.0|g" __input_${filecount}
            sed -i "s|-inf|-1.0/0.0|g" __input_${filecount}
            sed -i "s|inf|1.0/0.0|g" __input_${filecount}
            sed -i "s|#include<__>|#include<__input_${filecount}>|g" ${function_name}_f2c_check_harness.c
            /usr/bin/gcc -std=c99 ${function_name}_f2c_check_harness.c -I.
            echo "input_${filecount} "$(./a.out) >> test.out
            sed -i "s|#include<__input_${filecount}>|#include<__>|g" ${function_name}_f2c_check_harness.c
            mv __input_${filecount} input_${filecount}
            filecount=$((filecount + 1))
        fi
        echo "$z;" >> __input_${filecount}
    done < temp.txt

    rm temp.txt

    for y in $x
    do
        sed -i "s|${y%%.*} = ${y#*.};|${y%%.*} =;|" ${function_name}_f2c_check_harness.c
        sed -i "s|${y%%.*} = ${y#*.};|${y%%.*} =;|" ${function_name}_f2c_se_test_harness.c
    done

    echo >> test.out

done < parameterizations.txt

echo
echo ============= ${function_name} Symbolic Execution Results
echo $(grep -e "!" test.out | wc -l)/$(cat test.out | wc -l) exception-causing inputs
echo $(grep -e "!" test.out | grep -v "nan" | grep -v "inf" | wc -l)/$(cat test.out | wc -l) exception-handling failures
echo ==============================================
echo
