/* SPDX-License-Identifier: MIT */
/* libgptp: the private TimeSync framework, reached through dlopen and the
 * Objective-C runtime; no header for it exists.  Classes and selectors are
 * resolved once in gptp_open and checked with respondsToSelector, so a
 * renamed method fails at start-up with its name, not with a crash later.
 * The selectors and their type encodings were read from the framework on
 * macOS 15.7.7 (TimeSync 1340.13); probe/tsdump.m lists them. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "gptp.h"

#define FRAMEWORK_DIR "/System/Library/PrivateFrameworks/TimeSync.framework"
#define FRAMEWORK FRAMEWORK_DIR "/TimeSync"

/* The delegate selectors, from the TSClockClient and TSgPTPNetworkPortClient
 * protocols; all optional, and the framework sends whichever the client
 * implements.  Declared here so the compiler accepts the methods. */
@protocol GPTPClockClient <NSObject>
@optional
- (void)didChangeLockStateTo:(int)state forClock:(id)clock;
- (void)didBeginClockGrandmasterChangeForClock:(id)clock;
- (void)didBeginClockGrandmasterChangeWithGrandmasterID:(uint64_t)gm localPort:(uint16_t)port forClock:(id)clock;
- (void)didEndClockGrandmasterChangeForClock:(id)clock;
- (void)didEndClockGrandmasterChangeWithGrandmasterID:(uint64_t)gm localPort:(uint16_t)port forClock:(id)clock;
- (void)didChangeClockMasterForClock:(id)clock;
- (void)didChangeLocalPortWithGrandmasterID:(uint64_t)gm localPort:(uint16_t)port forClock:(id)clock;
- (void)didResetClock:(id)clock;
- (void)didProcessSync:(id)clock;
- (void)didChangeASCapable:(BOOL)capable forPort:(id)port;
- (void)didChangeAdministrativeEnable:(BOOL)enabled forPort:(id)port;
- (void)didSyncTimeoutForPort:(id)port;
- (void)didAnnounceTimeoutForPort:(id)port;
- (void)didTerminateServiceForPort:(id)port;
- (void)interruptedConnectionForClockManager:(id)manager;
@end

@interface GPTPClient : NSObject <GPTPClockClient>
@property (nonatomic) gptp_event_fn fn;
@property (nonatomic) void *ctx;
@end

@implementation GPTPClient
- (void)emit:(enum gptp_event)ev arg:(int64_t)arg { if (_fn) _fn(_ctx, ev, arg); }
- (void)didChangeLockStateTo:(int)state forClock:(id)clock { [self emit:GPTP_EV_LOCK_STATE arg:state]; }
- (void)didBeginClockGrandmasterChangeForClock:(id)clock { [self emit:GPTP_EV_GM_CHANGE_BEGIN arg:0]; }
- (void)didBeginClockGrandmasterChangeWithGrandmasterID:(uint64_t)gm localPort:(uint16_t)port forClock:(id)clock { [self emit:GPTP_EV_GM_CHANGE_BEGIN arg:(int64_t)gm]; }
- (void)didEndClockGrandmasterChangeForClock:(id)clock { [self emit:GPTP_EV_GM_CHANGE_END arg:0]; }
- (void)didEndClockGrandmasterChangeWithGrandmasterID:(uint64_t)gm localPort:(uint16_t)port forClock:(id)clock { [self emit:GPTP_EV_GM_CHANGE_END arg:(int64_t)gm]; }
- (void)didChangeClockMasterForClock:(id)clock { [self emit:GPTP_EV_MASTER_CHANGED arg:0]; }
- (void)didChangeLocalPortWithGrandmasterID:(uint64_t)gm localPort:(uint16_t)port forClock:(id)clock { [self emit:GPTP_EV_MASTER_CHANGED arg:(int64_t)gm]; }
- (void)didResetClock:(id)clock { [self emit:GPTP_EV_CLOCK_RESET arg:0]; }
- (void)didProcessSync:(id)clock { [self emit:GPTP_EV_SYNC arg:0]; }
- (void)didChangeASCapable:(BOOL)capable forPort:(id)port { [self emit:GPTP_EV_AS_CAPABLE arg:capable ? 1 : 0]; }
- (void)didSyncTimeoutForPort:(id)port { [self emit:GPTP_EV_SYNC_TIMEOUT arg:0]; }
- (void)didAnnounceTimeoutForPort:(id)port { [self emit:GPTP_EV_ANNOUNCE_TIMEOUT arg:0]; }
- (void)didTerminateServiceForPort:(id)port { [self emit:GPTP_EV_PORT_TERMINATED arg:0]; }
- (void)interruptedConnectionForClockManager:(id)manager { [self emit:GPTP_EV_INTERRUPTED arg:0]; }
@end

