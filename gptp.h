/* SPDX-License-Identifier: MIT */
/* libgptp: the Mac's gPTP clock from C.
 *
 * macOS keeps a full IEEE 802.1AS implementation in the kernel, and a
 * mapping from mach absolute time to grandmaster time that it disciplines
 * from a grandmaster on an AVB-capable Ethernet port with hardware
 * timestamps.  The private TimeSync framework exposes that mapping to user
 * space; this library reaches the framework through dlopen and the
 * Objective-C runtime and presents it as plain C, so that a program
 * written in C can add a gPTP port, watch the domain lock, and convert
 * any mach time to grandmaster time.
 *
 * Everything the framework gives is on the far side of the kernel's servo:
 * the mapping, its rate and anchors, and per-sync updates of the same
 * mapping.  Raw sync timestamps are not reachable.  Times are TAI
 * nanoseconds since the PTP epoch once the domain follows a real
 * grandmaster, and nanoseconds since boot while the Mac is its own.  The
 * library speaks TAI; the caller applies the UTC offset, since the
 * framework has no trustworthy view of it.
 *
 * gptp_open resolves every class and selector it will use and fails with
 * the name of anything missing, so a macOS release that changes the
 * framework breaks this library loudly at start-up, not a program later.
 * Written against TimeSync 1340.13 on macOS 15.7.7.  Nothing here needs a
 * privilege or an entitlement. */
#ifndef GPTP_H
#define GPTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gptp gptp_t;		/* one connection to the TimeSync daemon */

/* The domain clock's lockState: 2 was verified as locked, to itself or to
 * an external grandmaster; 1 was seen briefly during a grandmaster change. */
#define GPTP_LOCK_LOCKED 2

struct gptp_domain_state {
	int lock_state;
	uint64_t clock_identity;		/* the Mac's own gPTP clock identity */
	uint64_t grandmaster_identity;		/* equal to clock_identity when the Mac is its own */
	unsigned clock_class, clock_accuracy, priority1, priority2;	/* read as 0 through the framework */
};

struct gptp_port_state {
	bool present;				/* a gPTP port exists on the interface */
	bool enabled, as_capable;		/* asCapable: 802.1AS accepted the link */
	uint16_t port_number, remote_port_number;
	int port_role;				/* 3 seen while following a grandmaster */
	uint32_t propagation_delay_ns, propagation_delay_limit_ns;
	uint32_t propagation_delay_min_ns, propagation_delay_max_ns;
	uint64_t remote_clock_identity;
	unsigned local_timestamping_mode, remote_timestamping_mode;	/* 1: hardware */
};

/* The affine mapping from mach absolute time (ticks) to a clock's time (ns):
 * domain = domain_anchor + (mach - mach_anchor) * ratio_num / ratio_den.
 * With the Mac as its own grandmaster the ratio is the mach timebase.  The
 * kernel keeps it phase-continuous: a new anchor lies on the old line. */
struct gptp_mapping {
	uint64_t ratio_num, ratio_den, mach_anchor, domain_anchor;
};

struct gptp_time {
	uint64_t seconds;
	uint32_t nanoseconds;
	uint64_t grandmaster_identity;		/* the grandmaster the conversion used */
	bool ptp_timescale;			/* YES even when the Mac is its own grandmaster */
	bool time_traceable, frequency_traceable;
};

struct gptp_metrics {
	uint64_t time_to_lock, gm_changes;
	uint64_t port_total, port_successful, port_dropped, port_discarded;
	uint64_t port_sync_timeouts, port_mean_delay;
};

/* One per-sync update of the mapping, delivered at the sync rate (8 per
 * second in the gPTP profile).  mach_ns is mach absolute time in
 * nanoseconds, domain_ns the grandmaster's time at that instant, and
 * cumulative_scaled_rate the rate as a fraction of 2^41, updated about once
 * a second.  These agree with gptp_mapping to about 20 ns. */
struct gptp_sync_update {
	bool valid;
	unsigned flags;
	uint64_t mach_ns, domain_ns;
	uint64_t cumulative_scaled_rate, inverse_cumulative_scaled_rate;
	uint64_t grandmaster_identity;
	uint16_t local_port;
};

