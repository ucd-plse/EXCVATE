#! /bin/bash
REPO_ROOT=$(git rev-parse --show-toplevel)
export PATH=${PATH}:${REPO_ROOT}/scripts

if [ -f .venv/bin/activate ];
then
    source .venv/bin/activate
fi