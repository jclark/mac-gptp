# gptp-pps-offset

gptp-pps-offset compares the Mac's gPTP time with the PPS output of a GPS module received on the CTS pin of a USB serial adapter.
The comparison has two uses.

First, it is a sanity check on gPTP.
A stable PPS edge close to a gPTP second shows that the Mac's mapping from mach time to grandmaster time is behaving plausibly.

Second, if Apple's gPTP implementation is trusted, the comparison estimates the bias in PPS timestamps made by polling the serial adapter.
The program prints a value suitable for chrony's `offset` option when the same adapter is used in the [SatPulse serial PPS setup for chrony](https://satpulse.net/setup/ntp.html).

## Hardware

The Mac's gPTP interface and path to the grandmaster must meet the [gPTP hardware requirements](../README.md#hardware-requirements).
The measurement needs a GPS module with a PPS output and a USB serial adapter based on an FTDI chip.
The adapter must expose its RTS and CTS handshake pins as well as VCC and GND.
The GPS module and adapter are joined by three wires: VCC to VCC, GND to GND and the PPS output to CTS.

## Why the offset is needed

Connecting the pulse-per-second output of a GPS receiver to the CTS pin of a USB serial adapter lets software timestamp the pulse on a system without kernel PPS support.
On macOS, SatPulse detects this signal by polling `ioctl(TIOCMGET)` repeatedly around the expected time of the second.
It timestamps the edge midway between the last poll that saw the pin asserted and the first that saw it clear.
Sampling the pin in the adapter, transferring its state over USB and returning from the system call bias the timestamp later than the electrical edge.
chrony compensates for this measurement bias with the `offset` option on the refclock line.

## What the program does

gptp-pps-offset takes an exclusive lock on the serial device and uses the same polling method as SatPulse.
Neither chrony nor the system clock plays any part in the measurement.
The program predicts each UTC second from the gPTP mapping rather than from the previous pulse.
It subtracts the UTC offset from the current grandmaster time, rounds up to the next whole second and converts the result back to a mach deadline.
It then polls the pin in a window extending `--window` (100 ms) either side of that instant.
The start of a chosen window must be at least `--window-margin` (50 ms) in the future.
This leaves time to convert the domain-time bounds to mach deadlines and sleep until polling begins.
The program timestamps an edge in mach absolute time at the midpoint between the last asserted reading and the first clear reading, then converts that timestamp through the gPTP mapping.
A wide polling bracket indicates that the process was preempted, making its midpoint an unreliable timestamp for the pulse.
The program therefore rejects an edge whose bracket half-width exceeds `--max-uncertainty` (150 us).
The collection period defaults to 600 seconds and can be changed with `-t`.
At the end of the run, the program prints one value to stdout: the median position of the edge after the gPTP second, in seconds.
It writes the count, quartiles, minimum and median uncertainty to stderr, with a progress line every `--report` seconds.
`--log PATH` writes one record for each edge.

The result has the sign expected by chrony.
A positive bias in the detected edge time makes the refclock samples too negative by the same amount.
Since chrony adds `offset` to each sample, the printed value can be used directly.

## Precision

Polling locates each edge to within half the width of its bracket, about 45 us on a quiet Mac.
The median has a precision of about 3 us after ten minutes and about 1.5 us after an hour.
The delays in the serial path are one-sided, so the median estimates the typical measurement bias and the minimum gives a lower bound.
Below a microsecond the result is limited by the mapping's own fixed offset, which nothing on the Mac can measure.

## Running

    gptp-pps-offset en14 /dev/cu.usbserial-3110

The program needs no privileges.
If [gptp-refclock](gptp-refclock.md) is running, the program uses its gPTP port; otherwise it adds a port of its own for the duration of the run.
If the owner of an adopted port exits, the program retries until it can add or adopt the port again.
While the domain is not locked to an external grandmaster, the program waits and reports why.

## Command line

    gptp-pps-offset [OPTION]... INTERFACE DEVICE

`INTERFACE` names the interface used for gPTP.
`DEVICE` names the serial device whose CTS pin carries the GPS PPS signal.
`OPTION` can be any of the following:

- `-t SECONDS`: how long to collect edges (600).
- `--window US`: how far either side of the predicted second to poll (100000).
- `--window-margin US`: how far in the future the start of a newly chosen polling window must be (50000).
- `--max-uncertainty US`: reject an edge whose bracket half-width exceeds this (150).
- `--utc-offset N`: TAI minus UTC (37), needed to locate the UTC second in gPTP time.
- `--report SECONDS`: interval between progress lines on stderr (60).
- `--state-interval S`: interval between state checks while the port or domain is not ready (0.5).
- `--log PATH`: write a record for each edge.
  With `-j`, each record uses the same JSON format as a timestamp event from `satpulsetool sdp -j`.
  `timestamp` gives the edge time on the gPTP clock as TAI seconds and nanoseconds, while `tRead` gives the system time of the read that detected it.
  The `uncertainty` field gives the bracket half-width in seconds, as it does for edges from SatPulse's serial mode.
  The fractional part of `timestamp` is the position of the edge after the second.
  Without `-j`, the same fields are written as space-separated columns, with the date and time of the read in UTC first, under a `#` line giving the column names.
- `--timeout SECONDS`: how long to wait for the TimeSync daemon (2).
- `--debug`: write a line for each edge to stderr.

## What it found

On a Mac mini M4 with an FT232H, the five-minute median estimates in a 26-minute run ranged from +0.7 to +12.8 us and averaged about +7 us.
