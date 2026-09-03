# libgptp

libgptp is a C interface to the Mac's gPTP clock.
`gptp.h` declares the interface, `gptp.m` implements it, and `make` builds `libgptp.a`.
A program that uses it includes the header and links the archive together with Foundation:

    cc -o myprog myprog.c -lgptp -framework Foundation

The private framework is not linked at all.
The library loads it at run time with `dlopen`.

The interface is plain C.
Value types are structs whose names have the `gptp_` prefix.
A connection to the framework is represented by an opaque `gptp_t` handle.
Every function takes this handle as its first argument.
No privilege or entitlement is needed.

## What it gives

The kernel maintains an affine mapping from mach absolute time to grandmaster time.
It disciplines this mapping using syncs received with hardware timestamps on an AVB-capable port.
The library provides the parameters and state of the mapping, conversions through it, and updates at every sync.
These values have already passed through the kernel's servo.
The raw sync timestamps are not accessible from user space.

Once the domain is following a real grandmaster, times are TAI nanoseconds since the PTP epoch.
While the Mac is its own grandmaster, they are nanoseconds since boot.
The library works in TAI and leaves the UTC offset to the caller, since the framework does not provide a trustworthy value for it.
Mach times use the ticks returned by `mach_absolute_time`, since this is the unit accepted by the framework.
`gptp_mach_to_ns` and `gptp_ns_to_mach` convert between ticks and nanoseconds.
`gptp_time_to_ns` combines the seconds and nanoseconds of a `struct gptp_time`.

## A session

    char err[256];
    gptp_t *g = gptp_open(2000000000, err, sizeof err);      /* wait up to 2 s for the daemon */
    if (!g) { fprintf(stderr, "%s\n", err); exit(1); }
    if (!gptp_port_add(g, "en14", err, sizeof err)) { ... }

    struct gptp_domain_state d;
    gptp_domain_state(g, &d);
    if (d.lock_state == GPTP_LOCK_LOCKED && d.grandmaster_identity != d.clock_identity) {
        struct gptp_time t;
        gptp_time_from_mach(g, mach_absolute_time(), &t);   /* t.seconds, t.nanoseconds: TAI */
    }
    gptp_close(g);

`gptp_open` connects to the TimeSync daemon.
It uses `respondsToSelector` to check every class and selector required by the library.
If a macOS release has renamed one of them, `gptp_open` fails and names the missing class or selector.
Framework changes are therefore detected at startup rather than by a later crash in the calling program.
`tsdump` shows what the class provides in the new release, and the corresponding fix belongs in `gptp.m`.

`gptp_port_add` adds a link-layer gPTP port to the interface, or adopts a port that another process has already added there.
The kernel removes a port when the process that added it exits.
Other processes can use the domain through the port while it exists.
`gptp_close` removes the port only if the calling process added it.

## The domain

`gptp_domain_state` fills a `struct gptp_domain_state` with the lock state and the identities of the Mac's clock and the grandmaster.
It also returns the announce fields, which this framework reports as 0.
`GPTP_LOCK_LOCKED` has the value 2 and applies both when the domain follows an external grandmaster and when the Mac is its own grandmaster.
To detect an external time source, a program must therefore also check that the grandmaster identity differs from the Mac's own.
The lock state dropped to 1 for a few tens of milliseconds during a grandmaster change.

`gptp_port_state` fills a `struct gptp_port_state` for the port on an interface.
It reports whether the port exists, is enabled and is asCapable.
It also reports the identity and port number of the peer, the propagation delay with its minimum, maximum and limit, and the timestamping modes.
A timestamping mode of 1 means hardware timestamping.

`gptp_mapping` returns the mapping as a `struct gptp_mapping`.
The mapping consists of a fractional rate ratio, a mach anchor in ticks and a domain anchor in nanoseconds.
The domain time corresponding to a mach time is the domain anchor plus the ratio times the number of ticks since the mach anchor.
The kernel keeps the mapping continuous in phase: a new anchor always lies on the old line, and only the slope changes.
When the Mac is its own grandmaster, the ratio is exactly the mach timebase.
The ratio relative to the mach timebase, expressed in parts per billion, gives the grandmaster's frequency relative to the Mac's oscillator.
Changes in this value over time show the oscillator's frequency wander.

