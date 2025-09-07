# PipeWire 2 ASIO Wrapper

## Introduction

Wrapping PipeWire into ASIO in order to learn both technologies and find out similarities and differences.

> DISCLAIMER: highly experimental!!!

## Get Started

To clone and create the project, open a command prompt and proceed as follows:

### Linux

```cpp 
git clone https://.../pipewire2asio.git
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ../pipewire2asio
cmake --build .
```

## Get Help

###  PW Commandline Tools

* pw-cli list-objects | grep node.name
* pw-metadata -n settings (list sample rate and blocksize)

### Learnings

* SPA: https://docs.pipewire.org/page_spa.html
* Latency: https://docs.pipewire.org/devel/page_latency.html
* https://github.com/mikeroyal/PipeWire-Guide

