/* SPDX-License-Identifier: MIT */
/* gptp-pps-offset: measure the latency of the serial PPS polling path
 * against the Mac's gPTP clock, and print the value for chrony's "offset"
 * option on the CTS refclock line.
 *
 * The GPS pulse deasserts the CTS pin of a serial port.  satpulse's serial
 * mode finds it by polling ioctl(TIOCMGET) and places the edge midway
 * between the last poll that saw the pin asserted and the first that saw it
 * clear; the edge is therefore seen late by the path from the pin through
 * the USB adapter to the poll, and chrony's "offset" is where that latency
 * is declared.  This program polls the same way, but timestamps the edge in
 * mach absolute time and converts it through the kernel's gPTP mapping,
 * which knows where the UTC second is to well under a microsecond.  The
 * edge's position after the gPTP second, median of every edge in the run,
 * is the latency, and it goes to stdout in seconds.
 *
 * Each edge is only known to half its polling bracket, about 45 us on a
 * quiet Mac, so the run length sets the precision: about 3 us after ten
 * minutes, about 1.5 us after an hour.  Below that the number is limited by
 * the mapping's own fixed offset, which nothing on the Mac can measure.
 *
 * Needs no privilege.  If gptp-refclock is running, its gPTP port is
 * used; otherwise one is added for the run. */
#include <errno.h>
#include <fcntl.h>
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
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include "gptp.h"

#define NS_PER_S 1000000000LL

#define DEFAULT_TIME_S 600.0		/* -t: about 600 edges, median good to about 3 us */
#define DEFAULT_WINDOW_US 100000.0	/* either side of the predicted pulse */
#define DEFAULT_MAX_UNCERTAINTY_US 150.0	/* half the bracket; about 45 us when the Mac is quiet */
#define DEFAULT_UTC_OFFSET 37		/* TAI - UTC since 2017-01-01 */
#define DEFAULT_REPORT_S 60.0
#define DEFAULT_TIMEOUT_S 2.0		/* waiting for the TimeSync daemon */
#define DEFAULT_WINDOW_MARGIN_US 50000.0	/* minimum lead before a polling window */
#define DEFAULT_STATE_INTERVAL_S 0.5	/* between state checks while not locked */
#define WAIT_SLICE_NS 100000000		/* bounds signal-response latency while sleeping */
#define MAX_TIME_S 604800.0
#define MAX_WINDOW_US 500000.0
#define MAX_UNCERTAINTY_US 500000.0
#define MAX_REPORT_S 86400.0
#define MAX_TIMEOUT_S 60.0
#define MAX_WINDOW_MARGIN_US 500000.0
#define MAX_STATE_INTERVAL_S 60.0
#define MAX_UTC_OFFSET 1000U

static struct config {
	const char *interface, *device, *log_path;
	double time_s, window_us, max_uncertainty_us, report_s, timeout_s;
	double window_margin_us, state_interval_s;
	int utc_offset;
	bool json, debug;
} cfg = {
	.time_s = DEFAULT_TIME_S, .window_us = DEFAULT_WINDOW_US, .max_uncertainty_us = DEFAULT_MAX_UNCERTAINTY_US,
	.report_s = DEFAULT_REPORT_S, .timeout_s = DEFAULT_TIMEOUT_S, .utc_offset = DEFAULT_UTC_OFFSET,
	.window_margin_us = DEFAULT_WINDOW_MARGIN_US, .state_interval_s = DEFAULT_STATE_INTERVAL_S,
};

static gptp_t *ts;
static int fd = -1;
static FILE *log_out;
static uint64_t start_mach;
static volatile sig_atomic_t stop_flag;

static struct {
	double *pos_us, *unc_us;	/* every usable edge */
	size_t n, cap;
	unsigned windows, empty, rejected;
	bool locked, have_state;	/* the last state check */
	bool port_retry_failing;
	bool header_done;
} run;

/* ---- helpers ------------------------------------------------------------ */

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
	va_list ap;
	int64_t now_ns = (int64_t)clock_gettime_nsec_np(CLOCK_REALTIME);
	time_t secs = (time_t)(now_ns / NS_PER_S);
	struct tm tm;
	gmtime_r(&secs, &tm);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s.%03ld ", stamp, (long)(now_ns % NS_PER_S / 1000000));
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* the q quantile of a copy of v, so the caller's order is kept */
static double quantile(const double *v, size_t n, double q)
{
	double *c = malloc(n * sizeof(*c));
	if (!c) return NAN;
	memcpy(c, v, n * sizeof(*c));
	qsort(c, n, sizeof(*c), cmp_double);
	double pos = q * (double)(n - 1);
	size_t lower = (size_t)pos;
	size_t upper = lower + (lower + 1 < n);
	double r = c[lower] + (c[upper] - c[lower]) * (pos - (double)lower);
	free(c);
	return r;
}

