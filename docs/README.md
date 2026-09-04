# Documentation

- [gptp-refclock](gptp-refclock.md) explains how the program derives chrony samples from the Mac's gPTP clock, how to configure chrony and what the tests showed.
- [gptp-pps-offset](gptp-pps-offset.md) explains how a GPS PPS comparison provides a sanity check on gPTP and, if gPTP is trusted, an offset correction for a SatPulse serial refclock.
- [libgptp](libgptp.md) documents the C interface to the Mac's gPTP clock and the lifetime and threading rules for using it.
- [The TimeSync framework, as observed](timesync.md) records the private framework classes, mappings and behaviour found through runtime inspection and experiments.
- [Test setup](test-setup.md) describes the three machines, hardware, cabling, networking and software used for the measurements.
