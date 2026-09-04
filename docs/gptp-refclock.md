# gptp-refclock

gptp-refclock is a chrony reference clock driven by the Mac's gPTP clock.
At each interval it reads the system clock between two readings of mach absolute time.
It converts the corresponding mach time to grandmaster time through the kernel's mapping, then subtracts the UTC offset.
The difference is sent to chrony as a complete SOCK sample.
chrony uses these samples to steer the system clock towards the grandmaster.

## Running

    sudo gptp-refclock en14

Root privileges are needed only because chrony creates its socket with permissions that allow only root to write to it.
Without root privileges, the program still runs but cannot deliver samples to chrony; it reports this once.
The program adds a gPTP port on the interface, or uses a port that another process has already added there.
The kernel removes a port added by the program when the program exits, even if it crashes.
If the owner of an adopted port exits, the refclock retries until it can add or adopt the port again.
It waits `--port-retry` seconds between attempts.

Tuning chrony for the best results is subtle.
The following configuration gave the best result in these tests:

    refclock SOCK /var/run/chrony.gptp.sock refid GPTP poll 1 filter 1 precision 3e-7 minsamples 8 maxsamples 12 prefer
    corrtimeratio 8
    maxupdateskew 0.5
    maxslewrate 5
    makestep 0.001 3

chrony creates the socket at startup, so restart it after adding the refclock line.
Mark any NTP servers in the configuration as `noselect`.
They will remain visible for comparison but will not steer the clock.

These settings are needed because the Mac's oscillator wanders by several hundred ppb over a few minutes.
`minsamples` and `maxsamples` limit chrony's frequency regression to a span of 14 to 22 s, which is short enough to follow the wander rather than lag behind it by microseconds.
`corrtimeratio 8` gives the phase correction a timescale of 16 s.
`filter 1` keeps only the newest sample received during each two-second poll, since faster readings of the same kernel mapping are not independent measurements.
Running the refclock with `--interval 0.25` is a separate tweak that sends four samples a second, giving chrony a fresher sample when each poll runs.
`maxupdateskew` prevents chrony from applying a frequency estimate that is too uncertain.
With chrony's default settings and `poll 3`, the residual was 1.7 us RMS, and chrony lagged the wander by up to 5 us.
With the default settings and `poll 0`, the loop oscillated with a growing swing.
With the settings above and `--interval 0.25`, the residual over half an hour was 0.6 us RMS, with excursions of about 2 us during the fastest wander.

## What it reports

Events are written to stderr, each with its UTC date and time.
At startup, the program reports the port and grandmaster.
It then reports changes of state, events from the framework, and changes in the availability of chrony.
A launchd plist can pass `--os-log` to send these events to the unified log instead.
Each unified-log message contains the UTC time at which the event occurred, which can precede the log entry by up to one sampling interval.
They use the subsystem `com.jclark.gptp` and the category `gptp-refclock`:

    log show --predicate 'subsystem == "com.jclark.gptp"'

The state is determined by a sequence of checks, and the first check that fails gives the state its name:

- `noport`: there is no gPTP port on the interface.
- `notcapable`: the port has no gPTP peer, or the peer-delay measurement has not passed.
- `owngm`: the Mac is its own grandmaster.
  The Mac announces itself with priority1 248 and clock class 248, so any real grandmaster wins the election.
- `unlocked`: the domain has chosen the external grandmaster, but its servo has not yet locked.
- `notptp`: the domain is locked, but the conversion does not report the PTP timescale from that grandmaster.
- `settling`: the domain is locked; samples will be sent once `--settle` seconds (5) have passed without disturbance.
- `locked`: samples are being sent.

A loss of lock, grandmaster change, clock reset or timeout moves the state back and restarts the settling period.
Unplugging the cable takes the port out of the asCapable state and returns the domain to the Mac's own clock.
The refclock then stops sending samples.

## The measurement file

