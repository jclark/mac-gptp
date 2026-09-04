# The TimeSync framework, as observed

`/System/Library/PrivateFrameworks/TimeSync.framework` is the user-space interface to IOTimeSyncFamily, the kernel's IEEE 802.1AS (gPTP) implementation.
The framework is private and undocumented.
This description is based on inspection of its classes with the Objective-C runtime and experiments on a Mac mini M4 running macOS 15.7.7 with TimeSync 1340.13.
The classes and selectors used by the library remain available on the same Mac running macOS Tahoe 26.6.2 with TimeSync 1460.2.
`tsdump` was used to list the methods of each class and their type encodings.
[libgptp](libgptp.md) exposes the parts of the framework used by this repository as a C interface.

## The objects

The IOTimeSyncFamily driver creates a hierarchy of kernel objects in the I/O Registry.
The hierarchy begins with IOTimeSyncClockManager, whose children are the mach translation clock and IOTimeSyncgPTPManager.
IOTimeSyncgPTPManager has one IOTimeSyncDomain, containing a local clock port and one Ethernet port for each interface participating in gPTP.
IOTimeSyncClockManager also has one kernel clock for each AVB-capable adapter.
`avbdiagnose --no-coreaudio --no-info-tree` prints this hierarchy together with the properties of each object, the announce fields and the packet counters.
It also shows whether a port added by these programs has appeared.

Each framework class is a user-space proxy for one kind of kernel object.
The corresponding classes are `TSClockManager`, `TSgPTPManager`, `TSgPTPClock` for the domain, `TSgPTPEthernetPort` for a port, and `TSKernelClock` for an adapter clock.
Each proxy communicates with its kernel object through an IOKit user client.
Reading a property or requesting a conversion therefore makes a call into the kernel.
The framework computes no time of its own.

## The mapping

Nothing on the Mac steers a hardware clock.
The adapter clock and mach absolute time both run freely.
The kernel estimates the relationship between grandmaster time and mach time from sync messages and cross-timestamps.
The adapter clock timestamps each sync when it arrives, and the corresponding follow-up message supplies the grandmaster time.
Cross-timestamps relate the adapter clock to mach time.
Together these measurements produce an affine mapping from mach time to domain time.
The mapping consists of a domain anchor plus a rate ratio times the number of mach ticks since a mach anchor.
The kernel updates the mapping at every sync, re-estimating the rate about once a second while keeping the phase continuous, so the mapping bends but never steps.
Every conversion that the framework offers is an evaluation of this mapping.

## What was found

- Adding a link-layer gPTP port with `addLinkLayerPortOnInterfaceNamed:allocatedPortNumber:error:` needs no privilege.
  The port reports `localTimestampingMode` 1, meaning hardware, and a propagation delay limit of 1000 ns.
  The kernel removes the port when the process that added it exits, so a crashed run leaves nothing behind.
  Another process can see the port and use the domain through it.
  An attempt to remove a port owned by another process fails with IOErrorDomain -536870160.
  The library therefore removes a port only if its own process added it.
- `ports` returns freshly created wrapper objects on every call, so a port must be looked up by `interfaceName` each time rather than cached.
  The port object on which `addClient:` was called is kept, since the callbacks come through it.
- The delegate selectors are those of the `TSClockClient` and `TSgPTPNetworkPortClient` protocols, all optional, with the clock or port as the last argument.
  Callbacks for lock state, grandmaster change and master change arrived on a framework thread within a few tens of milliseconds of a grandmaster change.
  `didProcessSync:` and the port's asCapable callback were not observed.
  The programs therefore poll the state at every interval and use events only to restart their settling period.
- `getMachAbsoluteRateRatioNumerator:denominator:machAnchor:andDomainAnchor:withError:` returns the affine mapping.
  When the Mac is its own grandmaster the ratio is exactly the mach timebase, and the anchors move once a second.
  Conversions take mach ticks, not nanoseconds.
- `gPTPTimeFromMachAbsoluteTime:` reports `isPTPTimescale` as YES even when the Mac is its own grandmaster.
  Programs must also check that the grandmaster identity differs from the Mac's own before treating the domain as externally synchronized.
- The `lockState` of the domain is 2 when it is locked, whether to itself or to an external grandmaster.
  It drops to 1 briefly during a grandmaster change.
- The `clockClass` and `clockAccuracy` of the domain read as 0 through the framework, although avbdiagnose shows real values for the announce.
- The UTC offset and leap flags of the announce are not accessible through the framework.
  `TSTime` uses the framework's `tai_utc_history.plist` leap-second table rather than the announce.
  Its `initWithgPTPTime:` method crashed during a probe and is not used.
  The programs take a fixed `--utc-offset` instead.
- `getMetrics` returns `TSClockMetrics` for the domain and `TSPortMetrics` for the port.
  Their values stayed at zero even when locked, apart from `gmChangesCount`.
  The packet counters of the port appear in the output of avbdiagnose, not in these objects.
- `availableClockIdentifiers` lists, besides the domain and the mach translation clock, one kernel clock for each AVB-capable adapter.
  Each is the adapter's own hardware clock, with its own mapping to mach time.
  The mapping contains only the cross-timestamp filter, without the gPTP servo in front of it.
  Adapter mappings report mach time per adapter tick, the inverse of the ratio used by the domain.
  The library normalizes them to use the same direction as the domain mapping.
- The private per-sync callback delivers `updateWithSyncInfoValid:...` at the sync rate on a framework thread, without requiring a run loop.
  Each update contains mach time and grandmaster TAI in nanoseconds.
  Registering the callback starts by obtaining `sharedClockSyncManager` from `_TSF_TSDClockSyncManager` and calling `clockSyncForClockIdentifier:pid:`.
  Pass `addUpdateClient:` an object whose class declares the `TSDClockSyncPTPSyncClient`, `TSDClockSyncGeneralSyncClient` and `TSDKernelClockClient` protocols.
  Then call `registerAsyncCallback`.
  A class that declares only the PTP protocol receives nothing.
  The delivered pairs agree with the mapping to about 20 ns and are smooth to a few nanoseconds.
  They are therefore the output of the servo at each sync rather than raw timestamps.
  The raw sync timestamps are not accessible from user space.
