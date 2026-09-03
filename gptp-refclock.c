/* SPDX-License-Identifier: MIT */
/* gptp-refclock: a chrony SOCK reference clock fed from the Mac's own
 * gPTP clock.  The kernel keeps a mapping from mach absolute time to gPTP
 * domain time, disciplined from a grandmaster reached back to back or
 * through an 802.1AS bridge, with hardware timestamps at both ends.  Each interval the
 * program reads mach time bracketed around the system clock, converts the
 * mach time to gPTP time through that mapping, subtracts the UTC offset
 * and sends chrony the difference as a complete sample.  Nothing is sent
 * unless the domain is locked to an external grandmaster on the PTP
 * timescale and has been, undisturbed, for --settle seconds.
 *
 * Events (lock, loss of lock, grandmaster changes, chrony trouble) go to
 * stderr, or with --os-log to the unified log, where a launchd plist would
 * send them.  Measurements, one record per sample, go to the file named by
 * --log and nowhere else.  The framework delivers its callbacks on a thread
 * of its own; that thread only records what happened, and the main loop
 * writes everything.  libgptp talks to the framework; chrony-client.[ch] is
 * the SOCK client. */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <stdatomic.h>
#include <sys/time.h>
#include "chrony-client.h"
#include "gptp.h"

#define NS_PER_S 1000000000LL

/* defaults of the options */
#define DEFAULT_SOCK_PATH "/var/run/chrony.gptp.sock"
#define DEFAULT_INTERVAL_S 1.0
#define DEFAULT_PORT_RETRY_S 1.0	/* between attempts to restore a missing port */
#define DEFAULT_UTC_OFFSET 37		/* TAI - UTC since 2017-01-01; the announce's value is not reachable */
#define DEFAULT_SETTLE_S 5.0		/* locked to the same grandmaster for this long before sending */
#define DEFAULT_BRACKETS 4U		/* clock readings per sample; the narrowest counts */
#define DEFAULT_MAX_BRACKET_US 5.0	/* a wider bracket was preempted */
#define DEFAULT_TIMEOUT_S 2.0		/* waiting for the TimeSync daemon */
#define POLL_SLEEP_NS 100000000		/* the main loop's wait is chopped so a signal ends it promptly */
#define EVENT_RING 64			/* events recorded by the framework thread, awaiting the main loop */
#define OS_LOG_SUBSYSTEM "com.jclark.gptp"
#define MAX_INTERVAL_S 3600.0
#define MAX_PORT_RETRY_S 3600.0
#define MAX_SETTLE_S 86400.0
#define MAX_TIMEOUT_S 60.0
#define MAX_BRACKET_US 1000000.0
#define MAX_BRACKETS 1000U
#define MAX_UTC_OFFSET 1000U


static struct config {
	const char *interface;
	const char *sock_path;
	double interval_s, port_retry_s, settle_s, timeout_s;
	int utc_offset;
	unsigned brackets;
	double max_bracket_us;
	const char *log_path;
	bool json, os_log;
} cfg = {
	.sock_path = DEFAULT_SOCK_PATH, .interval_s = DEFAULT_INTERVAL_S, .port_retry_s = DEFAULT_PORT_RETRY_S,
	.settle_s = DEFAULT_SETTLE_S,
	.timeout_s = DEFAULT_TIMEOUT_S, .utc_offset = DEFAULT_UTC_OFFSET, .brackets = DEFAULT_BRACKETS,
	.max_bracket_us = DEFAULT_MAX_BRACKET_US,
};

/* What the domain is doing, in the order the conditions are checked; a
 * sample goes to chrony only in ST_LOCKED. */
enum state { ST_NO_PORT, ST_NOT_CAPABLE, ST_OWN_GM, ST_UNLOCKED, ST_NOT_PTP, ST_SETTLING, ST_LOCKED };
static const char *const state_name[] = { "noport", "notcapable", "owngm", "unlocked", "notptp", "settling", "locked" };

static gptp_t *ts;
static chrony_client_t *chrony;
static FILE *log_out;			/* the measurement file, or NULL */
static os_log_t oslog;
static mach_timebase_info_data_t timebase;
static uint64_t start_mach;
static volatile sig_atomic_t stop_flag;

/* Events from the framework arrive on its thread.  That thread only
 * records them here; the main loop drains the ring and reports them.  One
 * producer, one consumer, so two atomic indices are enough. */