/* ---- the serial port ---------------------------------------------------- */

static bool open_device(void)
{
	struct termios tios;
	fd = open(cfg.device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { fprintf(stderr, "%s: %s\n", cfg.device, strerror(errno)); return false; }
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		fprintf(stderr, "%s: %s\n", cfg.device, errno == EWOULDBLOCK ? "in use by another process" : strerror(errno));
		return false;
	}
	if (tcgetattr(fd, &tios) < 0) { fprintf(stderr, "%s is not a serial port\n", cfg.device); return false; }
	cfmakeraw(&tios);
	tios.c_cflag |= CLOCAL;
	if (tcsetattr(fd, TCSANOW, &tios) < 0) { fprintf(stderr, "tcsetattr %s: %s\n", cfg.device, strerror(errno)); return false; }
	return true;
}

/* sleep until a mach time, in short pieces so a signal ends it promptly */
static bool wait_until(uint64_t deadline)
{
	while (!stop_flag) {
		int64_t left = (int64_t)(deadline - mach_absolute_time());
		if (left <= 0) return true;
		int64_t ns = gptp_mach_to_ns(left);
		if (ns > WAIT_SLICE_NS) ns = WAIT_SLICE_NS;
		struct timespec rel = { .tv_sec = 0, .tv_nsec = (long)ns };
		nanosleep(&rel, NULL);
	}
	return false;
}

enum poll_result { POLL_STOPPED, POLL_EMPTY, POLL_CAUGHT, POLL_ERROR };

/* Poll the pin back to back from open until an edge or close.  The edge is
 * midway between the last read that saw CTS asserted and the first that saw
 * it clear, each read placed at the midpoint of its own mach bracket. */
static enum poll_result poll_edge(uint64_t open_time, uint64_t close_time, uint64_t *edge, uint64_t *bracket, int64_t *read_realtime_ns)
{
	bool have_prev = false, prev_cts = false;
	uint64_t prev_mid = 0;
	int status;
	if (!wait_until(open_time)) return POLL_STOPPED;
	for (;;) {
		uint64_t before = mach_absolute_time();
		if (before >= close_time) return POLL_EMPTY;
		if (stop_flag) return POLL_STOPPED;
		if (ioctl(fd, TIOCMGET, &status) < 0) return POLL_ERROR;
		uint64_t mid = before + (mach_absolute_time() - before) / 2;
		bool cts = (status & TIOCM_CTS) != 0;
		if (have_prev && prev_cts && !cts) {
			*edge = prev_mid + (mid - prev_mid) / 2;
			*bracket = mid - prev_mid;
			*read_realtime_ns = (int64_t)clock_gettime_nsec_np(CLOCK_REALTIME);
			return POLL_CAUGHT;
		}
		have_prev = true; prev_cts = cts; prev_mid = mid;
	}
}

/* ---- the gPTP side ------------------------------------------------------- */

/* Whether the domain follows an external grandmaster on the PTP timescale;
 * the reason is reported when it changes. */
static bool domain_locked(void)
{
	struct gptp_domain_state d; struct gptp_port_state p; struct gptp_time t;
	gptp_port_state(ts, &p);
	if (!p.present) {
		char err[256];
		if (!gptp_port_add(ts, cfg.interface, err, sizeof(err))) {
			if (!run.port_retry_failing) note("%s; will retry", err);
			run.port_retry_failing = true;
		} else {
			note("gPTP port on %s is available again", cfg.interface);
			run.port_retry_failing = false;
			gptp_port_state(ts, &p);
		}
	} else if (run.port_retry_failing) {
		note("gPTP port on %s is available again", cfg.interface);
		run.port_retry_failing = false;
	}
	gptp_domain_state(ts, &d);
	const char *why = NULL;
	if (!p.present) why = "no gPTP port on the interface";
	else if (!p.enabled || !p.as_capable) why = "the port has no gPTP peer";
	else if (d.grandmaster_identity == d.clock_identity) why = "the Mac is its own grandmaster";
	else if (d.lock_state != GPTP_LOCK_LOCKED) why = "the domain is not locked";
	else if (!gptp_time_from_mach(ts, mach_absolute_time(), &t) || !t.ptp_timescale || t.grandmaster_identity != d.grandmaster_identity)
		why = "the domain is not on the PTP timescale";
	bool locked = why == NULL;
	if (!run.have_state || locked != run.locked) {
		if (locked) note("locked to grandmaster %016" PRIx64 ", propagation delay %u ns; polling", d.grandmaster_identity, p.propagation_delay_ns);
		else note("waiting: %s", why);
	}
	run.locked = locked; run.have_state = true;
	return locked;
}

