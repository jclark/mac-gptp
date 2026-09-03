# mac-gptp

macOS includes a complete IEEE 802.1AS (gPTP) implementation in the kernel.
AVB audio and AirPlay use this implementation.
When a Mac with an AVB-capable Ethernet adapter is connected to a gPTP grandmaster, the kernel maintains a virtual clock synchronized to the grandmaster's time.
gPTP does not synchronize the Mac's system clock.
Apple exposes the gPTP clock through TimeSync, a private framework with no public documentation.
This repository contains a C library for using TimeSync and two programs built on the library.

## What macOS needs

The Ethernet adapter must support AVB, since macOS enables gPTP only on AVB adapters.
`system_profiler SPEthernetDataType` shows "AVB Support: Yes" for an adapter that qualifies.
The built-in Ethernet port of a Mac mini qualifies, as does the Apple Thunderbolt to Gigabit Ethernet adapter.

The path from the adapter to the grandmaster must have a small enough peer delay.
802.1AS measures the delay to the neighbour on each link and rejects links over a fixed limit.
On my Mac, this limit is 1000 ns.
An ordinary switch exceeds this limit.
A switch with 802.1AS support is the most convenient solution, but these are not cheap.
A direct cable is the cheapest solution,
but this requires a more complex network setup where at least one of the computers has multiple network connections.

The grandmaster must speak 802.1AS.
For example, ptp4l can act as the grandmaster when configured using the gPTP profile.
gPTP works at layer 2, so the link needs no IP configuration.
The Mac's port can carry whatever address the other side assigns it, or none at all.

## What it gives

The accuracy of the virtual clock depends on the setup.
My test used a direct gigabit cable from the Thunderbolt adapter to a grandmaster whose port clock was held within 10 ns of GPS-derived time.
Samples of the mapping taken a few seconds apart agreed to about 250 ns.
A GPS pulse checked against the mapping stayed steady within the few microseconds that the checking method can resolve.
I would trust the mapping to about a microsecond in absolute terms.
The remaining uncertainty is a fixed offset in the adapter-to-mach cross-timestamp that cannot be measured on the Mac.

With chrony steering the system clock from the refclock in this repository, the system clock tracked the grandmaster with a residual of 0.6 us RMS.
The limiting factor is the Mac's oscillator, which wanders by several hundred ppb over a few minutes, rather than the link.
The runs are described in [gptp-refclock](docs/gptp-refclock.md).

## The library

`libgptp.a`, built from `gptp.h` and `gptp.m`, is a C interface to the Mac's gPTP clock.
A program can use it to add a gPTP port, read the state of the domain and its mapping, and convert any mach time to grandmaster time.
The [libgptp](docs/libgptp.md) page describes the interface.

## The programs

`gptp-refclock INTERFACE` is a chrony reference clock.
It compares the gPTP mapping with the system clock and sends the differences to chrony as complete SOCK samples.
chrony uses these samples to make the system clock follow the grandmaster.
With the chrony configuration given in [gptp-refclock](docs/gptp-refclock.md), the system clock tracked the grandmaster with a residual of 0.6 us RMS.

`gptp-pps-offset` uses the gPTP clock to measure the latency of detecting a GPS pulse by polling a serial port's CTS pin.
It prints the value to use for chrony's `offset` option on that refclock's line.
The [gptp-pps-offset](docs/gptp-pps-offset.md) page describes the measurement.

`tsdump` lists the methods of any class in the framework, with their type encodings.
It was used to discover the TimeSync interface and can show what changed if a future macOS release breaks the library.

## Building

    make
    sudo make install

`make` builds `libgptp.a`, the two programs and `tsdump`, linking only Foundation.
The library loads the private framework at run time.
`make install` puts the programs in `/usr/local/bin`, and the library and header in `/usr/local/lib` and `/usr/local/include`.
`gptp.m` is Objective-C and everything else is C.

## Caveats

The framework is private and undocumented.
Everything here was verified on a single Mac mini M4 running macOS 15.7.7 with TimeSync 1340.13.
If another macOS release renames a class or selector, the library fails at start-up and names the missing class or selector.
`tsdump` then shows what the class provides in that release.
The programs need no entitlement.
The refclock has to run as root only because chrony creates its socket with permissions that allow only root to write to it.

## Files

- `gptp.h`, `gptp.m`: the library.
- `gptp-refclock.c`, `chrony-client.c`, `chrony-client.h`: the refclock and the chrony SOCK client.
- `gptp-pps-offset.c`: the PPS path measurement.
- `tsdump.m`: the class lister.
- `docs/`: [libgptp](docs/libgptp.md), [the TimeSync framework as observed](docs/timesync.md), [gptp-refclock](docs/gptp-refclock.md) and [gptp-pps-offset](docs/gptp-pps-offset.md).
