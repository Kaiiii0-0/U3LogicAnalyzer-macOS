/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 *
 * Copyright (C) 2026 u3ctl authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef U3CTL_U3CTL_H
#define U3CTL_U3CTL_H

#include <stdint.h>
#include <stdio.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include <libsigrokdecode/libsigrokdecode.h>

/* Driver name defaults, in the order they are tried for capture. */
#define U3CTL_DEFAULT_DRIVERS "wch-ch32h417-libusb,wch-ch32h417"

/* Default decoders path relative to the repo root (mirrors start_pv.sh). */
#define U3CTL_DEFAULT_DECODE_PATH "install/share/libsigrokdecode/decoders"

/*---- shared context ------------------------------------------------------*/

struct u3ctl_globals {
	struct sr_context *sr_ctx;
	struct sr_dev_driver **drivers; /* NULL-terminated array, sr_driver_list */
	int verbosity;
	char **argv;              /* process argv, for error reporting */
};

extern struct u3ctl_globals G;

/*---- u3ctl.c: infrastructure ------------------------------------------- */

int u3ctl_init(int loglevel);
void u3ctl_shutdown(void);
struct sr_dev_driver *find_driver(const char *name);
struct sr_dev_driver *find_default_capture_driver(void);
GSList *scan_driver_devices(struct sr_dev_driver *driver, const char *conn,
		GSList *scan_opts);
struct sr_dev_inst *pick_device(struct sr_dev_driver *driver, const char *conn,
		GSList *scan_opts);

/* JSON helpers: append to a GString; jqout() prints and exits. */
void json_str(GString *j, const char *s);
void json_kv_str(GString *j, const char *key, const char *val);
void json_kv_u64(GString *j, const char *key, uint64_t val);
void json_kv_i64(GString *j, const char *key, int64_t val);
void json_kv_f(GString *j, const char *key, double val);
void json_kv_bool(GString *j, const char *key, gboolean val);

/* Emit {"error": msg, ...} to stdout and exit(2). */
void die_json(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
/* Plain fatal error to stderr, exit(2). */
void die(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* SI parsing: "16M" -> 16000000, "512Ki" -> 524288, "1.2ms" handled elsewhere. */
int parse_si_u64(const char *s, uint64_t *out);
/* Time string -> samples: "50ms", "1.2us", "2s" at the given rate. */
int parse_time_samples(const char *s, uint64_t samplerate, uint64_t *out);
/* Window spec: sample number or time-at-rate. */
int parse_window(const char *s, uint64_t samplerate, uint64_t *out);

const char *trigger_match_name(int match);
int trigger_match_from_name(const char *name, int *match);

/*---- capture.c ---------------------------------------------------------- */

int cmd_capture(int argc, char **argv);
int cmd_devices_list(int argc, char **argv);
int cmd_capabilities(int argc, char **argv);

/*---- ipc.c ----------------------------------------------------------------- */

/* 0 = opened in the running GUI instance, 1 = no instance, <0 = error. */
int open_in_gui(const char *path);
void cmd_open_json(const char *path, int ret);
int cmd_open(int argc, char **argv);

/*---- query.c: drill-down queries over loaded sessions ---------------------- */

struct loaded_session;
struct sr_channel;

/* A logic sample word read little-endian out of the unitsize bytes. */
struct logic_sample {
	const uint8_t *data;
	int unitsize;
};

/* Pointer to sample v of the loaded session, or NULL out of range.
 * Chunks are contiguous by construction of replay_collect(). */
const uint8_t *sample_ptr(struct loaded_session *ls, uint64_t v);

/* Bit value of channel index in a sample word. */
int sample_bit(const uint8_t *data, int unitsize, int index);

/* Resolve a window: sample numbers or time literals at the session rate.
 * Missing bounds default to the whole capture. */
void resolve_window(struct loaded_session *ls, int argc, char **argv,
		uint64_t *from, uint64_t *to);

/* Extract the bare FILE.sr from an argv that also carries options. */
const char *query_arg_file(int argc, char **argv);

/* Resolve --channels D0,D1 (NULL = all enabled logic channels). */
GSList *resolve_chans(struct loaded_session *ls, int argc, char **argv);
const char *chan_name(struct sr_channel *ch);
int chan_index(struct sr_channel *ch);

int cmd_activity(int argc, char **argv);
int cmd_measure(int argc, char **argv);
int cmd_edges(int argc, char **argv);
int cmd_sample(int argc, char **argv);
int cmd_slice(int argc, char **argv);
int cmd_overview(int argc, char **argv);
int cmd_profile(int argc, char **argv);

/* Profile internals shared with nothing else. */
struct ch_stats {
	gboolean active;
	gboolean clock_like;
	double freq_hz;
	double duty;
	double seg_min_s, seg_max_s, seg_mean_s; /* level-segment widths, seconds */
	double pulse_regularity;   /* (max-min)/mean over segments, <1 = regular */
	uint64_t toggles;
	uint64_t first_edge, last_edge;
	int first_type, last_type;
};

void measure_channel(struct loaded_session *ls, int index,
		uint64_t from, uint64_t to, struct ch_stats *st);

/*---- render.c --------------------------------------------------------------- */

int cmd_render(int argc, char **argv);

/*---- sessionfile.c ------------------------------------------------------- */

int cmd_session_info(int argc, char **argv);
int cmd_sessions_list(int argc, char **argv);
int cmd_session_delete(int argc, char **argv);
int cmd_export(int argc, char **argv);

/* A loaded .sr session prepared for replay: session + first device +
 * samplerate + accumulated logic chunks. */
struct loaded_session {
	struct sr_session *session;
	struct sr_dev_inst *sdi;
	uint64_t samplerate;
	uint64_t total_samples;
	int unitsize;
	uint64_t trigger_pos;
	gboolean trigger_seen;
	GSList *chunks;           /* struct logic_chunk, collected on replay */
};

struct logic_chunk {
	uint8_t *data;
	uint64_t length;          /* bytes */
	uint64_t start_sample;    /* first sample number of this chunk */
};

int load_session_file(const char *path, struct loaded_session *ls);
void free_loaded_session(struct loaded_session *ls);

/*---- decode.c ------------------------------------------------------------ */

int cmd_decode(int argc, char **argv);
int cmd_decoders_list(int argc, char **argv);
int cmd_decoders_info(int argc, char **argv);

/* srd decoder path resolution (SIGROKDECODE_DIR → install prefix → lib
 * default); shared by decode and profile. */
const char *decoders_path(void);

/* Replay loaded session through datafeed, collecting logic chunks. */
int replay_collect(struct loaded_session *ls, GSList **chunks,
		uint64_t *total_samples);

#endif /* U3CTL_U3CTL_H */