static bool gptp_ns_at(uint64_t mach, int64_t *ns)
{
	struct gptp_time t;
	if (!gptp_time_from_mach(ts, mach, &t)) return false;
	*ns = gptp_time_to_ns(&t);
	return true;
}

/* ---- recording --------------------------------------------------------- */

/* One edge record, in the shape of satpulsetool sdp's extts event: the
 * edge's time on the gPTP clock as TAI seconds and nanoseconds, the system
 * time of the read that saw it as RFC 3339 UTC, and the bracket half-width
 * in seconds.  The space-separated form puts the read's date and time
 * first, as chrony's logs do. */
static void log_edge(int64_t edge_ns, int64_t read_realtime_ns, double unc_us)
{
	if (!log_out) return;
	time_t secs = (time_t)(read_realtime_ns / NS_PER_S);
	struct tm tm;
	gmtime_r(&secs, &tm);
	char date[16], clock[32];
	strftime(date, sizeof(date), "%Y-%m-%d", &tm);
	strftime(clock, sizeof(clock), "%H:%M:%S", &tm);
	long usec = (long)(read_realtime_ns % NS_PER_S / 1000);
	if (cfg.json) {
		fprintf(log_out, "{\"timestamp\":\"%" PRId64 ".%09" PRId64 "\",\"tRead\":\"%sT%s.%06ldZ\",\"uncertainty\":%.6f}\n",
			edge_ns / NS_PER_S, edge_ns % NS_PER_S, date, clock, usec, unc_us * 1e-6);
	} else {
		if (!run.header_done) { fputs("# date_utc time_utc timestamp_tai uncertainty\n", log_out); run.header_done = true; }
		fprintf(log_out, "%s %s.%06ld %" PRId64 ".%09" PRId64 " %.6f\n", date, clock, usec, edge_ns / NS_PER_S, edge_ns % NS_PER_S, unc_us * 1e-6);
	}
	fflush(log_out);
}

static bool record(double pos_us, double unc_us)
{
	if (run.n == run.cap) {
		size_t ncap = run.cap ? run.cap * 2 : 1024;
		double *p = realloc(run.pos_us, ncap * sizeof(*p));
		if (!p) return false;
		run.pos_us = p;
		double *u = realloc(run.unc_us, ncap * sizeof(*u));
		if (!u) return false;
		run.unc_us = u;
		run.cap = ncap;
	}
	run.pos_us[run.n] = pos_us; run.unc_us[run.n] = unc_us; run.n++;
	return true;
}

static void report(const char *what)
{
	if (run.n)
		note("%s: edge %+.1f us after the gPTP second (median of %zu, quartiles %+.1f/%+.1f, minimum %+.1f, uncertainty median %.1f us); %u rejected, %u empty of %u windows",
		     what, quantile(run.pos_us, run.n, 0.5), run.n, quantile(run.pos_us, run.n, 0.25), quantile(run.pos_us, run.n, 0.75),
		     quantile(run.pos_us, run.n, 0.0), quantile(run.unc_us, run.n, 0.5), run.rejected, run.empty, run.windows);
	else
		note("%s: no usable edge yet; %u rejected, %u empty of %u windows", what, run.rejected, run.empty, run.windows);
}