/* The per-sync update client.  The framework's dispatcher checks that the
 * receiver conforms to its protocols, so they are added to this class at
 * run time, after the framework is loaded; declaring protocols of the same
 * names here would shadow the framework's.  Updates arrive on a thread the
 * framework owns; no run loop is involved. */
@interface GPTPSyncClient : NSObject
@property (nonatomic) gptp_sync_fn fn;
@property (nonatomic) void *ctx;
@end

@implementation GPTPSyncClient
- (void)updateWithSyncInfoValid:(BOOL)valid syncFlags:(unsigned char)flags timeSyncTime:(uint64_t)mach_ns
		    domainTimeHi:(uint64_t)hi domainTimeLo:(uint64_t)lo cumulativeScaledRate:(uint64_t)csr
     inverseCumulativeScaledRate:(uint64_t)icsr grandmasterID:(uint64_t)gm localPortNumber:(uint16_t)port
{
	(void)hi;
	struct gptp_sync_update u = {
		.valid = valid, .flags = flags, .mach_ns = mach_ns, .domain_ns = lo,
		.cumulative_scaled_rate = csr, .inverse_cumulative_scaled_rate = icsr,
		.grandmaster_identity = gm, .local_port = port,
	};
	if (_fn) _fn(_ctx, &u);
}
@end

struct gptp {
	id clock_manager, gptp_manager, domain;
	id port;			/* the port registered on, or nil */
	NSString *ifname;
	bool added_port;
	GPTPClient *client;
	id clock_sync;			/* the daemon connection for per-sync updates, or nil */
	GPTPSyncClient *sync_client;
	char version[32];
};

static mach_timebase_info_data_t timebase;

/* objc_msgSend under the signatures used here */
typedef id (*msg_id)(id, SEL);
typedef id (*msg_id_u64)(id, SEL, uint64_t);
typedef id (*msg_id_u64_int)(id, SEL, uint64_t, int);
typedef uint64_t (*msg_u64)(id, SEL);
typedef uint64_t (*msg_u64_u64)(id, SEL, uint64_t);
typedef uint64_t (*msg_u64_u64_pu64)(id, SEL, uint64_t, uint64_t *);
typedef int (*msg_int)(id, SEL);
typedef unsigned (*msg_uint)(id, SEL);
typedef BOOL (*msg_bool)(id, SEL);
typedef void (*msg_void)(id, SEL);
typedef void (*msg_void_id)(id, SEL, id);
typedef void (*msg_void_u64)(id, SEL, uint64_t);
typedef BOOL (*msg_bool_id_pu16_perr)(id, SEL, id, uint16_t *, id __autoreleasing *);
typedef BOOL (*msg_bool_id_perr)(id, SEL, id, id __autoreleasing *);
typedef BOOL (*msg_bool_4pu64_perr)(id, SEL, uint64_t *, uint64_t *, uint64_t *, uint64_t *, id __autoreleasing *);

#define SEND(type, obj, sel, ...) ((type)objc_msgSend)((obj), sel_registerName(sel), ##__VA_ARGS__)