`--log PATH` writes one record per sample.
By default, records contain space-separated columns, with their names written on a `#` line when the file is opened.
With `-j`, each record is a JSON object on one line.
Without `--log` nothing is recorded.
If a write fails after the file has been opened, the program reports the error once and continues without measurement logging.
The code generates both formats from the same table of columns.
Each record contains:

- the mach time of the sample, the width of its bracket, the system time and the grandmaster time, and their difference `offset_ns`, which is gPTP-derived UTC minus the system clock;
- the state, the lock state, the Mac's identity and the grandmaster's, the timescale and traceability flags reported by the conversion, and whether the sample was sent;
- for the port: asCapable, role, the peer, the propagation delay and its extremes, and the timestamping modes;
- for the mapping: the rate ratio, both as a fraction and as `rate_ppb` relative to the mach timebase, and the two anchors;
- counts: syncs seen since the previous record, grandmaster changes, and the port's measurement counters.

Use `offset_ns` to assess how well chrony is tracking the grandmaster.
Its RMS over a run is the error of the system clock relative to the grandmaster, as seen by the refclock.
The RMS that chrony itself reports is an output of chrony's model and can be smaller than the physical residual.
Changes in `rate_ppb` between records show the oscillator's frequency wander.
The residual phase after fitting the anchors to a straight line shows how that wander develops over time.
Comparison with the adapter clock showed that the wander came from the oscillator rather than the kernel's servo.

## Options

- `INTERFACE`: the interface on which to add the gPTP port.
- `--sock PATH`: chrony's socket (`/var/run/chrony.gptp.sock`).
- `--interval S`: seconds between samples (1).
- `--port-retry S`: seconds between attempts to restore a missing port (1).
- `--utc-offset N`: TAI minus UTC in seconds (37, correct since 2017).
  The announce carries this value, but the framework does not expose it.
  It must therefore be configured explicitly.
  Every sample goes to chrony with the leap flag clear; chrony takes leap seconds from its NTP sources.
- `--settle S`: how long the domain must be locked and undisturbed before samples are sent (5).
- `--brackets N` and `--max-bracket US`: each sample reads mach time on both sides of the system clock reading, N times (4), and keeps the narrowest pair.
  A bracket wider than the limit (5 us) indicates that the thread was preempted.
  The sample is recorded but not sent.
  Mach time ticks at 24 MHz on Apple silicon, so a bracket reads as either 0 or 41 ns.
- `--timeout S`: how long to wait for the TimeSync daemon (2).
- `--log PATH`: the measurement file; `-j` makes it JSON lines.
- `--os-log`: send events to the unified log instead of stderr.

## What the runs showed

The tests used a Linux boundary clock with a direct gigabit link to the Mac and a direct 2.5-gigabit link to the grandmaster.
The grandmaster's port clock was held within 10 ns of GPS-derived time.
The hardware, network and PTP configuration is recorded in the [test setup](test-setup.md).
The refclock also runs without code changes on macOS Tahoe 26.6.2 with TimeSync 1460.2.

- The measured propagation delay was 40 to 42 ns, with a raw spread of 10 ns.
  The 10 ns spread is consistent with hardware timestamps accurate to a few nanoseconds.
- The port became asCapable 2 s after the grandmaster appeared, the domain locked within the following second, and samples were flowing 9 s after start.
- The kernel's mapping is continuous in phase and follows the oscillator.
  Over ten minutes, its phase relative to a straight line moved by tens of microseconds, and the adapter's own crystal, read through its own mapping, moved relative to mach time in the same way.
  The matching shapes identify the Mac's oscillator as the source of the wander.
  Its frequency changes by several hundred ppb over a few minutes, and the mapping tracks the change.
- A GPS pulse detected by polling a serial port's CTS pin stayed a steady few microseconds after the gPTP second, within the resolution of the polling method.
  [gptp-pps-offset](gptp-pps-offset.md) measures this offset.
- Before the switch to gPTP, the system clock was 2 ms fast while synchronized by NTP over the internet.
