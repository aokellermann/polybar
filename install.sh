#!/bin/bash

cd build
cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
make -j$(nproc)
sudo make install