enum gptp_clock_kind {
	GPTP_CLOCK_DOMAIN,		/* the gPTP domain: grandmaster time */
	GPTP_CLOCK_TRANSLATION,		/* mach time to the framework's TimeSyncTime; the identity on Apple silicon */
	GPTP_CLOCK_ADAPTER,		/* an AVB-capable adapter's own hardware clock, mapped to mach time */
	GPTP_CLOCK_OTHER,
};

struct gptp_clock_info {
	uint64_t identifier;
	enum gptp_clock_kind kind;
	int lock_state;
};

enum gptp_event {
	GPTP_EV_LOCK_STATE,		/* arg: the new lockState */
	GPTP_EV_GM_CHANGE_BEGIN,	/* arg: the new grandmaster identity, 0 if not given */
	GPTP_EV_GM_CHANGE_END,
	GPTP_EV_MASTER_CHANGED,
	GPTP_EV_CLOCK_RESET,
	GPTP_EV_SYNC,			/* never seen on macOS 15.7; use gptp_sync_subscribe */
	GPTP_EV_AS_CAPABLE,		/* arg: 1 or 0; never seen on macOS 15.7 */
	GPTP_EV_SYNC_TIMEOUT,
	GPTP_EV_ANNOUNCE_TIMEOUT,
	GPTP_EV_PORT_TERMINATED,
	GPTP_EV_INTERRUPTED,		/* the daemon connection was interrupted */
};

/* Callbacks arrive on the framework's own thread, not the caller's. */
typedef void (*gptp_event_fn)(void *ctx, enum gptp_event ev, int64_t arg);
typedef void (*gptp_sync_fn)(void *ctx, const struct gptp_sync_update *u);

/* Connect to the TimeSync daemon, waiting up to timeout_ns, and resolve
 * every class and selector used.  NULL on failure, with the reason in err. */
gptp_t *gptp_open(uint64_t timeout_ns, char *err, size_t errlen);
void gptp_close(gptp_t *g);		/* removes the port if this process added it */
const char *gptp_framework_version(gptp_t *g);

/* Events from the domain clock, the port and the daemon connection. */
void gptp_set_event_handler(gptp_t *g, gptp_event_fn fn, void *ctx);

/* Add a link-layer gPTP port on the interface, or adopt one another process
 * has there, and register for its events.  The kernel removes a port when
 * the process that added it exits. */
bool gptp_port_add(gptp_t *g, const char *ifname, char *err, size_t errlen);
bool gptp_port_remove(gptp_t *g, char *err, size_t errlen);
void gptp_port_state(gptp_t *g, struct gptp_port_state *st);	/* present = false if no port */

/* The domain. */
void gptp_domain_state(gptp_t *g, struct gptp_domain_state *st);
bool gptp_mapping(gptp_t *g, struct gptp_mapping *m, char *err, size_t errlen);
bool gptp_metrics(gptp_t *g, struct gptp_metrics *m);

/* Conversions through the kernel's mapping; mach values are in ticks. */
bool gptp_time_from_mach(gptp_t *g, uint64_t mach, struct gptp_time *t);
uint64_t gptp_domain_from_mach(gptp_t *g, uint64_t mach, uint64_t *grandmaster_used);
uint64_t gptp_mach_from_domain(gptp_t *g, uint64_t domain_ns);

/* mach ticks to nanoseconds and back, from mach_timebase_info. */
int64_t gptp_mach_to_ns(int64_t ticks);
uint64_t gptp_ns_to_mach(int64_t ns);

/* Per-sync updates, on a thread the framework owns; no run loop is needed.
 * One subscription per connection. */
bool gptp_sync_subscribe(gptp_t *g, gptp_sync_fn fn, void *ctx, char *err, size_t errlen);
void gptp_sync_unsubscribe(gptp_t *g);

/* Every clock the kernel has, and the mapping of any of them.  Adapter
 * clocks report their ratio inverted in the framework; here every mapping
 * means clock nanoseconds per mach tick, the same as the domain's. */
size_t gptp_clocks(gptp_t *g, struct gptp_clock_info *clocks, size_t max);
bool gptp_clock_mapping(gptp_t *g, uint64_t identifier, struct gptp_mapping *m, char *err, size_t errlen);
uint64_t gptp_clock_domain_from_mach(gptp_t *g, uint64_t identifier, uint64_t mach);

const char *gptp_event_name(enum gptp_event ev);
const char *gptp_lock_state_name(int lock_state);	/* "locked" for 2, otherwise the number */

#endif
