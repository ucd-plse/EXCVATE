#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
cd ${REPO_ROOT}/submodules

if ! wget https://github.com/cvc5/cvc5/archive/refs/tags/cvc5-1.2.0.tar.gz
then
    exit 1
fi
tar -xvf cvc5-1.2.0.tar.gz
rm cvc5-1.2.0.tar.gz

cd cvc5-cvc5-1.2.0
./configure.sh --auto-download --best --gpl --prefix=./install
cd build
make -j8
make install