/* ---- main ---------------------------------------------------------------- */

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static void usage(const char *prog)
{
	fprintf(stderr,
"Usage: %s [options] INTERFACE DEVICE\n"
"Measure how late the GPS pulse on DEVICE's CTS pin is seen by polling, against the\n"
"Mac's gPTP clock on INTERFACE, and print chrony's offset for the CTS refclock line.\n"
"  -t SECONDS              how long to collect edges (%g); an hour gets about 1.5 us\n"
"      --window US         poll either side of the predicted second (%g)\n"
"      --window-margin US  minimum lead before opening a polling window (%g)\n"
"      --max-uncertainty US  reject an edge whose half bracket exceeds this (%g)\n"
"      --utc-offset N      TAI minus UTC in seconds (%d)\n"
"      --report SECONDS    progress on stderr this often (%g)\n"
"      --state-interval S  seconds between state checks while waiting (%g)\n"
"      --log PATH          one record per edge: read time (UTC), edge time (TAI), uncertainty\n"
"  -j, --json              the record file as JSON lines, as satpulsetool sdp -j writes them\n"
"      --timeout SECONDS   wait this long for the TimeSync daemon (%g)\n"
"      --debug             a note per edge\n"
"  -h, --help\n"
"Output: the median position of the edge after the gPTP second, in seconds, on stdout.\n",
		prog, DEFAULT_TIME_S, DEFAULT_WINDOW_US, DEFAULT_WINDOW_MARGIN_US, DEFAULT_MAX_UNCERTAINTY_US,
		DEFAULT_UTC_OFFSET, DEFAULT_REPORT_S, DEFAULT_STATE_INTERVAL_S, DEFAULT_TIMEOUT_S);
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
	enum { OPT_WINDOW = 256, OPT_WINDOW_MARGIN, OPT_MAXUNC, OPT_UTC, OPT_REPORT, OPT_STATE_INTERVAL,
	       OPT_LOG, OPT_TIMEOUT, OPT_DEBUG };
	static const struct option opts[] = {
		{ "time", required_argument, NULL, 't' }, { "window", required_argument, NULL, OPT_WINDOW },
		{ "window-margin", required_argument, NULL, OPT_WINDOW_MARGIN },
		{ "max-uncertainty", required_argument, NULL, OPT_MAXUNC }, { "utc-offset", required_argument, NULL, OPT_UTC },
		{ "report", required_argument, NULL, OPT_REPORT }, { "state-interval", required_argument, NULL, OPT_STATE_INTERVAL },
		{ "log", required_argument, NULL, OPT_LOG }, { "json", no_argument, NULL, 'j' },
		{ "timeout", required_argument, NULL, OPT_TIMEOUT }, { "debug", no_argument, NULL, OPT_DEBUG },
		{ "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "t:jh", opts, NULL)) != -1) {
		switch (c) {
		case 't': cfg.time_s = arg_double("-t", optarg, 1, MAX_TIME_S); break;
		case OPT_WINDOW: cfg.window_us = arg_double("--window", optarg, 1, MAX_WINDOW_US); break;
		case OPT_WINDOW_MARGIN: cfg.window_margin_us = arg_double("--window-margin", optarg, 0, MAX_WINDOW_MARGIN_US); break;
		case OPT_MAXUNC: cfg.max_uncertainty_us = arg_double("--max-uncertainty", optarg, 0, MAX_UNCERTAINTY_US); break;
		case OPT_UTC: cfg.utc_offset = (int)arg_uint("--utc-offset", optarg, 0, MAX_UTC_OFFSET); break;
		case OPT_REPORT: cfg.report_s = arg_double("--report", optarg, 1, MAX_REPORT_S); break;
		case OPT_STATE_INTERVAL: cfg.state_interval_s = arg_double("--state-interval", optarg, 0.01, MAX_STATE_INTERVAL_S); break;
		case OPT_LOG: cfg.log_path = optarg; break;
		case 'j': cfg.json = true; break;
		case OPT_TIMEOUT: cfg.timeout_s = arg_double("--timeout", optarg, 0, MAX_TIMEOUT_S); break;
		case OPT_DEBUG: cfg.debug = true; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (argc - optind != 2) { usage(argv[0]); return 2; }
	cfg.interface = argv[optind];
	cfg.device = argv[optind + 1];
	if (cfg.window_us * 1e3 >= NS_PER_S / 2) { fprintf(stderr, "--window must be under half a second\n"); return 2; }
	if ((cfg.window_us + cfg.window_margin_us) * 1e3 >= NS_PER_S) {
		fprintf(stderr, "--window and --window-margin must total less than one second\n");
		return 2;
	}

	start_mach = mach_absolute_time();
	if (!open_device()) return 1;
	if (cfg.log_path) {
		log_out = fopen(cfg.log_path, "a");
		if (!log_out) { fprintf(stderr, "%s: %s\n", cfg.log_path, strerror(errno)); return 1; }
	}
	char err[256];
	ts = gptp_open((uint64_t)llround(cfg.timeout_s * 1e9), err, sizeof(err));
	if (!ts) { fprintf(stderr, "%s\n", err); return 1; }
	if (!gptp_port_add(ts, cfg.interface, err, sizeof(err))) { fprintf(stderr, "%s\n", err); gptp_close(ts); return 1; }
	note("polling %s against gPTP on %s for %g s: window +/-%g us, UTC offset %d s", cfg.device, cfg.interface, cfg.time_s, cfg.window_us, cfg.utc_offset);

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	int64_t window_ns = (int64_t)llround(cfg.window_us * 1e3);
	int64_t window_margin_ns = (int64_t)llround(cfg.window_margin_us * 1e3);
	int64_t utc_off_ns = (int64_t)cfg.utc_offset * NS_PER_S;
	uint64_t deadline = start_mach + gptp_ns_to_mach((int64_t)llround(cfg.time_s * NS_PER_S));
	uint64_t next_report = start_mach + gptp_ns_to_mach((int64_t)llround(cfg.report_s * NS_PER_S));
	bool failed = false;
	while (!stop_flag && mach_absolute_time() < deadline) {
		if (mach_absolute_time() >= next_report) {
			report("progress");
			next_report = mach_absolute_time() + gptp_ns_to_mach((int64_t)llround(cfg.report_s * NS_PER_S));
		}
		if (!domain_locked()) {
			wait_until(mach_absolute_time() + gptp_ns_to_mach((int64_t)llround(cfg.state_interval_s * NS_PER_S)));
			continue;
		}
		/* the next UTC second by gPTP, far enough away for the window to open */
		int64_t now_ns;
		if (!gptp_ns_at(mach_absolute_time(), &now_ns)) {
			wait_until(mach_absolute_time() + gptp_ns_to_mach((int64_t)llround(cfg.state_interval_s * NS_PER_S)));
			continue;
		}
		int64_t utc_ns = now_ns - utc_off_ns;
		int64_t next_s = utc_ns / NS_PER_S + 1;
		if (next_s * NS_PER_S - utc_ns < window_ns + window_margin_ns) next_s++;
		int64_t target = next_s * NS_PER_S + utc_off_ns;		/* the pulse, in domain time */
		uint64_t open_time = gptp_mach_from_domain(ts, (uint64_t)(target - window_ns));
		uint64_t close_time = gptp_mach_from_domain(ts, (uint64_t)(target + window_ns));
		if (close_time > deadline) break;
		uint64_t edge, bracket; int64_t read_realtime_ns = 0;
		enum poll_result r = poll_edge(open_time, close_time, &edge, &bracket, &read_realtime_ns);
		if (r == POLL_STOPPED) break;
		if (r == POLL_ERROR) { note("ioctl(TIOCMGET) on %s: %s", cfg.device, strerror(errno)); failed = true; break; }
		run.windows++;
		if (r == POLL_EMPTY) { run.empty++; if (cfg.debug) note("no edge in the window"); continue; }
		double unc_us = (double)gptp_mach_to_ns((int64_t)bracket) / 2 * 1e-3;
		if (unc_us > cfg.max_uncertainty_us) {
			run.rejected++;
			if (cfg.debug) note("edge rejected: uncertainty %.1f us", unc_us);
			continue;
		}
		int64_t edge_ns;
		if (!gptp_ns_at(edge, &edge_ns)) {
			run.rejected++;
			if (cfg.debug) note("edge rejected: gPTP conversion failed");
			continue;
		}
		double pos_us = (double)(edge_ns - target) * 1e-3;
		if (!record(pos_us, unc_us)) { note("out of memory"); failed = true; break; }
		log_edge(edge_ns, read_realtime_ns, unc_us);
		if (cfg.debug) note("edge %+.1f us after the gPTP second, uncertainty %.1f us", pos_us, unc_us);
	}

	report(failed ? "failed" : stop_flag ? "stopped" : "done");
	gptp_close(ts);
	close(fd);
	if (log_out) fclose(log_out);
	if (failed) return 1;
	if (!run.n) { fprintf(stderr, "no usable edge: nothing to report\n"); return 1; }
	printf("%.1e\n", quantile(run.pos_us, run.n, 0.5) * 1e-6);
	return 0;
}
