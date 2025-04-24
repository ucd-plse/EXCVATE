#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)

cd ${REPO_ROOT}/submodules
git clone https://github.com/ucd-plse/FPGen.git
cd FPGen

docker pull ucdavisplse/fpgen-artifact:icse20