static void set_err(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;
	if (!err || !errlen) return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static const char *nserr(id e)
{
	return e ? [[e description] UTF8String] : "no error given";
}

/* Every selector the file sends, checked against the class that must
 * answer it.  The port and sync classes are checked here too, before any
 * port exists, so a missing one fails in gptp_open. */
static bool check_selectors(struct gptp *g, char *err, size_t errlen)
{
	static const char *const domain_sels[] = {
		"lockState", "clockIdentity", "grandmasterIdentity", "clockClass", "clockAccuracy",
		"clockPriority1", "clockPriority2", "ports", "addClient:", "removeClient:", "getMetrics", "clockIdentifier",
		"addLinkLayerPortOnInterfaceNamed:allocatedPortNumber:error:",
		"removeLinkLayerPortFromInterfaceNamed:error:",
		"getMachAbsoluteRateRatioNumerator:denominator:machAnchor:andDomainAnchor:withError:",
		"gPTPTimeFromMachAbsoluteTime:", "convertFromMachAbsoluteToDomainTime:grandmasterUsed:",
		"convertFromMachAbsoluteToDomainTime:", "convertFromDomainToMachAbsoluteTime:", NULL
	};
	static const char *const port_sels[] = {
		"interfaceName", "enabled", "asCapable", "portNumber", "remotePortNumber", "portRole",
		"propagationDelay", "propagationDelayLimit", "minimumPropagationDelay", "maximumPropagationDelay",
		"remoteClockIdentity", "localTimestampingMode", "remoteTimestampingMode",
		"addClient:", "removeClient:", "getMetrics", NULL
	};
	static const char *const time_sels[] = {
		"seconds", "nanoseconds", "grandmasterIdentity", "isPTPTimescale", "isTimeTraceable", "isFrequencyTraceable", NULL
	};
	static const char *const clock_metrics_sels[] = { "timeToLock", "gmChangesCount", NULL };
	static const char *const port_metrics_sels[] = {
		"totalMeasurements", "successfulMeasurements", "droppedMeasurements", "syncTimeouts", "meanDelayTime",
		"discardedDelayLimitExceededMeasurements", "discardedOutOfBoundsMeasurements",
		"discardedPpmLimitMeasurements", "discardedTimestampsOutOfOrderMeasurements", NULL
	};
	static const char *const kernel_clock_sels[] = {
		"lockState", "convertFromMachAbsoluteToDomainTime:",
		"getMachAbsoluteRateRatioNumerator:denominator:machAnchor:andDomainAnchor:withError:", NULL
	};
	static const char *const manager_sels[] = { "availableClockIdentifiers", "clockWithClockIdentifier:", NULL };
	static const char *const sync_sels[] = { "addUpdateClient:", "removeUpdateClient:", "registerAsyncCallback", "deregisterAsyncCallback", NULL };
	static const char *const sync_manager_class_sels[] = { "sharedClockSyncManager", NULL };
	static const char *const sync_manager_sels[] = { "clockSyncForClockIdentifier:pid:", NULL };
	struct { const char *cls; const char *const *sels; bool meta; } checks[] = {
		{ "TSgPTPClock", domain_sels, false }, { "TSgPTPEthernetPort", port_sels, false }, { "TSgPTPTime", time_sels, false },
		{ "TSClockMetrics", clock_metrics_sels, false }, { "TSPortMetrics", port_metrics_sels, false },
		{ "TSKernelClock", kernel_clock_sels, false }, { "TSClockManager", manager_sels, false },
		{ "_TSF_TSDClockSync", sync_sels, false }, { "_TSF_TSDClockSyncManager", sync_manager_class_sels, true },
		{ "_TSF_TSDClockSyncManager", sync_manager_sels, false },
	};
	for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		Class c = objc_getClass(checks[i].cls);
		if (!c) { set_err(err, errlen, "TimeSync: class %s not found", checks[i].cls); return false; }
		Class target = checks[i].meta ? object_getClass((id)c) : c;
		for (const char *const *s = checks[i].sels; *s; s++)
			if (!class_respondsToSelector(target, sel_registerName(*s))) {
				set_err(err, errlen, "TimeSync: %s does not respond to %s%s", checks[i].cls, checks[i].meta ? "+" : "-", *s);
				return false;
			}
	}
	if (!objc_getProtocol("TSDClockSyncPTPSyncClient")) { set_err(err, errlen, "TimeSync: protocol TSDClockSyncPTPSyncClient not found"); return false; }
	if (![g->domain isKindOfClass:objc_getClass("TSgPTPClock")]) {
		set_err(err, errlen, "TimeSync: systemDomain is a %s, not a TSgPTPClock", class_getName(object_getClass(g->domain)));
		return false;
	}
	return true;
}

