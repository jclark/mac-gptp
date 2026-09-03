# gptp-pps-offset

    gptp-pps-offset -t 3600 en14 /dev/cu.usbserial-3110

gptp-pps-offset uses the Mac's gPTP clock to measure the latency of detecting a GPS pulse by polling a serial port's CTS pin.
It prints the value to use for chrony's `offset` option on the line for that refclock.

## The serial PPS path

Connecting the pulse-per-second output of a GPS receiver to the CTS pin of a USB serial adapter lets software timestamp the pulse on a system without kernel PPS support.
The software polls `ioctl(TIOCMGET)` repeatedly around the expected time of the second.
It timestamps the edge midway between the last poll that saw the pin asserted and the first that saw it clear.
SatPulse's serial mode uses this method.
The timestamp is late by the combined delays from sampling the pin in the adapter, transferring its state over USB and returning from the system call.
chrony compensates for this fixed latency with the `offset` option on the refclock line.
Before this measurement, the latency on my Mac was assumed to be 20 us.

## What the program does

gptp-pps-offset uses the same polling method and takes the same exclusive lock on the port.
It timestamps each edge in mach absolute time and converts the timestamp through the kernel's gPTP mapping.
The gPTP mapping locates the UTC second to well under a microsecond, and neither chrony nor the system clock plays any part in the measurement.
The program predicts each second from the mapping rather than from the previous pulse.
It subtracts the UTC offset from the current grandmaster time, rounds up to the next whole second and converts the result back to a mach deadline.
It then polls the pin in a window extending `--window` (100 ms) either side of that instant.
A wide polling bracket indicates that the process was preempted, making its midpoint an unreliable timestamp for the pulse.
The program therefore rejects an edge whose bracket half-width exceeds `--max-uncertainty` (150 us).
The collection period defaults to 600 seconds and can be changed with `-t`.
At the end of the run, the program prints one value to stdout: the median position of the edge after the gPTP second, in seconds.
It writes the count, quartiles, minimum and median uncertainty to stderr, with a progress line every `--report` seconds.
`--log PATH` writes one record for each edge.

The result has the sign expected by chrony.
A late pulse makes the refclock samples too negative by the amount of the latency.
Since chrony adds `offset` to each sample, the printed value can be used directly.

## Precision

Polling locates each edge to within half the width of its bracket, about 45 us on a quiet Mac.
The median has a precision of about 3 us after ten minutes and about 1.5 us after an hour.
The delay is one-sided, so the median represents the typical latency and the minimum represents its lower bound.
Below a microsecond the result is limited by the mapping's own fixed offset, which nothing on the Mac can measure.

The program needs no privileges.
If the refclock is running it uses the refclock's gPTP port; otherwise it adds a port of its own for the duration of the run.
While the domain is not locked to an external grandmaster, the program waits and reports why.

## Options

- `-t SECONDS`: how long to collect edges (600).
- `--window US`: how far either side of the predicted second to poll (100000).
- `--max-uncertainty US`: reject an edge whose bracket half-width exceeds this (150).
- `--utc-offset N`: TAI minus UTC (37), needed to locate the UTC second in gPTP time.
- `--report SECONDS`: interval between progress lines on stderr (60).
- `--log PATH`: write a record for each edge.
  With `-j`, each record uses the same JSON format as a timestamp event from `satpulsetool sdp -j`.
  `timestamp` gives the edge time on the gPTP clock as TAI seconds and nanoseconds, while `tRead` gives the system time of the read that detected it.
  The `uncertainty` field gives the bracket half-width in seconds, as it does for edges from SatPulse's serial mode.
  The fractional part of `timestamp` is the position of the edge after the second.
  Without `-j`, the same fields are written as space-separated columns, with the date and time of the read in UTC first, under a `#` line giving the column names.
- `--timeout SECONDS`: how long to wait for the TimeSync daemon (2).
- `--debug`: write a line for each edge to stderr.

## What it found

The same measurement was originally part of the refclock.
On a Mac mini M4 with an FT232H, a 26-minute run gave five-minute medians between +0.7 and +12.8 us, with a mean of about +7 us.
The measured latency is about a third of the 20 us previously assumed.