`gptp_metrics` fills in the kernel's metrics objects for the domain and the port.
On macOS 15.7 these stayed at zero apart from the count of grandmaster changes.

## Conversions

`gptp_time_from_mach` converts a mach time to a `struct gptp_time`.
The result contains seconds and nanoseconds, the identity of the grandmaster used for the conversion, and the timescale and traceability flags from the announce.
The PTP timescale flag is true even when the Mac is its own grandmaster, so by itself it does not indicate an external time source.

`gptp_domain_from_mach` does the same conversion but returns a plain count of nanoseconds, with the grandmaster identity as an out-parameter.
`gptp_mach_from_domain` performs the inverse conversion, allowing a future domain time to be used as a deadline in mach ticks.

## Events

`gptp_set_event_handler` installs a callback for events from the domain and port.
These include lock-state changes, the beginning and end of a grandmaster change, a master change, a clock reset, sync and announce timeouts, termination of the port service, and interruption of the daemon connection.
The events are enumerated by `enum gptp_event`, and `gptp_event_name` gives the name of each.
The callback runs on a thread owned by the framework and can be called whenever the kernel reports an event.
It should record the event and return, leaving any reporting to the program's own loop.
On macOS 15.7, lock-state and grandmaster-change events arrived within a few tens of milliseconds of a grandmaster change.
`GPTP_EV_SYNC` and `GPTP_EV_AS_CAPABLE` did not arrive.
A program should therefore poll the state as well as handling events.

## Per-sync updates

`gptp_sync_subscribe` delivers a `struct gptp_sync_update` for every sync.
The gPTP profile sends eight syncs a second.
Each update contains the mach time and grandmaster time at that instant, the identities of the grandmaster and port, and the cumulative scaled rate as a fraction of 2 to the 41st.
Both times are in nanoseconds.
This rate changes about once a second.
The updates are evaluations of the kernel's mapping rather than raw sync timestamps, and agree with `gptp_mapping` to about 20 ns.
Updates arrive on a thread owned by the framework and require no run loop.
A connection can have one subscription.
Either `gptp_sync_unsubscribe` or `gptp_close` ends it.

## The other kernel clocks

`gptp_clocks` lists every clock in the kernel.
For each clock, it returns a `struct gptp_clock_info` containing the identifier, kind and lock state.
The list includes the domain, the translation clock and one `GPTP_CLOCK_ADAPTER` for each AVB-capable adapter.
The translation clock maps mach time to the framework's TimeSyncTime and is an identity mapping on Apple silicon.
Each adapter clock is the adapter's own hardware clock, with its own mapping to mach time.
This mapping contains only the cross-timestamp filter, without the gPTP servo in front of it.
`gptp_clock_mapping` and `gptp_clock_domain_from_mach` read any clock by its identifier.
The framework reports the ratio of an adapter clock the other way up from that of the domain.
The library inverts this ratio so that every mapping expresses clock nanoseconds per mach tick.
Comparing the adapter-clock and domain mappings against mach time made it possible to distinguish oscillator wander from the behaviour of the kernel servo.

## Names

`gptp_framework_version` returns the bundle version of the framework.
The version on the machine used to write the library was `1340.13`.
`gptp_lock_state_name` returns "locked" for 2 and the number as a string otherwise, since 2 is the only value whose meaning is known.

## What it does not do

The library does not send anything to chrony or steer any clock.
The programs perform those tasks.
The announce carries the UTC offset and leap flags, but the framework does not expose them to the library.
It cannot reach the raw sync timestamps or the parameters of the kernel's servo.
It was written against TimeSync 1340.13 on macOS 15.7.7 and may need repair for another release.
What was found about the framework is recorded in [The TimeSync framework, as observed](timesync.md).
