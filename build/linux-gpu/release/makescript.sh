#!/bin/sh

# make -j8 -O BASE=/usr CUDA_INCLUDE=/opt/cuda/include OPENBABEL_INCLUDE=/usr/include/openbabel-2.0  NVCC=/opt/cuda/bin/nvcc LDFLAGS='-L/usr/lib -L. -L/usr/local/lib -L/opt/cuda/lib64'

if [ "$(hostname)" = "t410" ]
then
make -j2 BASE=/usr CUDA_INCLUDE=/opt/cuda/include OPENBABEL_INCLUDE=/usr/include/openbabel-2.0  NVCC=/opt/cuda/bin/nvcc LDFLAGS='-L/usr/lib -L. -L/usr/local/lib -L/opt/cuda/lib64'
else
make -j2 BASE=/usr CUDA_INCLUDE=/usr/local/cuda/include OPENBABEL_INCLUDE=/usr/include/openbabel-2.0  NVCC=/usr/local/cuda/bin/nvcc LDFLAGS='-L/usr/lib -L. -L/usr/local/lib -L/usr/local/cuda/lib64'
fi

cp smina.gpu ../../../test/