#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
rm -rf ${REPO_ROOT}/submodules 
mkdir ${REPO_ROOT}/submodules

set -e

source ${REPO_ROOT}/scripts/install/install_pin_3.31.sh
source ${REPO_ROOT}/scripts/install/install_python_packages.sh
source ${REPO_ROOT}/scripts/install/install_cvc5_1.2.0.sh