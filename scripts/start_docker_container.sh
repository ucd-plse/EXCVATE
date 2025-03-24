#!/bin/bash

if [ -z "$1" ]; then
    docker run -it --rm -u $(id -u):$(id -g) -v $(realpath $(git rev-parse --show-toplevel)):/home/EXCVATE ucdavisplse/excvate:artifact bash -c "cd /home/EXCVATE && source scripts/set_env.sh && bash --rcfile /home/.bashrc"
else
    command="cd /home/EXCVATE && source scripts/set_env.sh && ${1}"
    docker run --rm -u $(id -u):$(id -g) -v $(realpath $(git rev-parse --show-toplevel)):/home/EXCVATE ucdavisplse/excvate:artifact bash -c "${command}"
fi