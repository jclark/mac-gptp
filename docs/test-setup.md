# Test setup

The test setup uses three machines:

- `abondance.lan` is the gPTP grandmaster.
- `edam.lan` is a linuxptp boundary clock.
- `mimolette` is the Mac under test.

The main LAN is `10.56.65.0/24`.

## Hardware

The timing path uses three computers joined by two direct Ethernet cables.

```
u-blox ZED-F9P PPS
         |
         v
abondance.lan enp4s0 --- 2.5 Gb/s --- eth3 edam.lan eth1 --- 1 Gb/s --- en14 mimolette
       grandmaster                       boundary clock                       gPTP client
```

### abondance.lan

`abondance.lan` is an ASUS D500MD desktop with an Intel Core i5-12400 processor.
Its `enp4s0` interface is an onboard Intel I225-LM with a PTP hardware clock.
The PPS output of an internal u-blox ZED-F9P is wired to the `SDP1` pin of this interface.
There is a 2.5 Gb/s Ethernet cable from `enp4s0` to `eth3` on `edam.lan`.
There is an Ethernet cable from its `enp1s0` interface to a switch on the main LAN.

### edam.lan

`edam.lan` is a generic mini-PC with an Intel N5105 processor and four Intel I226-V Ethernet ports.
Each Ethernet port has a separate PTP hardware clock.
There is an Ethernet cable from `eth3` to `enp4s0` on `abondance.lan` and another from `eth1` to `en14` on `mimolette`.
There is an Ethernet cable from `eth0` to a switch on the main LAN.

### mimolette

`mimolette` is a Mac mini with an Apple M4 Pro processor.
The gPTP interface is `en14`, provided by an Apple Thunderbolt to Gigabit Ethernet Adapter connected through an Apple Thunderbolt 3 to Thunderbolt 2 Adapter.
There is a 1 Gb/s Ethernet cable from this adapter to `eth1` on `edam.lan`.
`system_profiler SPEthernetDataType` reports `AVB Support: Yes` for `en14`.
There is an Ethernet cable from its onboard 10GbE interface `en0` to a switch on the main LAN.

## Software

### abondance.lan

`abondance.lan` runs Debian GNU/Linux 13 with kernel 6.12.90+deb13.1-amd64 and linuxptp 4.2.
The `igc` driver exposes the PTP hardware clock on `enp4s0` as `/dev/ptp0`.
`enp1s0` has address `10.56.65.4/24` on the main LAN and carries the default route.
`enp4s0` has address `192.168.104.0/31` on the direct link to `edam.lan`.
SatPulse reads the ZED-F9P over `/dev/ttyUBX0`, timestamps its PPS in the `enp4s0` PHC and disciplines that PHC directly.
The relevant parts of `/etc/satpulse.toml` are:

```toml
[phc]
interface = "enp4s0"
channel = 0
pin = 1

[serial]
device = "/dev/ttyUBX0"
speed = 115200

[gps]
config = true
vendor = "u-blox"

[ptp]
ptp4l.udsAddress = "/var/run/ptp4l"
domainNumber = 0
majorSdoId = 1
clockAccuracy = 150
```

SatPulse also sends the current UTC offset and clock-quality properties to `ptp4l` through its management socket.
The `majorSdoId` value matches the 802.1AS `transportSpecific` value used by linuxptp.
The system clock and chrony on this machine are not part of the gPTP time path.

The relevant part of `/etc/linuxptp/ptp4l.conf` is:

```ini
[global]
serverOnly 1
gmCapable 1
priority1 100
priority2 248
logAnnounceInterval 0
logSyncInterval -3
syncReceiptTimeout 3
neighborPropDelayThresh 800
min_neighbor_prop_delay -20000000
assume_two_step 1
path_trace_enabled 1
follow_up_info 1
transportSpecific 0x1
ptp_dst_mac 01:80:C2:00:00:0E
network_transport L2
delay_mechanism P2P
time_stamping hardware
uds_address /var/run/ptp4l

[enp4s0]
```

This is the linuxptp gPTP profile with the port forced to the server role.
Priority 100 makes `abondance.lan` win the best-master election against `edam.lan`, whose priority is 200, and the Mac, whose priority is 248.

### edam.lan

`edam.lan` runs Fedora Linux 44 with kernel 7.1.3-201.fc44.x86_64 and linuxptp 4.4.
The `igc` driver exposes a separate PTP hardware clock for each I226-V port.
`eth0` has address `10.56.65.11/24` on the main LAN and carries the default route.
`eth3` has address `192.168.104.1/31` on the direct link to `abondance.lan`.
`eth1` has address `192.168.101.1/30` on the direct link to `mimolette`.
A single `ptp4l` process runs as a JBOD boundary clock because each I226 port has its own PHC.
The relevant part of `/etc/ptp4l.conf` is:

```ini
[global]
gmCapable 1
priority1 200
priority2 248
logAnnounceInterval 0
logSyncInterval -3
syncReceiptTimeout 3
neighborPropDelayThresh 800
min_neighbor_prop_delay -20000000
assume_two_step 1
path_trace_enabled 1
follow_up_info 1
transportSpecific 0x1
ptp_dst_mac 01:80:C2:00:00:0E
network_transport L2
delay_mechanism P2P
time_stamping hardware
boundary_clock_jbod 1
user linuxptp

[eth3]

[eth1]
serverOnly 1
```

`eth3` receives gPTP from `abondance.lan`, and `ptp4l` disciplines its PHC at `/dev/ptp3`.
chrony 4.8 reads this PHC into the system clock using the following refclock line in `/etc/chrony.conf`:

```
refclock PHC /dev/ptp3 tai dpoll 4 precision 1e-7 refid PTP
```

The enabled `phc2sys@eth1` service then drives the `eth1` PHC from the system clock with this command:

```
/usr/sbin/phc2sys -s CLOCK_REALTIME -c eth1 -w -f /etc/ptp4l.conf
```

`ptp4l` sends gPTP from that PHC through its server-only `eth1` port to the Mac.
The transfer deliberately goes from the `eth3` PHC through the system clock to the `eth1` PHC, because PHC-to-system comparisons can use the I226 ports' precise cross timestamps.

NetworkManager marks the `eth1` timing connection as `never-default`.
`dnsmasq` listens only on `eth1` and leases `192.168.101.2/30` to the Mac without router or DNS options.

### mimolette

`mimolette` runs macOS Tahoe 26.6.2, build 25G83, with TimeSync framework version 1460.2.
`en0` has address `10.56.65.28/24` on the main LAN and carries the default route.
The `en14` timing interface receives `192.168.101.2/30` from `edam.lan` by DHCP and has no default route.

chrony 4.9 runs as the `org.chrony-project.chronyd` launchd service and reads `/etc/chrony.d/chrony.conf`.
The relevant configuration is:

```
server ntp.lan iburst xleave minpoll 0 maxpoll 0 noselect
refclock SOCK /var/run/chrony.gptp.sock refid GPTP poll 1 filter 1 precision 3e-7 minsamples 8 maxsamples 12 prefer
corrtimeratio 8
maxupdateskew 0.5
maxslewrate 5
makestep 0.001 3
```

`gptp-refclock` was run by hand under `sudo`, rather than as a launchd service:

```
sudo gptp-refclock --interval 0.25 en14
```