gptp_t *gptp_open(uint64_t timeout_ns, char *err, size_t errlen)
{
	@autoreleasepool {
		if (!timebase.denom) mach_timebase_info(&timebase);
		if (!dlopen(FRAMEWORK, RTLD_NOW)) { set_err(err, errlen, "dlopen %s: %s", FRAMEWORK, dlerror()); return NULL; }
		Class cm_class = objc_getClass("TSClockManager"), gm_class = objc_getClass("TSgPTPManager");
		if (!cm_class || !gm_class) { set_err(err, errlen, "TimeSync: TSClockManager or TSgPTPManager not found"); return NULL; }
		if (!class_respondsToSelector(object_getClass((id)cm_class), sel_registerName("sharedClockManagerSyncWithTimeout:")) ||
		    !class_respondsToSelector(object_getClass((id)gm_class), sel_registerName("sharedgPTPManagerSyncWithTimeout:")) ||
		    !class_respondsToSelector(gm_class, sel_registerName("systemDomain"))) {
			set_err(err, errlen, "TimeSync: the manager selectors have changed");
			return NULL;
		}
		struct gptp *g = calloc(1, sizeof(*g));
		if (!g) return NULL;
		g->clock_manager = SEND(msg_id_u64, (id)cm_class, "sharedClockManagerSyncWithTimeout:", timeout_ns);
		if (!g->clock_manager) { set_err(err, errlen, "TimeSync: no clock manager within the timeout"); free(g); return NULL; }
		g->gptp_manager = SEND(msg_id_u64, (id)gm_class, "sharedgPTPManagerSyncWithTimeout:", timeout_ns);
		if (!g->gptp_manager) { set_err(err, errlen, "TimeSync: no gPTP manager within the timeout"); free(g); return NULL; }
		g->domain = SEND(msg_id, g->gptp_manager, "systemDomain");
		if (!g->domain) { set_err(err, errlen, "TimeSync: no system gPTP domain"); free(g); return NULL; }
		if (!check_selectors(g, err, errlen)) { free(g); return NULL; }
		NSString *v = [[NSBundle bundleWithPath:@FRAMEWORK_DIR] objectForInfoDictionaryKey:@"CFBundleVersion"];
		snprintf(g->version, sizeof(g->version), "%s", v ? [v UTF8String] : "unknown");
		g->client = [GPTPClient new];
		SEND(msg_void_id, g->domain, "addClient:", g->client);
		if (class_respondsToSelector(cm_class, sel_registerName("addClient:")))
			SEND(msg_void_id, g->clock_manager, "addClient:", g->client);
		return g;
	}
}

void gptp_close(gptp_t *g)
{
	if (!g) return;
	@autoreleasepool {
		gptp_sync_unsubscribe(g);
		if (g->added_port) gptp_port_remove(g, NULL, 0);
		else if (g->port) { SEND(msg_void_id, g->port, "removeClient:", g->client); g->port = nil; }
		SEND(msg_void_id, g->domain, "removeClient:", g->client);
		g->client = nil;
		g->domain = nil; g->gptp_manager = nil; g->clock_manager = nil; g->ifname = nil;
	}
	free(g);
}

const char *gptp_framework_version(gptp_t *g)
{
	return g->version;
}

void gptp_set_event_handler(gptp_t *g, gptp_event_fn fn, void *ctx)
{
	g->client.fn = fn;
	g->client.ctx = ctx;
}

/* The port object for the interface, from a fresh copy of the domain's
 * port list: the framework hands out new wrapper objects on each call. */
static id find_port(struct gptp *g)
{
	if (!g->ifname) return nil;
	SEL if_sel = sel_registerName("interfaceName");
	for (id p in SEND(msg_id, g->domain, "ports")) {
		if (![p respondsToSelector:if_sel]) continue;
		NSString *name = SEND(msg_id, p, "interfaceName");
		if ([name isEqualToString:g->ifname]) return p;
	}
	return nil;
}

bool gptp_port_add(gptp_t *g, const char *ifname, char *err, size_t errlen)
{
	@autoreleasepool {
		g->ifname = [NSString stringWithUTF8String:ifname];
		id existing = find_port(g);
		if (!existing) {
			uint16_t portnum = 0;
			id __autoreleasing e = nil;
			if (!SEND(msg_bool_id_pu16_perr, g->domain, "addLinkLayerPortOnInterfaceNamed:allocatedPortNumber:error:", g->ifname, &portnum, &e)) {
				set_err(err, errlen, "adding a gPTP port on %s: %s", ifname, nserr(e));
				/* Keep the interface so a state read can find a port that
				 * reappears before the next add attempt. */
				return false;
			}
			g->added_port = true;
			existing = find_port(g);
			if (!existing) {
				set_err(err, errlen, "the gPTP port on %s (port %u) was added but is not listed", ifname, portnum);
				return false;
			}
		}
		g->port = existing;
		SEND(msg_void_id, g->port, "addClient:", g->client);
		return true;
	}
}

