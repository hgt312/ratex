#!/usr/bin
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

set -ex

if [ "$#" -lt 2 ]; then
    echo "Usage: ubuntu_install_torch.sh <cpu|cu113> <nightly|version>"
    exit 1
fi
PLATFORM=$1
VERSION=$2

if [ "$VERSION" == "nightly" ]; then
    # Nightly build
    python3 -m pip install --force-reinstall --pre torch -f https://download.pytorch.org/whl/nightly/$PLATFORM/torch_nightly.html
    LIBTORCH_LINK=https://download.pytorch.org/libtorch/nightly/$PLATFORM/libtorch-cxx11-abi-shared-with-deps-latest.zip
else
    # Stable build
    python3 -m pip install torch==$VERSION+$PLATFORM -f https://download.pytorch.org/whl/$PLATFORM/torch_stable.html
    LIBTORCH_LINK=https://download.pytorch.org/libtorch/$PLATFORM/libtorch-cxx11-abi-shared-with-deps-$VERSION%2Bcpu.zip
fi


# Install PyTorch
PYTORCH_GIT_SHA=$(python3 -c "import torch; print(torch.version.git_version)")
PYTORCH_INSTALL_PATH=$(dirname `python3 -c "import torch; print(torch.__file__)"`)

# Install libtorch with cxx11 ABIs
pushd .
cd /tmp
wget -O libtorch-cxx11.zip $LIBTORCH_LINK
unzip libtorch-cxx11.zip
cp -rf libtorch/* $PYTORCH_INSTALL_PATH/
rm -rf libtorch libtorch-cxx11.zip
popd

# Clone PyTorch for header files
git clone https://github.com/pytorch/pytorch.git
cd pytorch
git checkout $PYTORCH_GIT_SHA

python3 -m pip install -r requirements.txt