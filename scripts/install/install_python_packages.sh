#! /bin/bash

REPO_ROOT=$(git rev-parse --show-toplevel)
cd ${REPO_ROOT}
python3 -m venv .venv
source .venv/bin/activate

pip3 install pandas
pip3 install plotly