bool gptp_port_remove(gptp_t *g, char *err, size_t errlen)
{
	@autoreleasepool {
		if (!g->ifname) return true;
		if (g->port) { SEND(msg_void_id, g->port, "removeClient:", g->client); g->port = nil; }
		id __autoreleasing e = nil;
		bool ok = SEND(msg_bool_id_perr, g->domain, "removeLinkLayerPortFromInterfaceNamed:error:", g->ifname, &e);
		if (!ok) set_err(err, errlen, "removing the gPTP port from %s: %s", [g->ifname UTF8String], nserr(e));
		g->added_port = false;
		g->ifname = nil;
		return ok;
	}
}

void gptp_port_state(gptp_t *g, struct gptp_port_state *st)
{
	@autoreleasepool {
		memset(st, 0, sizeof(*st));
		id p = find_port(g);
		if (!p) return;
		st->present = true;
		st->enabled = SEND(msg_bool, p, "enabled");
		st->as_capable = SEND(msg_bool, p, "asCapable");
		st->port_number = SEND(msg_uint, p, "portNumber") & 0xffff;
		st->remote_port_number = SEND(msg_uint, p, "remotePortNumber") & 0xffff;
		st->port_role = SEND(msg_int, p, "portRole");
		st->propagation_delay_ns = SEND(msg_uint, p, "propagationDelay");
		st->propagation_delay_limit_ns = SEND(msg_uint, p, "propagationDelayLimit");
		st->propagation_delay_min_ns = SEND(msg_uint, p, "minimumPropagationDelay");
		st->propagation_delay_max_ns = SEND(msg_uint, p, "maximumPropagationDelay");
		st->remote_clock_identity = SEND(msg_u64, p, "remoteClockIdentity");
		st->local_timestamping_mode = SEND(msg_uint, p, "localTimestampingMode") & 0xff;
		st->remote_timestamping_mode = SEND(msg_uint, p, "remoteTimestampingMode") & 0xff;
	}
}

void gptp_domain_state(gptp_t *g, struct gptp_domain_state *st)
{
	@autoreleasepool {
		id d = g->domain;
		st->lock_state = SEND(msg_int, d, "lockState");
		st->clock_identity = SEND(msg_u64, d, "clockIdentity");
		st->grandmaster_identity = SEND(msg_u64, d, "grandmasterIdentity");
		st->clock_class = SEND(msg_uint, d, "clockClass") & 0xff;
		st->clock_accuracy = SEND(msg_uint, d, "clockAccuracy") & 0xff;
		st->priority1 = SEND(msg_uint, d, "clockPriority1") & 0xff;
		st->priority2 = SEND(msg_uint, d, "clockPriority2") & 0xff;
	}
}

static bool read_mapping(id clock, struct gptp_mapping *m, char *err, size_t errlen)
{
	id __autoreleasing e = nil;
	memset(m, 0, sizeof(*m));
	if (!SEND(msg_bool_4pu64_perr, clock, "getMachAbsoluteRateRatioNumerator:denominator:machAnchor:andDomainAnchor:withError:",
		  &m->ratio_num, &m->ratio_den, &m->mach_anchor, &m->domain_anchor, &e)) {
		set_err(err, errlen, "reading the rate ratio: %s", nserr(e));
		return false;
	}
	return true;
}

bool gptp_mapping(gptp_t *g, struct gptp_mapping *m, char *err, size_t errlen)
{
	@autoreleasepool { return read_mapping(g->domain, m, err, errlen); }
}

bool gptp_metrics(gptp_t *g, struct gptp_metrics *m)
{
	@autoreleasepool {
		memset(m, 0, sizeof(*m));
		id cm = SEND(msg_id, g->domain, "getMetrics");
		if (!cm) return false;
		m->time_to_lock = SEND(msg_u64, cm, "timeToLock");
		m->gm_changes = SEND(msg_u64, cm, "gmChangesCount");
		id p = find_port(g);
		id pm = p ? SEND(msg_id, p, "getMetrics") : nil;
		if (!pm) return true;
		m->port_total = SEND(msg_u64, pm, "totalMeasurements");
		m->port_successful = SEND(msg_u64, pm, "successfulMeasurements");
		m->port_dropped = SEND(msg_u64, pm, "droppedMeasurements");
		m->port_discarded = SEND(msg_u64, pm, "discardedDelayLimitExceededMeasurements") +
			SEND(msg_u64, pm, "discardedOutOfBoundsMeasurements") +
			SEND(msg_u64, pm, "discardedPpmLimitMeasurements") +
			SEND(msg_u64, pm, "discardedTimestampsOutOfOrderMeasurements");
		m->port_sync_timeouts = SEND(msg_u64, pm, "syncTimeouts");
		m->port_mean_delay = SEND(msg_u64, pm, "meanDelayTime");
		return true;
	}
}

