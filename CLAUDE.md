# CLAUDE.md

Guidance for Claude Code in this repository.
README.md says what is here; docs/libgptp.md describes the library, docs/gptp-refclock.md and docs/gptp-pps-offset.md the programs, and docs/timesync.md what the private framework does as observed; this file is about working on the code.
`CLAUDE.local.md`, not committed, has the machines, paths and run history of the author's own setup; read it if it exists.

## Layout

- `gptp.h`, `gptp.m`: the library, the only code that touches the private TimeSync framework.
  Objective-C with ARC inside, a C interface outside, built into `libgptp.a`.
  Every class and selector used is checked in `gptp_open` with `respondsToSelector`, and a failure names what is missing.
  Do not send a framework message from anywhere else.
- `gptp-refclock.c`: `gptp-refclock INTERFACE`; options, the sampling loop, the state machine that gates sending, the log record (one column table, so the space-separated form and the JSON form cannot disagree), the summary.
- `gptp-pps-offset.c`: `gptp-pps-offset INTERFACE DEVICE`, the CTS polling and the offset estimate for chrony's CTS refclock line; the polling code lives only here.
- `chrony-client.[ch]`: the chrony SOCK client; it knows nothing about gPTP.
- `tsdump.m`: lists a class's methods and type encodings from the running framework; the tool for fixing the library when macOS changes it.
- `PLAN.md`: the author's plan; not committed.

## Build

`make` builds `libgptp.a`, `gptp-refclock` and `gptp-pps-offset`; nothing but Foundation is linked, and the framework is opened with `dlopen` at run time.
Only `gptp.m` compiles with `-fobjc-arc`; everything else is C with `-std=gnu11`.
Warnings are on; keep the build clean.
File names use hyphens; C identifiers use the `gptp_` prefix, structs as `struct gptp_x` with no suffix, typedefs with `_t`.

## Conventions

- Anything that tunes behaviour is an option with a sensible name, not a constant.
- Medians for anything estimated from a handful of latency-type measurements.
- Do not keep old names as aliases when renaming.
- No attribution trailers in commits.
  Push nothing unless told to.
- Whoever runs this decides what runs under `sudo`; the programs say when they need it.
- Required things are positional arguments, not options; no option is required.
- Output: events go to stderr, or to the unified log (subsystem `com.jclark.gptp`) with `--os-log`, which a launchd plist passes; measurements go only to the file named by `--log`, space-separated columns as chrony's logs or JSON lines with `-j`; nothing goes to stdout unless the program's product is a value, as gptp-pps-offset's is.
  The framework's callback thread never writes; it records, and the main loop reports.

## chrony sign convention

`chrony_client_send_sample` sends a complete sample: pulse 0, offset = true time minus system time, leap flag.
Here true time is gPTP time minus `--utc-offset`.
The refclock line must not have `pps` or `offset`.

## Prose

The README and everything under `docs/` are in the voice of one engineer explaining to a capable peer.
Use plain declarative sentences, British spelling with -ize, ASCII only and headings in sentence case.
Put one sentence on each Markdown line.
Use "I" only for something the author personally did, measured or judged.
Avoid marketing adjectives, exclamation marks, announcing openers and closing summaries.
The following points record the problems that have needed correction in these documents.

- Sentence structure.
  Avoid stacking a main clause, an appositive, a participial phrase, a qualification and another thought into one sentence.
  Let each sentence carry one natural step in the reasoning.
  A longer sentence is appropriate when its parts form one coherent step.
  Do not turn the same material into a mechanical sequence of short sentences.
  For example, a description of a test setup, the agreement between mapping samples, the result of a GPS pulse check, the absolute accuracy and the remaining uncertainty are separate steps in the reasoning.
  Write them as "My test used a direct gigabit cable", "Samples of the mapping agreed to about 250 ns", and so on, rather than hanging all five from one opening clause.
  But do not split a single idea into "The framework is private. It is undocumented. Another release may change it." when "The framework is private and undocumented, and another macOS release may change it" expresses the idea naturally.
- Write each paragraph from its facts, in the order the reader needs them, rather than editing sentence by sentence.
  Replacing semicolons with full stops, or splitting one thought into a run of short sentences, produces the mechanical cadence that is the problem.
  Vary sentence length naturally.
- Lead with the practical fact, then the mechanism.
  State a requirement as what the reader must provide or configure, not as an implementation detail.
  Do not combine unrelated points, such as the requirements for bringing up a link and the accuracy measured with one particular setup.
  For example, say that the Ethernet adapter must support AVB before explaining that macOS enables gPTP only on such adapters.
- Prefer a named subject when a pronoun would be vague.
  "It", "That" and "This" are natural when their antecedent is clear.
  Avoid introductory modifiers that do not attach cleanly to the subject, such as "Run unprivileged, the program...".
  Write "Without root privileges, the program still runs" instead.
- Describe technical objects literally.
  A mapping does not "know" where a second is; it locates the second to within some error.
  Do not say that gPTP synchronizes the Mac's system clock.
  The kernel maintains a virtual clock synchronized to the grandmaster's time, while the system clock remains unchanged.
- Attach a qualification to the claim it qualifies, and add "because", "therefore" or "so that" only where the relation is real and worth stating.
  Preserve distinctions, and do not invent causes.
- Integrate cross-references into ordinary sentences with descriptive Markdown links.
  Avoid filler such as "See X for the interface", "`docs/foo.md` describes it" and "That is the option...".
  "The runs are described in [gptp-refclock](docs/gptp-refclock.md)" is a useful sentence because it tells the reader what the linked page contains.
- Keep precise terms precise: "samples of the mapping", not "samples of the clock"; a "fixed" limit, not a "configured" one when the user cannot set it.
  Say which class or selector is missing, not which "interface".
- Prefer the direct formulation: "chrony compensates for this fixed latency using the `offset` option on the refclock line" rather than "that latency is a constant, and chrony's `offset` is where it is declared"; "Without root privileges, the program still runs but cannot deliver samples to chrony. It reports this once." rather than "Run unprivileged, it does everything but deliver the samples, and says so once."
- Refer to another project when it is relevant to the technical explanation, but do not include unrelated project history as background.
- After writing, read each paragraph as if aloud and ask whether an experienced engineer would say it that way to another; if a sentence reads as a transformation of source text, rewrite it from its meaning.