struct event { uint64_t mach; enum gptp_event ev; int64_t arg; };
static struct {
	struct event ring[EVENT_RING];
	atomic_uint head, tail;		/* head: next to write; tail: next to read */
	atomic_uint dropped;		/* ring full */
	atomic_uint_fast64_t syncs;	/* since the last log line */
	uint64_t total;			/* reported, by the main loop */
} ev;

static struct {
	enum state state;
	bool have_state;
	uint64_t settle_start_mach, settle_gm, next_port_retry_mach;
	uint64_t samples, sent, state_changes, wide_brackets;
	bool header_done, chrony_failing, mapping_failing, port_retry_failing;
} run;

/* ---- time helpers ---------------------------------------------------- */

static double mach_to_s(uint64_t ticks) { return (double)gptp_mach_to_ns((int64_t)ticks) * 1e-9; }
static uint64_t seconds_to_mach(double s) { return gptp_ns_to_mach((int64_t)llround(s * 1e9)); }
static double since_start_s(void) { return mach_to_s(mach_absolute_time() - start_mach); }

/* An event line: to stderr, or with --os-log to the unified log.  Only the
 * main loop calls this. */
static void note_at(uint64_t mach, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note_at(uint64_t mach, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	/* the event's own time, as UTC date and time */
	int64_t now_ns = (int64_t)clock_gettime_nsec_np(CLOCK_REALTIME);
	int64_t at_ns = now_ns - gptp_mach_to_ns((int64_t)(mach_absolute_time() - mach));
	time_t secs = (time_t)(at_ns / NS_PER_S);
	struct tm tm;
	gmtime_r(&secs, &tm);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
	if (cfg.os_log) {
		os_log(oslog, "event time %{public}s.%03ld UTC: %{public}s", stamp, (long)(at_ns % NS_PER_S / 1000000), msg);
		return;
	}
	fprintf(stderr, "%s.%03ld %s\n", stamp, (long)(at_ns % NS_PER_S / 1000000), msg);
}
#define note(...) note_at(mach_absolute_time(), __VA_ARGS__)

/* mach time bracketed around CLOCK_REALTIME; the narrowest of cfg.brackets
 * tries, false if every one was wider than --max-bracket (preempted). */
static bool read_clocks(uint64_t *mach_mid, int64_t *realtime_ns, int64_t *width_ns)
{
	int64_t best = INT64_MAX;
	for (unsigned i = 0; i < cfg.brackets; i++) {
		uint64_t m1 = mach_absolute_time();
		uint64_t r = clock_gettime_nsec_np(CLOCK_REALTIME);
		uint64_t m2 = mach_absolute_time();
		int64_t w = gptp_mach_to_ns((int64_t)(m2 - m1));
		if (w < best) { best = w; *mach_mid = m1 + (m2 - m1) / 2; *realtime_ns = (int64_t)r; }
	}
	*width_ns = best;
	return best <= (int64_t)(cfg.max_bracket_us * 1e3);
}

/* gPTP time at a mach time as nanoseconds; false if the mapping gave nothing */
static bool gptp_ns_at(uint64_t mach, struct gptp_time *t, int64_t *ns)
{
	if (!gptp_time_from_mach(ts, mach, t)) return false;
	*ns = gptp_time_to_ns(t);
	return true;
}

/* ---- events ----------------------------------------------------------- */

/* On the framework's thread: record and return. */
static void on_event(void *ctx, enum gptp_event e, int64_t arg)
{
	(void)ctx;
	if (e == GPTP_EV_SYNC) { atomic_fetch_add(&ev.syncs, 1); return; }
	unsigned head = atomic_load_explicit(&ev.head, memory_order_relaxed);
	unsigned next = (head + 1) % EVENT_RING;
	if (next == atomic_load_explicit(&ev.tail, memory_order_acquire)) { atomic_fetch_add(&ev.dropped, 1); return; }
	ev.ring[head] = (struct event){ .mach = mach_absolute_time(), .ev = e, .arg = arg };
	atomic_store_explicit(&ev.head, next, memory_order_release);
}

/* On the main loop: report what arrived, and say whether any of it
 * unsettles the domain (anything but a lock, an asCapable link or the end
 * of a change). */
static bool drain_events(void)
{
	bool disturbed = false;
	unsigned tail = atomic_load_explicit(&ev.tail, memory_order_relaxed);
	while (tail != atomic_load_explicit(&ev.head, memory_order_acquire)) {
		const struct event *e = &ev.ring[tail];
		ev.total++;
		if (!((e->ev == GPTP_EV_LOCK_STATE && e->arg == GPTP_LOCK_LOCKED) || (e->ev == GPTP_EV_AS_CAPABLE && e->arg) || e->ev == GPTP_EV_GM_CHANGE_END))
			disturbed = true;
		if (e->ev == GPTP_EV_LOCK_STATE)
			note_at(e->mach, "event: lock state %s", gptp_lock_state_name((int)e->arg));
		else if (e->ev == GPTP_EV_AS_CAPABLE)
			note_at(e->mach, "event: port %s", e->arg ? "asCapable" : "no longer asCapable");
		else if ((e->ev == GPTP_EV_GM_CHANGE_BEGIN || e->ev == GPTP_EV_GM_CHANGE_END || e->ev == GPTP_EV_MASTER_CHANGED) && e->arg)
			note_at(e->mach, "event: %s, grandmaster %016" PRIx64, gptp_event_name(e->ev), (uint64_t)e->arg);
		else
			note_at(e->mach, "event: %s", gptp_event_name(e->ev));
		tail = (tail + 1) % EVENT_RING;
		atomic_store_explicit(&ev.tail, tail, memory_order_release);
	}
	unsigned dropped = atomic_exchange(&ev.dropped, 0);
	if (dropped) { note("%u events arrived faster than they were read and were lost", dropped); disturbed = true; }
	return disturbed;
}

/* ---- the log line ------------------------------------------------------
 * The space-separated header and row and the JSON object all come from one
 * column table, so they cannot disagree. */
struct col {
	const char *name;
	enum { COL_I, COL_U, COL_D, COL_S } kind;
	int prec;
	int64_t i; uint64_t u; double d; const char *s;
};
#define COL_I(n, v) { n, COL_I, 0, (v), 0, 0, NULL }
#define COL_U(n, v) { n, COL_U, 0, 0, (v), 0, NULL }
#define COL_D(n, p, v) { n, COL_D, (p), 0, 0, (v), NULL }
#define COL_S(n, v) { n, COL_S, 0, 0, 0, 0, (v) }

static void col_value(FILE *f, const struct col *k, bool json)
{
	switch (k->kind) {
	case COL_I: fprintf(f, "%" PRId64, k->i); break;
	case COL_U: fprintf(f, "%" PRIu64, k->u); break;
	case COL_D: if (json && isnan(k->d)) fputs("null", f); else fprintf(f, "%.*f", k->prec, k->d); break;
	case COL_S: fprintf(f, json ? "\"%s\"" : "%s", k->s); break;
	}
}

struct sample {
	uint64_t mach;
	int64_t realtime_ns, bracket_ns;
	bool bracket_ok, have_gptp, sent;
	struct gptp_time gptp;
	int64_t gptp_ns, offset_ns;	/* offset: gPTP-derived UTC minus system time */
	struct gptp_domain_state dom;
	struct gptp_port_state port;
	struct gptp_mapping map;
	bool have_map;
	struct gptp_metrics met;
	uint64_t syncs;
};

static void log_line(const struct sample *s)
{
	if (!log_out) return;

	char own[24], gm[24], remote[24], used[24];
	snprintf(own, sizeof(own), "%016" PRIx64, s->dom.clock_identity);
	snprintf(gm, sizeof(gm), "%016" PRIx64, s->dom.grandmaster_identity);
	snprintf(remote, sizeof(remote), "%016" PRIx64, s->port.remote_clock_identity);
	snprintf(used, sizeof(used), "%016" PRIx64, s->gptp.grandmaster_identity);
	/* the rate ratio against the mach timebase, as a frequency offset */
	double rate_ppb = s->have_map && s->map.ratio_den ?
		((double)s->map.ratio_num * timebase.denom / ((double)s->map.ratio_den * timebase.numer) - 1.0) * 1e9 : NAN;
	double t_mono = mach_to_s(s->mach - start_mach), t_real = (double)s->realtime_ns * 1e-9;

	const struct col cols[] = {
		COL_D("t_mono_s", 6, t_mono), COL_D("t_real_s", 6, t_real), COL_S("state", state_name[run.state]),
		COL_I("lock_state", s->dom.lock_state), COL_S("clock_identity", own), COL_S("grandmaster", gm),
		COL_U("clock_class", s->dom.clock_class), COL_U("clock_accuracy", s->dom.clock_accuracy),
		COL_U("port_present", s->port.present), COL_U("as_capable", s->port.as_capable),
		COL_I("port_role", s->port.port_role), COL_S("remote_identity", remote),
		COL_U("pdelay_ns", s->port.propagation_delay_ns), COL_U("pdelay_min_ns", s->port.propagation_delay_min_ns),
		COL_U("pdelay_max_ns", s->port.propagation_delay_max_ns), COL_U("pdelay_limit_ns", s->port.propagation_delay_limit_ns),
		COL_U("local_ts_mode", s->port.local_timestamping_mode), COL_U("remote_ts_mode", s->port.remote_timestamping_mode),
		COL_U("ratio_num", s->map.ratio_num), COL_U("ratio_den", s->map.ratio_den), COL_D("rate_ppb", 4, rate_ppb),
		COL_U("mach_anchor", s->map.mach_anchor), COL_U("domain_anchor", s->map.domain_anchor),
		COL_U("mach", s->mach), COL_I("bracket_ns", s->bracket_ns), COL_I("realtime_ns", s->realtime_ns),
		COL_I("gptp_ns", s->gptp_ns), COL_S("gptp_grandmaster", used),
		COL_U("ptp_timescale", s->gptp.ptp_timescale), COL_U("time_traceable", s->gptp.time_traceable),
		COL_U("freq_traceable", s->gptp.frequency_traceable),
		COL_I("offset_ns", s->offset_ns), COL_U("sent", s->sent), COL_U("syncs", s->syncs),
		COL_U("gm_changes", s->met.gm_changes), COL_U("time_to_lock", s->met.time_to_lock),
		COL_U("meas_total", s->met.port_total), COL_U("meas_ok", s->met.port_successful),
		COL_U("meas_dropped", s->met.port_dropped), COL_U("meas_discarded", s->met.port_discarded),
		COL_U("sync_timeouts", s->met.port_sync_timeouts), COL_U("mean_delay", s->met.port_mean_delay),
	};
	const size_t ncols = sizeof(cols) / sizeof(cols[0]);
	size_t k;

	errno = 0;
	if (cfg.json) {
		fputc('{', log_out);
		for (k = 0; k < ncols; k++) {
			fprintf(log_out, "%s\"%s\":", k ? "," : "", cols[k].name);
			col_value(log_out, &cols[k], true);
		}
		fputs("}\n", log_out);
	} else {
		/* space-separated columns; the names on a # line when the file is opened */
		if (!run.header_done) {
			fputc('#', log_out);
			for (k = 0; k < ncols; k++) fprintf(log_out, " %s", cols[k].name);
			fputc('\n', log_out);
			run.header_done = true;
		}
		for (k = 0; k < ncols; k++) { if (k) fputc(' ', log_out); col_value(log_out, &cols[k], false); }
		fputc('\n', log_out);
	}
	if (fflush(log_out) == EOF || ferror(log_out)) {
		int saved_errno = errno ? errno : EIO;
		FILE *failed = log_out;
		log_out = NULL;
		fclose(failed);
		note("measurement log %s: %s; logging disabled", cfg.log_path, strerror(saved_errno));
	}
}

/* ---- one sample ----------------------------------------------------- */

/* The state the domain is in now, and whether the settling time has run. */
static enum state classify(const struct sample *s, bool disturbed)
{
	if (!s->port.present) return ST_NO_PORT;
	if (!s->port.enabled || !s->port.as_capable) return ST_NOT_CAPABLE;
	if (s->dom.grandmaster_identity == s->dom.clock_identity) return ST_OWN_GM;
	if (s->dom.lock_state != GPTP_LOCK_LOCKED) return ST_UNLOCKED;
	if (!s->have_gptp || !s->gptp.ptp_timescale || s->gptp.grandmaster_identity != s->dom.grandmaster_identity) return ST_NOT_PTP;
	if (disturbed || run.settle_gm != s->dom.grandmaster_identity || run.state < ST_SETTLING) {
		run.settle_start_mach = s->mach;
		run.settle_gm = s->dom.grandmaster_identity;
		if (disturbed && run.state == ST_LOCKED) note("settling again after the event");
	}
	return mach_to_s(s->mach - run.settle_start_mach) >= cfg.settle_s ? ST_LOCKED : ST_SETTLING;
}

static bool read_port_state(struct gptp_port_state *port)
{
	char err[256];
	gptp_port_state(ts, port);
	if (port->present) {
		if (run.port_retry_failing) note("gPTP port on %s is available again", cfg.interface);
		run.port_retry_failing = false;
		run.next_port_retry_mach = 0;
		return false;
	}
	uint64_t now = mach_absolute_time();
	if (run.next_port_retry_mach && (int64_t)(run.next_port_retry_mach - now) > 0) return true;
	run.next_port_retry_mach = now + seconds_to_mach(cfg.port_retry_s);
	if (!gptp_port_add(ts, cfg.interface, err, sizeof(err))) {
		if (!run.port_retry_failing) note("%s; will retry", err);
		run.port_retry_failing = true;
		return true;
	}
	note("gPTP port on %s is available again", cfg.interface);
	run.port_retry_failing = false;
	run.next_port_retry_mach = 0;
	gptp_port_state(ts, port);
	return true;
}

static void take_sample(void)
{
	struct sample s;
	bool disturbed, port_disturbed;
	char err[256];

	memset(&s, 0, sizeof(s));
	port_disturbed = read_port_state(&s.port);
	gptp_domain_state(ts, &s.dom);
	if (log_out) {
		s.have_map = gptp_mapping(ts, &s.map, err, sizeof(err));
		if (!s.have_map && !run.mapping_failing) note("%s; mapping fields will be zero", err);
		else if (s.have_map && run.mapping_failing) note("reading the gPTP mapping again");
		run.mapping_failing = !s.have_map;
		gptp_metrics(ts, &s.met);
	}

	s.bracket_ok = read_clocks(&s.mach, &s.realtime_ns, &s.bracket_ns);
	if (!s.bracket_ok) run.wide_brackets++;
	s.have_gptp = gptp_ns_at(s.mach, &s.gptp, &s.gptp_ns);
	s.offset_ns = s.gptp_ns - (int64_t)cfg.utc_offset * NS_PER_S - s.realtime_ns;

	disturbed = drain_events();
	disturbed = disturbed || port_disturbed;
	s.syncs = atomic_exchange(&ev.syncs, 0);

	enum state st = classify(&s, disturbed);
	if (!run.have_state || st != run.state) {
		if (run.have_state) run.state_changes++;
		run.state = st;
		run.have_state = true;
		switch (st) {
		case ST_NO_PORT: note("no gPTP port on %s", cfg.interface); break;
		case ST_NOT_CAPABLE: note("port on %s is not asCapable: no gPTP peer on the link", cfg.interface); break;
		case ST_OWN_GM: note("the Mac is its own grandmaster (%016" PRIx64 "); waiting for a better one", s.dom.clock_identity); break;
		case ST_UNLOCKED: note("grandmaster %016" PRIx64 ", lock state %d: not locked", s.dom.grandmaster_identity, s.dom.lock_state); break;
		case ST_NOT_PTP: note("locked to %016" PRIx64 " but not on the PTP timescale (timescale %d, conversion used %016" PRIx64 ")",
				     s.dom.grandmaster_identity, s.gptp.ptp_timescale, s.gptp.grandmaster_identity); break;
		case ST_SETTLING: note("locked to %016" PRIx64 " (class %u, accuracy %u, propagation delay %u ns); settling for %g s",
				      s.dom.grandmaster_identity, s.dom.clock_class, s.dom.clock_accuracy, s.port.propagation_delay_ns, cfg.settle_s); break;
		case ST_LOCKED: note("sending: gPTP minus system %+.3f us", (double)s.offset_ns * 1e-3); break;
		}
	}

	if (st == ST_LOCKED && s.bracket_ok) {
		struct timeval tv = { .tv_sec = (time_t)(s.realtime_ns / NS_PER_S), .tv_usec = (suseconds_t)(s.realtime_ns % NS_PER_S / 1000) };
		if (chrony_client_send_sample(chrony, &tv, (double)s.offset_ns * 1e-9, 0) == 0) {
			s.sent = true; run.sent++;
			if (run.chrony_failing) { note("chrony: sending again"); run.chrony_failing = false; }
		} else if (!run.chrony_failing) {
			int saved_errno = errno;
			if (saved_errno == EACCES || saved_errno == EPERM)
				note("chrony: %s: %s; run gptp-refclock with sudo", cfg.sock_path, strerror(saved_errno));
			else
				note("chrony: %s: %s (is the refclock line in chrony.conf, and chronyd running?)", cfg.sock_path, strerror(saved_errno));
			run.chrony_failing = true;
		}
	}
	run.samples++;
	log_line(&s);
}

/* ---- main -------------------------------------------------------------- */

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static void usage(const char *prog)
{
	fprintf(stderr,
"Usage: %s [options] INTERFACE\n"
"A chrony SOCK refclock from the Mac's gPTP clock on INTERFACE.\n"
"      --sock PATH             chrony's SOCK refclock socket (%s)\n"
"      --interval S            seconds between samples (%g)\n"
"      --port-retry S          seconds between attempts to restore a missing port (%g)\n"
"      --utc-offset N          TAI minus UTC in seconds (%d)\n"
"      --settle S              locked and undisturbed for this long before sending (%g)\n"
"      --brackets N            clock readings per sample; the narrowest counts (%u)\n"
"      --max-bracket US        widest bracket accepted; a wider one was preempted (%g)\n"
"      --timeout S             wait this long for the TimeSync daemon (%g)\n"
"      --log PATH              write one measurement record per sample here; none without it\n"
"  -j, --json                  the measurement file as JSON lines instead of space-separated columns\n"
"      --os-log                events to the unified log (subsystem " OS_LOG_SUBSYSTEM ") instead of stderr\n"
"  -h, --help\n",
		prog, DEFAULT_SOCK_PATH, DEFAULT_INTERVAL_S, DEFAULT_PORT_RETRY_S, DEFAULT_UTC_OFFSET, DEFAULT_SETTLE_S, DEFAULT_BRACKETS,
		DEFAULT_MAX_BRACKET_US, DEFAULT_TIMEOUT_S);
}

static double arg_double(const char *name, const char *s, double lo, double hi)
{
	char *end;
	errno = 0;
	double v = strtod(s, &end);
	if (end == s || *end || errno == ERANGE || !isfinite(v) || v < lo || v > hi) {
		fprintf(stderr, "%s: bad value '%s'; must be between %g and %g\n", name, s, lo, hi);
		exit(2);
	}
	return v;
}

static unsigned arg_uint(const char *name, const char *s, unsigned lo, unsigned hi)
{
	char *end;
	errno = 0;
	if (*s == '-') goto bad;
	unsigned long long v = strtoull(s, &end, 10);
	if (end == s || *end || errno == ERANGE || v < lo || v > hi) goto bad;
	return (unsigned)v;
bad:
	fprintf(stderr, "%s: bad value '%s'; must be an integer between %u and %u\n", name, s, lo, hi);
	exit(2);
}

int main(int argc, char **argv)
{
	enum { OPT_SOCK = 256, OPT_INTERVAL, OPT_PORT_RETRY, OPT_UTC, OPT_SETTLE, OPT_BRACKETS, OPT_MAXBR, OPT_TIMEOUT,
	       OPT_LOG, OPT_OSLOG };
	static const struct option opts[] = {
		{ "sock", required_argument, NULL, OPT_SOCK }, { "interval", required_argument, NULL, OPT_INTERVAL },
		{ "port-retry", required_argument, NULL, OPT_PORT_RETRY },
		{ "utc-offset", required_argument, NULL, OPT_UTC }, { "settle", required_argument, NULL, OPT_SETTLE },
		{ "brackets", required_argument, NULL, OPT_BRACKETS }, { "max-bracket", required_argument, NULL, OPT_MAXBR },
		{ "timeout", required_argument, NULL, OPT_TIMEOUT }, { "log", required_argument, NULL, OPT_LOG },
		{ "json", no_argument, NULL, 'j' }, { "os-log", no_argument, NULL, OPT_OSLOG },
		{ "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "jh", opts, NULL)) != -1) {
		switch (c) {
		case 'j': cfg.json = true; break;
		case OPT_SOCK: cfg.sock_path = optarg; break;
		case OPT_INTERVAL: cfg.interval_s = arg_double("--interval", optarg, 0.01, MAX_INTERVAL_S); break;
		case OPT_PORT_RETRY: cfg.port_retry_s = arg_double("--port-retry", optarg, 0.1, MAX_PORT_RETRY_S); break;
		case OPT_UTC: cfg.utc_offset = (int)arg_uint("--utc-offset", optarg, 0, MAX_UTC_OFFSET); break;
		case OPT_SETTLE: cfg.settle_s = arg_double("--settle", optarg, 0, MAX_SETTLE_S); break;
		case OPT_BRACKETS: cfg.brackets = arg_uint("--brackets", optarg, 1, MAX_BRACKETS); break;
		case OPT_MAXBR: cfg.max_bracket_us = arg_double("--max-bracket", optarg, 0, MAX_BRACKET_US); break;
		case OPT_TIMEOUT: cfg.timeout_s = arg_double("--timeout", optarg, 0, MAX_TIMEOUT_S); break;
		case OPT_LOG: cfg.log_path = optarg; break;
		case OPT_OSLOG: cfg.os_log = true; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (argc - optind != 1) { usage(argv[0]); return 2; }
	cfg.interface = argv[optind];

	start_mach = mach_absolute_time();
	mach_timebase_info(&timebase);
	if (cfg.os_log) oslog = os_log_create(OS_LOG_SUBSYSTEM, "gptp-refclock");

	if (cfg.log_path) {
		log_out = fopen(cfg.log_path, "a");
		if (!log_out) { note("measurement log %s: %s", cfg.log_path, strerror(errno)); return 1; }
	}
	chrony = chrony_client_create(NULL, cfg.sock_path);
	if (!chrony) {
		note("cannot create the chrony client for %s: %s", cfg.sock_path, strerror(errno));
		if (log_out) fclose(log_out);
		return 1;
	}

	char err[256];
	ts = gptp_open((uint64_t)llround(cfg.timeout_s * 1e9), err, sizeof(err));
	if (!ts) {
		note("%s", err);
		chrony_client_destroy(chrony);
		if (log_out) fclose(log_out);
		return 1;
	}
	gptp_set_event_handler(ts, on_event, NULL);
	if (!gptp_port_add(ts, cfg.interface, err, sizeof(err))) {
		note("%s", err);
		gptp_close(ts);
		chrony_client_destroy(chrony);
		if (log_out) fclose(log_out);
		return 1;
	}
	{
		struct gptp_domain_state d; struct gptp_port_state p;
		gptp_domain_state(ts, &d); gptp_port_state(ts, &p);
		note("gPTP port %u on %s (timestamping mode %u, propagation delay limit %u ns); own clock identity %016" PRIx64 ", grandmaster %016" PRIx64 ", lock state %d; UTC offset %d s",
		     p.port_number, cfg.interface, p.local_timestamping_mode, p.propagation_delay_limit_ns, d.clock_identity, d.grandmaster_identity, d.lock_state, cfg.utc_offset);
		note("chrony samples to %s from %s", chrony_client_remote_path(chrony), chrony_client_local_path(chrony));
	}

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	uint64_t interval = seconds_to_mach(cfg.interval_s), next = mach_absolute_time();
	while (!stop_flag) {
		take_sample();
		do {
			next += interval;
		} while ((int64_t)(next - mach_absolute_time()) <= 0);
		for (;;) {
			if (stop_flag) break;
			int64_t left = (int64_t)(next - mach_absolute_time());
			if (left <= 0) break;
			int64_t ns = gptp_mach_to_ns(left);
			if (ns > POLL_SLEEP_NS) ns = POLL_SLEEP_NS;
			struct timespec rel = { .tv_sec = 0, .tv_nsec = (long)ns };
			nanosleep(&rel, NULL);
		}
	}

	drain_events();
	gptp_close(ts);		/* removes the port only if this process added it */
	chrony_client_destroy(chrony);
	if (log_out) fclose(log_out);

	note("summary: run time %.1f s; %" PRIu64 " samples, %" PRIu64 " sent to chrony, %" PRIu64 " with a wide bracket; %" PRIu64 " state changes, final state %s; %" PRIu64 " events",
	     since_start_s(), run.samples, run.sent, run.wide_brackets, run.state_changes, run.have_state ? state_name[run.state] : "none", ev.total);
	return 0;
}
