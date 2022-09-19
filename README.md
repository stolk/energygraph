# energygraph

Graphs the energy use of a host inside a text terminal, using intel-rapl data from the /sys filesystem.

![screenshot](images/screenshot0.png "screenshot")

## Building

$ make

## Running

$ sudo ./energygraph

## Compatibility

Probably only works on Intel CPUs.

Requires /sys/devices/virtual/powercap/intel-rapl/ entries.

Requires root privileges.

## Copyright

Copyright 2022 by Bram Stolk, licensed using the MIT Open Source License.

