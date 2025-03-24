#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
cd ${REPO_ROOT}/submodules

KIT_ID=pin-external-3.31-98861-g71afcc22f-gcc-linux

# clean
rm -rf ${KIT_ID}

# acquire src
if ! wget https://software.intel.com/sites/landingpage/pintool/downloads/${KIT_ID}.tar.gz; then
    exit 1
fi
tar -xvf ${KIT_ID}.tar.gz
rm ${KIT_ID}.tar.gz

# symlink in header-only utils file
pushd ${KIT_ID}/source/tools/Utils
for x in $(find ../../../../../src/pintools -name "*.h")
do
    ln -sf $x $(basename $x)
done
popd

# set up directory for EXCVATE pintools
cd ${REPO_ROOT}/submodules/${KIT_ID}/source/tools/
mkdir EXCVATE
cp MyPinTool/makefile* EXCVATE
cd EXCVATE
mkdir obj-intel64
 
# acquire version of boost required for EXCVATE pintools
cd ${REPO_ROOT}/submodules
if [ ! -d boost_1_66_0 ]; then
    wget https://archives.boost.io/release/1.66.0/source/boost_1_66_0.tar.gz
    tar -xvf boost_1_66_0.tar.gz
    rm boost_1_66_0.tar.gz
fi
cd ${REPO_ROOT}/submodules/${KIT_ID}/source/tools/EXCVATE
echo TOOL_CXXFLAGS+=-I${REPO_ROOT}/submodules/boost_1_66_0 >> makefile.rules

# symlink in src for pintools and compile
for x in $(find ../../../../../src/pintools -name "*.cpp")
do
    ln -sf $x $(basename $x)
    make obj-intel64/$(basename ${x%.*}).so || true
done

cd ${REPO_ROOT}/submodules