<div style="font-family:JetBrainsMono Nerd Font">
<div align="justify">

![Arachne's Logo](https://raw.githubusercontent.com/astrogewgaw/logos/main/rasters/arachne.png)

## Why?

## Build

To build, simply type and run `make`. If you have [**Zig**](https://ziglang.org/) installed, type and run `make cross` instead to build a target cross-compiled for the x86_64 architecture on Linux systems.

## Usage

To use, just run the binary with the right arguments. Currently, one needs to pass the path to the configuration file, and the path to a `*.sim` file which contains the simulated FRB one wishes to inject into an ongoing observation. The `*.sim` file is generated from a `*.dat` file generated via the [**simulateSearch**](https://bitbucket.csiro.au/projects/PSRSOFT/repos/simulatesearch) program. You also need to either be at the telescope itself, or you need to have a file to shared memory emulator running on a local machine or server. One that matches the current layout of shared memory used at the GMRT can be found inside the [**`gptool`**](https://github.com/chowdhuryaditya/gptool) library: the **`fileToSHM`** binary.

For a local test, the following commands work:

1. **`fileToSHM -doFRB -f <PATH to *.raw FILE>`**
2. **`./arachne -c <PATH TO CONFIG FILE> -b <PATH TO *.sim FILE>`**

> **Note**: `fileToSHM` runs successfully only for GMRT raw data consisting of 8-bit integers, with 4096 channels and 1.31072 milliseconds sampling time. These parameters are hard-coded, and hence are not subject to change.

## Cite

Coming soon 😁 !
