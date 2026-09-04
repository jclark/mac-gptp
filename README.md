# mac-gptp

macOS includes a complete IEEE 802.1AS (gPTP) implementation in the kernel.
AVB audio and AirPlay use this implementation.
When a Mac with an AVB-capable Ethernet adapter is connected to a gPTP grandmaster, the kernel maintains a virtual clock synchronized to the grandmaster's time.
gPTP does not synchronize the Mac's system clock.
Apple exposes the gPTP clock through TimeSync, a private framework with no public documentation.
This repository allows you to make use of this.
The main program is `gptp-refclock` which uses the gPTP clock to provide timing information to chrony.
There is also a C library that wraps this framework.

![ChronyControl showing GPTP as chrony's selected source with a sub-microsecond RMS offset](docs/ChronyControl-GPTP.png)

The code and documentation in this repository have been written almost entirely by Claude Code Fable 5.1 and ChatGPT 5.6 Sol, with extensive direction from me.

## Hardware requirements

The Mac needs to have an Ethernet interface that supports AVB, since macOS enables gPTP only on such an interface.
`system_profiler SPEthernetDataType` reports `AVB Support: Yes` for a suitable interface.
There are two ways to get an AVB-capable interface: onboard Ethernet on a desktop Mac, or a Thunderbolt Ethernet adapter.
An ordinary USB-C Ethernet adapter will not work.
The cheapest Thunderbolt option is usually a used Apple Thunderbolt to Gigabit Ethernet Adapter connected through an Apple Thunderbolt 3 (USB-C) to Thunderbolt 2 Adapter.
This is the combination used for the tests here.

The PTP grandmaster must speak the IEEE 802.1AS profile.
A suitable Linux grandmaster can use a GPS module and [SatPulse](https://satpulse.net/) to synchronize an Ethernet port's PTP hardware clock.
`ptp4l` from [linuxptp](https://www.linuxptp.org/) serves time from that clock.
Configure `ptp4l` with the `gPTP.cfg` supplied with linuxptp so that it uses the 802.1AS profile required by the Mac.

Separately, each link on the path between the grandmaster and the Mac must pass the 802.1AS peer-delay check.
The TimeSync port used here reports a fixed upper limit of 1000 ns and does not become `asCapable` when the measured delay exceeds it.
The path can use an AVB switch or another switch with explicit 802.1AS support.
AVB switches are purpose-built for this use but are expensive.
I do not have either kind of switch, so I have not tested these options.
An ordinary Ethernet switch will not work.

A simple alternative is to use a PTP grandmaster with multiple network ports and have a cable from the Mac to one of those.
I used a more complicated [setup](docs/test-setup.md) with a four-port Linux mini-PC acting as a boundary clock, with direct cables to the grandmaster and the Mac.
Good performance from this setup requires [PCIe Precision Time Measurement](https://satpulse.net/hardware/ptm.html), so that time can be accurately transferred between a port's PTP hardware clock and the system clock.
My experience is that PTM will work on a mini-PC with an N5105 or later processor and Intel I226 ports.
Run `phc_ctl eth1` to check a port; working PTM is reported as `has cross timestamping support`.

## What it gives

The accuracy of the gPTP virtual clock depends on the setup.
In my tests, successive estimates of the relationship between mach absolute time and gPTP time agreed to about 250 ns over intervals of a few seconds.
Comparison with a GPS pulse showed that the virtual clock was stable to within the checking method's resolution of a few microseconds.
I would trust the virtual clock to about a microsecond in absolute terms.
The remaining uncertainty is a fixed offset in the Ethernet-adapter-to-mach cross-timestamp that cannot be measured on the Mac.

With chrony steering the system clock from the refclock in this repository, the system clock tracked the grandmaster with a residual of 0.6 us RMS.
The limiting factor is the Mac's oscillator, which wanders by several hundred ppb over a few minutes, rather than the link.
The runs are described in [gptp-refclock](docs/gptp-refclock.md).

## The library

[libgptp](docs/libgptp.md) is a C interface to the Mac's gPTP clock, built as `libgptp.a` from `gptp.h` and `gptp.m`.
A program can use it to add a gPTP port, read the state of the domain and its mapping, and convert any mach time to grandmaster time.

## The programs

[`gptp-refclock`](docs/gptp-refclock.md) is a chrony reference clock.
It compares the gPTP mapping with the system clock and sends the differences to chrony as complete SOCK samples.
chrony uses these samples to make the system clock follow the grandmaster.

[`gptp-pps-offset`](docs/gptp-pps-offset.md) compares the gPTP clock with a GPS PPS signal received on the CTS pin of a USB serial adapter.
The comparison provides a sanity check on gPTP.
If Apple's gPTP implementation is trusted, it also estimates the bias in PPS timestamps made by polling the adapter.
It prints the value to use for chrony's `offset` option on the SatPulse refclock line.

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

The Mac code was first verified on a single Mac mini M4 running macOS 15.7.7 with TimeSync 1340.13.
The refclock continues to work without code changes on the same Mac running macOS Tahoe 26.6.2 with TimeSync 1460.2.
If another macOS release renames a class or selector, the library fails at start-up and names the missing class or selector.
`tsdump` then shows what the class provides in that release.
The refclock has to run as root only because chrony creates its socket with permissions that allow only root to write to it.

## Documentation

The [documentation index](docs/README.md) covers the library, programs, private framework and test setup.
