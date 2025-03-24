FROM intel/hpckit:2024.2.0-1-devel-ubuntu22.04

WORKDIR /home/

RUN /bin/bash -c 'apt-get update'
RUN /bin/bash -c 'apt-get install -y python3.10-venv'
RUN /bin/bash -c 'apt-get install -y gfortran=4:11.2.0-1ubuntu1'

# cvc5 dependencies
RUN /bin/bash -c 'apt-get install -y autoconf libtool libedit-dev'

RUN /bin/bash -c 'git config --global --add safe.directory "*" && cp /root/.profile* /home/ && cp /root/.bashrc* /home/'