bool gptp_time_from_mach(gptp_t *g, uint64_t mach, struct gptp_time *t)
{
	@autoreleasepool {
		memset(t, 0, sizeof(*t));
		id gt = SEND(msg_id_u64, g->domain, "gPTPTimeFromMachAbsoluteTime:", mach);
		if (!gt) return false;
		t->seconds = SEND(msg_u64, gt, "seconds");
		t->nanoseconds = SEND(msg_uint, gt, "nanoseconds");
		t->grandmaster_identity = SEND(msg_u64, gt, "grandmasterIdentity");
		t->ptp_timescale = SEND(msg_bool, gt, "isPTPTimescale");
		t->time_traceable = SEND(msg_bool, gt, "isTimeTraceable");
		t->frequency_traceable = SEND(msg_bool, gt, "isFrequencyTraceable");
		return true;
	}
}

uint64_t gptp_domain_from_mach(gptp_t *g, uint64_t mach, uint64_t *grandmaster_used)
{
	uint64_t gm = 0;
	uint64_t d = SEND(msg_u64_u64_pu64, g->domain, "convertFromMachAbsoluteToDomainTime:grandmasterUsed:", mach, &gm);
	if (grandmaster_used) *grandmaster_used = gm;
	return d;
}

uint64_t gptp_mach_from_domain(gptp_t *g, uint64_t domain_ns)
{
	return SEND(msg_u64_u64, g->domain, "convertFromDomainToMachAbsoluteTime:", domain_ns);
}

int64_t gptp_mach_to_ns(int64_t ticks)
{
	if (!timebase.denom) mach_timebase_info(&timebase);
	return (int64_t)((__int128)ticks * timebase.numer / timebase.denom);
}

uint64_t gptp_ns_to_mach(int64_t ns)
{
	if (!timebase.denom) mach_timebase_info(&timebase);
	return (uint64_t)((__int128)ns * timebase.denom / timebase.numer);
}

/* ---- per-sync updates ------------------------------------------------- */

bool gptp_sync_subscribe(gptp_t *g, gptp_sync_fn fn, void *ctx, char *err, size_t errlen)
{
	@autoreleasepool {
		if (g->clock_sync) { set_err(err, errlen, "already subscribed"); return false; }
		/* The framework's dispatcher delivers only to a client that conforms to
		 * its protocols; all three that the daemon-side clock classes declare
		 * are needed, the PTP sync one alone gets nothing. */
		static const char *const protos[] = { "TSDClockSyncPTPSyncClient", "TSDClockSyncGeneralSyncClient", "TSDKernelClockClient" };
		for (size_t i = 0; i < 3; i++) {
			Protocol *proto = objc_getProtocol(protos[i]);
			if (!proto) { set_err(err, errlen, "TimeSync: protocol %s not found", protos[i]); return false; }
			if (!class_conformsToProtocol([GPTPSyncClient class], proto)) class_addProtocol([GPTPSyncClient class], proto);
		}
		id mgr = SEND(msg_id, (id)objc_getClass("_TSF_TSDClockSyncManager"), "sharedClockSyncManager");
		if (!mgr) { set_err(err, errlen, "TimeSync: no clock sync manager"); return false; }
		uint64_t cid = SEND(msg_u64, g->domain, "clockIdentifier");
		id cs = SEND(msg_id_u64_int, mgr, "clockSyncForClockIdentifier:pid:", cid, (int)getpid());
		if (!cs) { set_err(err, errlen, "TimeSync: no clock sync connection for the domain"); return false; }
		g->sync_client = [GPTPSyncClient new];
		g->sync_client.fn = fn;
		g->sync_client.ctx = ctx;
		SEND(msg_void_id, cs, "addUpdateClient:", g->sync_client);
		if (!SEND(msg_bool, cs, "registerAsyncCallback")) {
			SEND(msg_void_id, cs, "removeUpdateClient:", g->sync_client);
			g->sync_client = nil;
			set_err(err, errlen, "TimeSync: registerAsyncCallback failed");
			return false;
		}
		g->clock_sync = cs;
		return true;
	}
}

