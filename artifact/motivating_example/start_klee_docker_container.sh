#!/bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
cd ${REPO_ROOT}

if [ -z "$1" ]; then
    docker run -it --rm -u $(id -u):$(id -g) -v $(realpath ./submodules/FPGen):/home/fptesting/FPTesting -v $(realpath ./artifact/motivating_example):/home/fptesting/EXCVATE_motivating_example --ulimit='stack=-1:-1' ucdavisplse/fpgen-artifact:icse20
else
    command="cd /home/fptesting && source .profile && ${1}"
    docker run --rm -u $(id -u):$(id -g) -v $(realpath ./submodules/FPGen):/home/fptesting/FPTesting -v $(realpath ./artifact/motivating_example):/home/fptesting/EXCVATE_motivating_example --ulimit='stack=-1:-1' ucdavisplse/fpgen-artifact:icse20 bash -c "${command}"
fi