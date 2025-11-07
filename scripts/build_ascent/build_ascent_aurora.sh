#!/bin/bash -l

# Proxies
export HTTP_PROXY=http://proxy.alcf.anl.gov:3128
export HTTPS_PROXY=http://proxy.alcf.anl.gov:3128
export http_proxy=http://proxy.alcf.anl.gov:3128
export https_proxy=http://proxy.alcf.anl.gov:3128

module reset
module use /soft/modulefiles
module load cmake
module load python/3.10.14
module load py-cython py-numpy py-pip py-wheel py-setuptools

env CC=`which mpicc` CXX=`which mpicxx` FTN=`which mpifort` enable_64bit_ids=ON enable_sycl=ON enable_mpi=ON enable_find_mpi=OFF enable_fortran=ON enable_python=ON raja_enable_vectorization=ON enable_tests=OFF enable_verbose=OFF ./build_ascent_sycl.sh
