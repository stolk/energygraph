# energygraph

Graphs the energy use of a host inside a text terminal, using intel-rapl data from the /sys filesystem.

## Building

$ make

## Running

$ ./energygraph

## Compatibility

Probably only works on Intel CPUs.

Requires /sys/devices/virtual/powercap/intel-rapl/ entries.

## Copyright

Copyright 2022 by Bram Stolk, licensed using the MIT Open Source License.