void gptp_sync_unsubscribe(gptp_t *g)
{
	@autoreleasepool {
		if (!g->clock_sync) return;
		SEND(msg_bool, g->clock_sync, "deregisterAsyncCallback");
		SEND(msg_void_id, g->clock_sync, "removeUpdateClient:", g->sync_client);
		g->clock_sync = nil;
		g->sync_client = nil;
	}
}

/* ---- the other kernel clocks ------------------------------------------- */

static enum gptp_clock_kind clock_kind(id clock)
{
	Class c = object_getClass(clock);
	if (c == objc_getClass("TSgPTPClock")) return GPTP_CLOCK_DOMAIN;
	if (c == objc_getClass("TSTranslationClock")) return GPTP_CLOCK_TRANSLATION;
	if (c == objc_getClass("TSKernelClock")) return GPTP_CLOCK_ADAPTER;
	return GPTP_CLOCK_OTHER;
}

static id clock_with_identifier(struct gptp *g, uint64_t identifier)
{
	return SEND(msg_id_u64, g->clock_manager, "clockWithClockIdentifier:", identifier);
}

size_t gptp_clocks(gptp_t *g, struct gptp_clock_info *clocks, size_t max)
{
	@autoreleasepool {
		size_t n = 0;
		for (NSNumber *num in SEND(msg_id, g->clock_manager, "availableClockIdentifiers")) {
			if (n == max) break;
			uint64_t cid = num.unsignedLongLongValue;
			id c = clock_with_identifier(g, cid);
			clocks[n].identifier = cid;
			clocks[n].kind = c ? clock_kind(c) : GPTP_CLOCK_OTHER;
			clocks[n].lock_state = c ? SEND(msg_int, c, "lockState") : -1;
			n++;
		}
		return n;
	}
}

bool gptp_clock_mapping(gptp_t *g, uint64_t identifier, struct gptp_mapping *m, char *err, size_t errlen)
{
	@autoreleasepool {
		id c = clock_with_identifier(g, identifier);
		if (!c) { set_err(err, errlen, "no clock %016llx", (unsigned long long)identifier); return false; }
		if (!read_mapping(c, m, err, errlen)) return false;
		if (clock_kind(c) == GPTP_CLOCK_ADAPTER) {
			/* Adapter clocks report the ratio the other way up: mach nanoseconds
			 * per clock nanosecond, times the timebase.  Turn it into clock
			 * nanoseconds per mach tick, which is what the domain reports. */
			if (!timebase.denom) mach_timebase_info(&timebase);
			uint64_t num = m->ratio_num, den = m->ratio_den;
			m->ratio_num = den * timebase.numer * timebase.numer;
			m->ratio_den = num * timebase.denom * timebase.denom;
		}
		return true;
	}
}

uint64_t gptp_clock_domain_from_mach(gptp_t *g, uint64_t identifier, uint64_t mach)
{
	@autoreleasepool {
		id c = clock_with_identifier(g, identifier);
		return c ? SEND(msg_u64_u64, c, "convertFromMachAbsoluteToDomainTime:", mach) : 0;
	}
}

/* ---- names ------------------------------------------------------------- */

const char *gptp_event_name(enum gptp_event ev)
{
	switch (ev) {
	case GPTP_EV_LOCK_STATE: return "lock state";
	case GPTP_EV_GM_CHANGE_BEGIN: return "grandmaster change began";
	case GPTP_EV_GM_CHANGE_END: return "grandmaster change ended";
	case GPTP_EV_MASTER_CHANGED: return "clock master changed";
	case GPTP_EV_CLOCK_RESET: return "clock reset";
	case GPTP_EV_SYNC: return "sync";
	case GPTP_EV_AS_CAPABLE: return "asCapable";
	case GPTP_EV_SYNC_TIMEOUT: return "sync timeout";
	case GPTP_EV_ANNOUNCE_TIMEOUT: return "announce timeout";
	case GPTP_EV_PORT_TERMINATED: return "port service terminated";
	case GPTP_EV_INTERRUPTED: return "daemon connection interrupted";
	}
	return "?";
}

const char *gptp_lock_state_name(int lock_state)
{
	static char buf[16];
	if (lock_state == GPTP_LOCK_LOCKED) return "locked";
	snprintf(buf, sizeof(buf), "%d", lock_state);
	return buf;
}
