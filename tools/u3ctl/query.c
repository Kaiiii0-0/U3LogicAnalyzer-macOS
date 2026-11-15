/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * P2 drill-down queries over loaded sessions: activity, measure, edges,
 * sample, slice, overview, profile. All read-only, all windowed, all JSON.
 */

#include "u3ctl.h"

#include <math.h>

/*---- sample access --------------------------------------------------------- */

const uint8_t *sample_ptr(struct loaded_session *ls, uint64_t v)
{
	for (GSList *l = ls->chunks; l; l = l->next) {
		struct logic_chunk *c = l->data;
		uint64_t n = c->length / (uint64_t)ls->unitsize;
		if (v >= c->start_sample && v < c->start_sample + n)
			return c->data + (v - c->start_sample) * (uint64_t)ls->unitsize;
	}
	return NULL;
}

int sample_bit(const uint8_t *data, int unitsize, int index)
{
	uint32_t w = 0;

	memcpy(&w, data, (size_t)unitsize);
	return (int)((w >> index) & 1);
}

/*---- shared helpers --------------------------------------------------------- */

static const char *arg_file_current;

static void load_or_die(const char *path, struct loaded_session *ls)
{
	if (!path)
		die_json("missing FILE.sr argument");
	if (load_session_file(path, ls) != SR_OK)
		die_json("cannot load session file '%s'", path);
	/* Pull the stored logic data into memory before scanning it. */
	if (replay_collect(ls, NULL, NULL) != SR_OK)
		die_json("failed to replay session '%s'", path);
	if (!ls->chunks)
		die_json("session '%s' contains no logic data", path);
	arg_file_current = path;
}

/* Options that take a value (used to skip over them when hunting the FILE). */
static gboolean opt_has_value(const char *a)
{
	static const char *valued[] = {
		"--from", "--to", "--channels", "--bucket", "--max-buckets",
		"--points", "--max", "--type", "--at", "--format",
		"--width", "--row-height", "--out",
	};

	for (size_t k = 0; k < sizeof(valued) / sizeof(valued[0]); k++)
		if (!strcmp(a, valued[k]))
			return TRUE;
	return FALSE;
}

/* Skip option-value pairs so the bare FILE.sr stands out. */
const char *query_arg_file(int argc, char **argv)
{
	for (int i = 0; i < argc; i++) {
		if (!g_str_has_prefix(argv[i], "--"))
			return argv[i];
		if (opt_has_value(argv[i]))
			i++; /* skip the value too */
	}
	return NULL;
}

#define arg_file(argc, argv) query_arg_file(argc, argv)

void resolve_window(struct loaded_session *ls, int argc, char **argv,
		uint64_t *from, uint64_t *to)
{
	*from = 0;
	*to = ls->total_samples;
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--from") && i + 1 < argc) {
			if (parse_window(argv[i + 1], ls->samplerate, from) != 0)
				die_json("bad --from '%s'", argv[i + 1]);
			i++;
		} else if (!strcmp(argv[i], "--to") && i + 1 < argc) {
			if (parse_window(argv[i + 1], ls->samplerate, to) != 0)
				die_json("bad --to '%s'", argv[i + 1]);
			i++;
		}
	}
	if (*from >= *to)
		die_json("empty window (from %llu >= to %llu)",
				(unsigned long long)*from, (unsigned long long)*to);
	if (*to > ls->total_samples)
		*to = ls->total_samples;
}

GSList *resolve_chans(struct loaded_session *ls, int argc, char **argv)
{
	GSList *out = NULL;
	const char *csv = NULL;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--channels") && i + 1 < argc) {
			csv = argv[i + 1];
			i++;
		}
	}

	GSList *all = sr_dev_inst_channels_get(ls->sdi);
	if (!csv) {
		for (GSList *l = all; l; l = l->next) {
			struct sr_channel *ch = l->data;
			if (ch->type == SR_CHANNEL_LOGIC && ch->enabled)
				out = g_slist_append(out, ch);
		}
		return out;
	}

	gchar **names = g_strsplit(csv, ",", 0);
	for (gchar **n = names; *n; n++) {
		struct sr_channel *found = NULL;
		for (GSList *l = all; l; l = l->next) {
			struct sr_channel *ch = l->data;
			if (ch->type == SR_CHANNEL_LOGIC && !strcmp(ch->name, *n)) {
				found = ch;
				break;
			}
		}
		if (!found)
			die_json("channel '%s' not found in session", *n);
		out = g_slist_append(out, found);
	}
	g_strfreev(names);
	return out;
}

const char *chan_name(struct sr_channel *ch)
{
	return ch->name ? ch->name : "?";
}

int chan_index(struct sr_channel *ch)
{
	return ch->index;
}

static void json_window(GString *j, struct loaded_session *ls,
		uint64_t from, uint64_t to)
{
	json_kv_str(j, "file", arg_file_current ? arg_file_current : "");
	g_string_append_c(j, ',');
	json_kv_u64(j, "samplerate", ls->samplerate);
	g_string_append_c(j, ',');
	json_kv_u64(j, "from_sample", from);
	g_string_append_c(j, ',');
	json_kv_u64(j, "to_sample", to);
	g_string_append_c(j, ',');
	json_kv_f(j, "from_s", ls->samplerate ? (double)from / ls->samplerate : 0);
	g_string_append_c(j, ',');
	json_kv_f(j, "to_s", ls->samplerate ? (double)to / ls->samplerate : 0);
}

/*---- activity ---------------------------------------------------------------- */

int cmd_activity(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t from, to, bucket = 0, span, nb;
	int max_buckets = 512;
	GSList *chans, *l;
	uint8_t *prev;
	uint32_t **counts;
	int i, nch;

	load_or_die(arg_file(argc, argv), &ls);
	resolve_window(&ls, argc, argv, &from, &to);
	chans = resolve_chans(&ls, argc, argv);
	if (!chans)
		die_json("no logic channels");

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--bucket") && i + 1 < argc) {
			if (parse_si_u64(argv[i + 1], &bucket) != 0 || !bucket)
				die_json("bad --bucket");
			max_buckets = INT_MAX; /* explicit bucket size wins */
			i++;
		} else if (!strcmp(argv[i], "--max-buckets") && i + 1 < argc) {
			max_buckets = atoi(argv[i + 1]);
			if (max_buckets <= 0)
				max_buckets = 512;
			i++;
		}
	}

	span = to - from;
	if (max_buckets == INT_MAX) {
		nb = (span + bucket - 1) / bucket;
	} else {
		bucket = (span + (uint64_t)max_buckets - 1) / (uint64_t)max_buckets;
		if (bucket == 0)
			bucket = 1;
		nb = (span + bucket - 1) / bucket;
	}

	nch = g_slist_length(chans);
	prev = g_malloc0(nch);
	counts = g_new0(uint32_t *, nch);
	for (i = 0; i < nch; i++)
		counts[i] = g_malloc0(sizeof(uint32_t) * nb);

	i = 0;
	for (l = chans; l; l = l->next, i++) {
		const uint8_t *s = sample_ptr(&ls, from);
		prev[i] = (uint8_t)sample_bit(s, ls.unitsize, chan_index(l->data));
	}

	for (uint64_t v = from + 1; v < to; v++) {
		const uint8_t *s = sample_ptr(&ls, v);
		i = 0;
		for (l = chans; l; l = l->next, i++) {
			int cur = sample_bit(s, ls.unitsize, chan_index(l->data));
			if (cur != prev[i]) {
				counts[i][(v - from) / bucket]++;
				prev[i] = (uint8_t)cur;
			}
		}
	}

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_window(j, &ls, from, to);
	g_string_append_c(j, ',');
	json_kv_u64(j, "bucket_samples", bucket);
	g_string_append_c(j, ',');
	json_kv_u64(j, "buckets", nb);
	g_string_append_c(j, ',');
	g_string_append(j, "\"channels\":{");
	gboolean first = TRUE;
	i = 0;
	for (l = chans; l; l = l->next, i++) {
		g_string_append(j, first ? "" : ",");
		json_str(j, chan_name(l->data));
		g_string_append(j, ":[");
		for (uint64_t b = 0; b < nb; b++)
			g_string_append_printf(j, "%s%u", b ? "," : "", counts[i][b]);
		g_string_append(j, "]");
		first = FALSE;
	}
	g_string_append(j, "}}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);

	for (i = 0; i < nch; i++)
		g_free(counts[i]);
	g_free(counts);
	g_free(prev);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}

/*---- measure ------------------------------------------------------------------ */

void measure_channel(struct loaded_session *ls, int index,
		uint64_t from, uint64_t to, struct ch_stats *st)
{
	uint64_t v, seg_start;
	int cur, prev_level = -1;
	double high_time = 0;
	double seg_min = 0, seg_max = 0, seg_sum = 0, seg_count = 0;
	double period_sum = 0, period_count = 0;
	uint64_t last_rising = 0;

	memset(st, 0, sizeof(*st));

	for (v = from; v < to; v++) {
		const uint8_t *s = sample_ptr(ls, v);
		if (!s)
			break;
		cur = sample_bit(s, ls->unitsize, index);
		if (prev_level == -1) {
			prev_level = cur;
			seg_start = v;
			continue;
		}
		if (cur != prev_level) {
			/* Segment [seg_start, v) held prev_level; edge at v. */
			double len = (double)(v - seg_start);

			if (seg_count == 0) {
				seg_min = seg_max = len;
			} else {
				if (len < seg_min) seg_min = len;
				if (len > seg_max) seg_max = len;
			}
			seg_sum += len;
			seg_count++;
			if (prev_level)
				high_time += len;

			int type = cur ? SR_TRIGGER_RISING : SR_TRIGGER_FALLING;
			if (st->toggles == 0) {
				st->first_edge = v;
				st->first_type = type;
			}
			st->last_edge = v;
			st->last_type = type;
			st->toggles++;

			if (cur) {
				if (last_rising && v > last_rising) {
					period_sum += (double)(v - last_rising);
					period_count++;
				}
				last_rising = v;
			}
			prev_level = cur;
			seg_start = v;
		}
	}
	/* Trailing segment reaching the window end. */
	if (seg_start < to) {
		double len = (double)(to - seg_start);

		if (seg_count == 0) {
			seg_min = seg_max = len;
		} else {
			if (len < seg_min) seg_min = len;
			if (len > seg_max) seg_max = len;
		}
		seg_sum += len;
		seg_count++;
		if (prev_level == 1)
			high_time += len;
	}

	double span_t = (double)(to - from);
	double srate = (double)ls->samplerate;

	st->duty = span_t > 0 ? high_time / span_t : 0;
	st->freq_hz = period_count > 0
		? srate / (period_sum / period_count) : 0;
	st->seg_min_s = seg_count ? seg_min / srate : 0;
	st->seg_max_s = seg_count ? seg_max / srate : 0;
	st->seg_mean_s = seg_count ? seg_sum / seg_count / srate : 0;
	st->pulse_regularity = (seg_count > 1 && seg_sum / seg_count > 0)
		? (seg_max - seg_min) / (seg_sum / seg_count) : 0;
	st->active = st->toggles >= 4;
	st->clock_like = st->active && period_count >= 8 &&
		st->duty >= 0.3 && st->duty <= 0.7 && st->pulse_regularity < 0.6;
}

int cmd_measure(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t from, to;
	GSList *chans, *l;

	load_or_die(arg_file(argc, argv), &ls);
	resolve_window(&ls, argc, argv, &from, &to);
	chans = resolve_chans(&ls, argc, argv);

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_window(j, &ls, from, to);
	g_string_append_c(j, ',');
	g_string_append(j, "\"channels\":{");
	gboolean first = TRUE;
	for (l = chans; l; l = l->next) {
		struct sr_channel *ch = l->data;
		struct ch_stats st;

		measure_channel(&ls, chan_index(ch), from, to, &st);
		g_string_append(j, first ? "" : ",");
		g_string_append_printf(j, "\"%s\":{", chan_name(ch));
		json_kv_u64(j, "toggles", st.toggles);
		g_string_append_c(j, ',');
		json_kv_bool(j, "active", st.active);
		g_string_append_c(j, ',');
		json_kv_bool(j, "clock_like", st.clock_like);
		if (st.freq_hz > 0) {
			g_string_append_c(j, ',');
			json_kv_f(j, "frequency_hz", st.freq_hz);
		}
		g_string_append_c(j, ',');
		json_kv_f(j, "duty_cycle", st.duty);
		g_string_append_c(j, ',');
		g_string_append(j, "\"pulse_width\":{");
		json_kv_f(j, "min_ns", st.seg_min_s * 1e9);
		g_string_append_c(j, ',');
		json_kv_f(j, "max_ns", st.seg_max_s * 1e9);
		g_string_append_c(j, ',');
		json_kv_f(j, "mean_ns", st.seg_mean_s * 1e9);
		g_string_append(j, "}");
		g_string_append_c(j, ',');
		g_string_append_printf(j, "\"first_edge\":{\"sample\":%llu,\"type\":\"%s\"}",
				(unsigned long long)st.first_edge,
				trigger_match_name(st.first_type));
		g_string_append_c(j, ',');
		g_string_append_printf(j, "\"last_edge\":{\"sample\":%llu,\"type\":\"%s\"}",
				(unsigned long long)st.last_edge,
				trigger_match_name(st.last_type));
		if (st.toggles < 4)
			g_string_append(j, ",\"note\":\"no periodic signal detected\"");
		g_string_append(j, "}");
		first = FALSE;
	}
	g_string_append(j, "}}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}

/*---- edges --------------------------------------------------------------------- */

struct edge_row {
	uint64_t sample;
	int type;
};

int cmd_edges(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t from, to;
	int max = 200;
	const char *type_name = "any";
	int want_type = -1;
	GSList *chans, *l;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--type") && i + 1 < argc) {
			type_name = argv[i + 1];
			i++;
		} else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
			max = atoi(argv[i + 1]);
			if (max <= 0 || max > 10000)
				max = 10000;
			i++;
		}
	}
	if (!strcmp(type_name, "rising"))
		want_type = SR_TRIGGER_RISING;
	else if (!strcmp(type_name, "falling"))
		want_type = SR_TRIGGER_FALLING;
	else if (strcmp(type_name, "any"))
		die_json("bad --type '%s' (any|rising|falling)", type_name);

	load_or_die(arg_file(argc, argv), &ls);
	resolve_window(&ls, argc, argv, &from, &to);
	chans = resolve_chans(&ls, argc, argv);

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_window(j, &ls, from, to);
	g_string_append_c(j, ',');
	json_kv_str(j, "type", type_name);
	g_string_append_c(j, ',');
	g_string_append(j, "\"channels\":{");
	gboolean first = TRUE;
	for (l = chans; l; l = l->next) {
		struct sr_channel *ch = l->data;
		struct edge_row *rows = g_new0(struct edge_row, (size_t)max + 1);
		int n = 0, seen = 0, total = 0;
		int prev_level = -1;
		uint64_t prev_sample = 0;

		for (uint64_t v = from; v < to; v++) {
			const uint8_t *s = sample_ptr(&ls, v);
			if (!s)
				break;
			int cur = sample_bit(s, ls.unitsize, chan_index(ch));
			if (prev_level != -1 && cur != prev_level) {
				int type = cur ? SR_TRIGGER_RISING : SR_TRIGGER_FALLING;
				total++;
				if (want_type == -1 || want_type == type) {
					seen++;
					if (n < max) {
						rows[n].sample = v;
						rows[n].type = type;
						n++;
					}
				}
				prev_sample = v;
			}
			if (prev_level == -1 || cur != prev_level)
				prev_level = cur;
			(void)prev_sample;
		}

		g_string_append(j, first ? "" : ",");
		g_string_append_printf(j, "\"%s\":{", chan_name(ch));
		json_kv_u64(j, "total", (uint64_t)total);
		g_string_append_c(j, ',');
		json_kv_u64(j, "matched", (uint64_t)seen);
		g_string_append_c(j, ',');
		json_kv_bool(j, "truncated", seen > max);
		g_string_append_c(j, ',');
		g_string_append(j, "\"edges\":[");
		double srate = (double)ls.samplerate;
		gboolean efirst = TRUE;
		for (int i = 0; i < n && i <= max; i++) {
			double t = srate ? (double)rows[i].sample / srate : 0;
			double delta = 0;
			if (i > 0 && srate)
				delta = (double)(rows[i].sample - rows[i - 1].sample) / srate * 1e9;
			g_string_append_printf(j, "%s{\"sample\":%llu,\"type\":\"%s\","
					"\"t_s\":%.10g,\"delta_ns\":%.10g}",
					efirst ? "" : ",",
					(unsigned long long)rows[i].sample,
					trigger_match_name(rows[i].type), t, delta);
			efirst = FALSE;
		}
		g_string_append(j, "]}");
		g_free(rows);
		first = FALSE;
	}
	g_string_append(j, "}}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}

/*---- sample ---------------------------------------------------------------------- */

int cmd_sample(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t at = 0;
	gboolean have_at = FALSE;
	GSList *chans, *l;
	const uint8_t *s;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--at") && i + 1 < argc) {
			have_at = TRUE;
			i++;
		}
	}
	if (!have_at)
		die_json("--at is required, e.g. --at 8352 or --at 1.2ms");

	load_or_die(arg_file(argc, argv), &ls);
	/* The --at literal sits before the file in argv; re-scan for it. */
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--at") && i + 1 < argc) {
			if (parse_window(argv[i + 1], ls.samplerate, &at) != 0)
				die_json("bad --at '%s'", argv[i + 1]);
			break;
		}
	}
	if (at >= ls.total_samples)
		die_json("--at %llu out of range (total %llu)",
				(unsigned long long)at, (unsigned long long)ls.total_samples);

	chans = resolve_chans(&ls, 0, NULL);
	s = sample_ptr(&ls, at);
	if (!s)
		die_json("sample %llu not in data", (unsigned long long)at);

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "file", arg_file_current);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samplerate", ls.samplerate);
	g_string_append_c(j, ',');
	json_kv_u64(j, "sample", at);
	g_string_append_c(j, ',');
	json_kv_f(j, "t_s", ls.samplerate ? (double)at / ls.samplerate : 0);
	g_string_append_c(j, ',');
	g_string_append(j, "\"values\":{");
	gboolean first = TRUE;
	for (l = chans; l; l = l->next) {
		g_string_append(j, first ? "" : ",");
		g_string_append_printf(j, "\"%s\":%d",
				chan_name(l->data),
				sample_bit(s, ls.unitsize, chan_index(l->data)));
		first = FALSE;
	}
	g_string_append(j, "}}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}

/*---- slice ------------------------------------------------------------------------ */

#define SLICE_MAX_SAMPLES 65536

int cmd_slice(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t from, to, span;
	const char *fmt = "hex";
	GSList *chans, *l;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--format") && i + 1 < argc) {
			fmt = argv[i + 1];
			i++;
		}
	}
	if (strcmp(fmt, "hex") && strcmp(fmt, "bits") && strcmp(fmt, "base64"))
		die_json("bad --format '%s' (hex|bits|base64)", fmt);

	load_or_die(arg_file(argc, argv), &ls);
	resolve_window(&ls, argc, argv, &from, &to);
	span = to - from;
	if (span > SLICE_MAX_SAMPLES)
		die_json("window of %llu samples exceeds the %d-sample slice cap; "
				"narrow --from/--to", (unsigned long long)span, SLICE_MAX_SAMPLES);
	chans = resolve_chans(&ls, argc, argv);

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_window(j, &ls, from, to);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samples", span);
	g_string_append_c(j, ',');
	json_kv_i64(j, "unitsize", ls.unitsize);
	g_string_append_c(j, ',');
	json_kv_str(j, "format", fmt);
	g_string_append_c(j, ',');

	if (!strcmp(fmt, "bits")) {
		g_string_append(j, "\"channels\":{");
		gboolean first = TRUE;
		for (l = chans; l; l = l->next) {
			char *bits = g_malloc((size_t)span + 1);
			for (uint64_t v = 0; v < span; v++) {
				const uint8_t *s = sample_ptr(&ls, from + v);
				bits[v] = (char)('0' + sample_bit(s, ls.unitsize,
						chan_index(l->data)));
			}
			bits[span] = '\0';
			g_string_append(j, first ? "" : ",");
			json_kv_str(j, chan_name(l->data), bits);
			g_free(bits);
			first = FALSE;
		}
		g_string_append(j, "}");
	} else {
		size_t nbytes = (size_t)(span * (uint64_t)ls.unitsize);
		uint8_t *raw = g_malloc(nbytes);
		for (uint64_t v = 0; v < span; v++) {
			const uint8_t *s = sample_ptr(&ls, from + v);
			memcpy(raw + v * (uint64_t)ls.unitsize, s, (size_t)ls.unitsize);
		}
		if (!strcmp(fmt, "hex")) {
			GString *hex = g_string_sized_new(nbytes * 2 + 1);
			for (size_t b = 0; b < nbytes; b++)
				g_string_append_printf(hex, "%02x", raw[b]);
			json_kv_str(j, "data", hex->str);
			g_string_free(hex, TRUE);
		} else {
			gchar *b64 = g_base64_encode(raw, nbytes);
			json_kv_str(j, "data", b64);
			g_free(b64);
		}
		g_free(raw);
	}
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}

/*---- overview ------------------------------------------------------------------------ */

int cmd_overview(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t from, to, span, bucket, nb;
	int points = 2000;
	GSList *chans, *l;
	int i, nch;
	uint32_t **toggles;
	uint8_t **mn, **mx;
	uint8_t *cur, *seen;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--points") && i + 1 < argc) {
			points = atoi(argv[i + 1]);
			if (points <= 0 || points > 100000)
				points = 2000;
			i++;
		}
	}

	load_or_die(arg_file(argc, argv), &ls);
	resolve_window(&ls, argc, argv, &from, &to);
	chans = resolve_chans(&ls, argc, argv);
	if (!chans)
		die_json("no logic channels");
	span = to - from;
	bucket = (span + (uint64_t)points - 1) / (uint64_t)points;
	if (bucket == 0)
		bucket = 1;
	nb = (span + bucket - 1) / bucket;

	nch = g_slist_length(chans);
	toggles = g_new0(uint32_t *, nch);
	mn = g_new0(uint8_t *, nch);
	mx = g_new0(uint8_t *, nch);
	cur = g_malloc0(nch);
	seen = g_malloc0(nch);
	for (i = 0; i < nch; i++) {
		toggles[i] = g_malloc0(sizeof(uint32_t) * nb);
		mn[i] = g_malloc0(nb);
		mx[i] = g_malloc0(nb);
		/* init min to 1 so constant-0 buckets show min 0 after first sample */
		memset(mn[i], 1, nb);
	}

	for (uint64_t v = from; v < to; v++) {
		const uint8_t *s = sample_ptr(&ls, v);
		uint64_t b = (v - from) / bucket;
		i = 0;
		for (l = chans; l; l = l->next, i++) {
			int bit = sample_bit(s, ls.unitsize, chan_index(l->data));
			if (seen[i] && bit != cur[i])
				toggles[i][b]++;
			if (bit < mn[i][b]) mn[i][b] = (uint8_t)bit;
			if (bit > mx[i][b]) mx[i][b] = (uint8_t)bit;
			cur[i] = (uint8_t)bit;
			seen[i] = 1;
		}
	}

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_window(j, &ls, from, to);
	g_string_append_c(j, ',');
	json_kv_u64(j, "points", points);
	g_string_append_c(j, ',');
	json_kv_u64(j, "bucket_samples", bucket);
	g_string_append_c(j, ',');
	json_kv_u64(j, "buckets", nb);
	g_string_append_c(j, ',');
	g_string_append(j, "\"channels\":{");
	gboolean first = TRUE;
	i = 0;
	for (l = chans; l; l = l->next, i++) {
		g_string_append(j, first ? "" : ",");
		json_str(j, chan_name(l->data));
		g_string_append(j, ":{\"min\":[");
		for (uint64_t b = 0; b < nb; b++)
			g_string_append_printf(j, "%s%d", b ? "," : "", mn[i][b]);
		g_string_append(j, "],\"max\":[");
		for (uint64_t b = 0; b < nb; b++)
			g_string_append_printf(j, "%s%d", b ? "," : "", mx[i][b]);
		g_string_append(j, "],\"toggles\":[");
		for (uint64_t b = 0; b < nb; b++)
			g_string_append_printf(j, "%s%u", b ? "," : "", toggles[i][b]);
		g_string_append(j, "]}");
		first = FALSE;
	}
	g_string_append(j, "}}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);

	for (i = 0; i < nch; i++) {
		g_free(toggles[i]);
		g_free(mn[i]);
		g_free(mx[i]);
	}
	g_free(toggles); g_free(mn); g_free(mx); g_free(cur); g_free(seen);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}

/*---- profile --------------------------------------------------------------------------- */

/* guess_bitrate annotation collector: "1.2 Mbaud/s" -> 1200000. */
struct bit_ctx {
	uint64_t val;
};

static void bitrate_ann_cb(struct srd_proto_data *pdata, void *cb_data)
{
	struct bit_ctx *c = cb_data;
	struct srd_proto_data_annotation *ann = pdata->data;
	double v;
	uint64_t mult = 1;
	const char *p;

	if (!ann || !ann->ann_text || !ann->ann_text[0] || c->val)
		return;
	v = g_ascii_strtod(ann->ann_text[0], NULL);
	p = ann->ann_text[0];
	while (*p && (*p == '.' || (*p >= '0' && *p <= '9') || *p == ' '))
		p++;
	if (*p == 'k' || *p == 'K')
		mult = 1000;
	else if (*p == 'M')
		mult = 1000 * 1000;
	else if (*p == 'G')
		mult = 1000 * 1000 * 1000;
	c->val = (uint64_t)(v * mult + 0.5);
}

/* Run guess_bitrate on one channel over the loaded data; 0 on failure.
 * Caller must have srd_init()'d and will srd_exit() — CPython does not
 * survive being finalized and re-initialized, so profile inits once. */
static uint64_t guess_bitrate_ch(struct loaded_session *ls, int index)
{
	struct srd_session *sess = NULL;
	struct srd_decoder_inst *di;
	GHashTable *chanmap;
	struct bit_ctx ctx = { 0 };
	uint64_t best = 0;

	if (srd_session_new(&sess) == SRD_OK) {
		di = srd_inst_new(sess, "guess_bitrate", NULL);
		if (di) {
			/* Samplerate after instance creation: metadata_set pushes
			 * only to instances that exist at that point. */
			srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE,
					g_variant_new_uint64(ls->samplerate));
			chanmap = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, (GDestroyNotify)g_variant_unref);
			g_hash_table_insert(chanmap, g_strdup("data"),
					g_variant_new_int32(index));
			srd_inst_channel_set_all(di, chanmap);
			g_hash_table_destroy(chanmap);

			/* Fill ALL options from their defaults — PDs index
			 * self.options as a dict, and an unset options set
			 * breaks them (same as the decode command does). */
			GHashTable *opts = g_hash_table_new_full(g_str_hash,
					g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
			for (GSList *ol = di->decoder->options; ol; ol = ol->next) {
				struct srd_decoder_option *opt = ol->data;
				if (opt->def)
					g_hash_table_insert(opts, g_strdup(opt->id),
							g_variant_ref(opt->def));
			}
			srd_inst_option_set(di, opts);
			g_hash_table_destroy(opts);

			srd_pd_output_callback_add(sess, SRD_OUTPUT_ANN,
					bitrate_ann_cb, &ctx);

			srd_session_start(sess);
			for (GSList *l = ls->chunks; l; l = l->next) {
				struct logic_chunk *chunk = l->data;
				uint64_t n = chunk->length / (uint64_t)ls->unitsize;
				srd_session_send(sess, chunk->start_sample,
						chunk->start_sample + n,
						chunk->data, chunk->length,
						(uint64_t)ls->unitsize, 0, 0);
			}
			srd_session_send_eof(sess);
			best = ctx.val;
		}
		srd_session_destroy(sess);
	}
	return best;
}

struct prof_ch {
	struct sr_channel *ch;
	struct ch_stats st;
	uint64_t bitrate;
};

int cmd_profile(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t from, to, span;
	GSList *chans, *l;
	struct prof_ch *pcs;
	int i, nch;
	int have_clock = 0;
	const char *ck_name = NULL;
	gboolean first;

	load_or_die(arg_file(argc, argv), &ls);
	resolve_window(&ls, argc, argv, &from, &to);
	chans = resolve_chans(&ls, argc, argv);
	if (!chans)
		die_json("no logic channels");
	span = to - from;

	nch = g_slist_length(chans);
	pcs = g_new0(struct prof_ch, nch);

	/* Per-channel measurement; busy spans come from first/last edge. */
	i = 0;
	for (l = chans; l; l = l->next, i++) {
		struct prof_ch *p = &pcs[i];
		p->ch = l->data;

		measure_channel(&ls, chan_index(p->ch), from, to, &p->st);
		if (p->st.clock_like && !have_clock) {
			have_clock = 1;
			ck_name = chan_name(p->ch);
		}
	}

	/* Busy windows: per-channel [first_edge, last_edge] spans, merged
	 * when they overlap. Channels without edges contribute nothing. */
	GArray *wins = g_array_new(FALSE, FALSE, sizeof(uint64_t) * 2);
	for (i = 0; i < nch; i++) {
		struct prof_ch *p = &pcs[i];
		if (p->st.toggles == 0)
			continue;
		uint64_t w[2] = { p->st.first_edge, p->st.last_edge };
		g_array_append_vals(wins, w, 1);
	}
	/* Sort by start (insertion sort, handful of channels). */
	int nw = (int)wins->len;
	for (int a = 1; a < nw; a++) {
		uint64_t key[2] = {
			g_array_index(wins, uint64_t, a * 2),
			g_array_index(wins, uint64_t, a * 2 + 1)
		};
		int b = a - 1;
		while (b >= 0 && g_array_index(wins, uint64_t, b * 2) > key[0]) {
			g_array_index(wins, uint64_t, b * 2 + 2) =
				g_array_index(wins, uint64_t, b * 2);
			g_array_index(wins, uint64_t, b * 2 + 3) =
				g_array_index(wins, uint64_t, b * 2 + 1);
			b--;
		}
		g_array_index(wins, uint64_t, b * 2 + 2) = key[0];
		g_array_index(wins, uint64_t, b * 2 + 3) = key[1];
	}
	/* Merge overlapping spans in place. */
	int out_n = 0;
	for (int a = 0; a < nw; a++) {
		uint64_t s = g_array_index(wins, uint64_t, a * 2);
		uint64_t e = g_array_index(wins, uint64_t, a * 2 + 1);
		if (out_n > 0 && s <= g_array_index(wins, uint64_t, (out_n - 1) * 2 + 1)) {
			uint64_t *pe = &g_array_index(wins, uint64_t, (out_n - 1) * 2 + 1);
			if (e > *pe)
				*pe = e;
		} else {
			g_array_index(wins, uint64_t, out_n * 2) = s;
			g_array_index(wins, uint64_t, out_n * 2 + 1) = e;
			out_n++;
		}
	}
	g_array_set_size(wins, (guint)(out_n * 2));

	/* guess_bitrate on active non-clock channels (cap 4 to bound cost).
	 * srd is initialized ONCE here: srd_exit() finalizes the embedded
	 * Python interpreter, which cannot be re-initialized afterwards. */
	int guesses = 0;
	if (srd_init(decoders_path()) == SRD_OK) {
		srd_decoder_load_all();
		for (i = 0; i < nch && guesses < 4; i++) {
			struct prof_ch *p = &pcs[i];
			if (p->st.active && !p->st.clock_like) {
				p->bitrate = guess_bitrate_ch(&ls, chan_index(p->ch));
				guesses++;
			}
		}
		srd_exit();
	}

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_window(j, &ls, from, to);
	g_string_append_c(j, ',');
	g_string_append(j, "\"channels\":[");
	first = TRUE;
	for (i = 0; i < nch; i++) {
		struct prof_ch *p = &pcs[i];
		const char *sugg = p->st.clock_like ? "SCL/SCK (clock-like)"
				: p->st.active ? (p->bitrate ?
					"data (bitrate guessed)" : "data")
				: "idle";
		g_string_append(j, first ? "" : ",");
		g_string_append_printf(j, "{\"ch\":");
		json_str(j, chan_name(p->ch));
		g_string_append(j, ",\"active\":");
		g_string_append_printf(j, p->st.active ? "true" : "false");
		if (p->st.freq_hz > 0) {
			g_string_append(j, ",\"freq_hz\":");
			g_string_append_printf(j, "%.10g", p->st.freq_hz);
		}
		g_string_append(j, ",\"clock_like\":");
		g_string_append_printf(j, p->st.clock_like ? "true" : "false");
		if (p->bitrate) {
			g_string_append(j, ",\"bitrate\":");
			g_string_append_printf(j, "%llu", (unsigned long long)p->bitrate);
		}
		g_string_append(j, ",\"suggestion\":");
		json_str(j, sugg);
		g_string_append(j, "}");
		first = FALSE;
	}
	g_string_append(j, "],\"candidates\":[");

	GSList *cands = NULL;
	struct prof_ch *ck = NULL;
	for (i = 0; i < nch; i++)
		if (pcs[i].st.clock_like) { ck = &pcs[i]; break; }

	if (ck) {
		for (i = 0; i < nch; i++) {
			struct prof_ch *p = &pcs[i];
			if (!p->st.active || p->st.clock_like)
				continue;
			gchar *c1 = g_strdup_printf("i2c (scl=%s, sda=%s)",
					ck_name, chan_name(p->ch));
			gchar *c2 = g_strdup_printf("spi (sck=%s, mosi=%s, cs/miso unconfirmed)",
					ck_name, chan_name(p->ch));
			cands = g_slist_append(cands, c1);
			cands = g_slist_append(cands, c2);
			break; /* first data channel is enough */
		}
	} else {
		for (i = 0; i < nch && g_slist_length(cands) < 2; i++) {
			struct prof_ch *p = &pcs[i];
			if (!p->st.active || p->st.clock_like)
				continue;
			gchar *c = p->bitrate
				? g_strdup_printf("uart (tx=%s, baud~%llu)",
						chan_name(p->ch),
						(unsigned long long)p->bitrate)
				: g_strdup_printf("uart (tx=%s, baud unknown)",
						chan_name(p->ch));
			cands = g_slist_append(cands, c);
		}
	}
	if (!cands)
		cands = g_slist_append(cands, g_strdup("unknown"));

	first = TRUE;
	for (GSList *c = cands; c; c = c->next) {
		g_string_append(j, first ? "" : ",");
		json_str(j, c->data);
		first = FALSE;
	}
	g_string_append(j, "],\"busy_windows\":[");
	first = TRUE;
	for (i = 0; i * 2 < (int)wins->len; i++) {
		uint64_t *w = &g_array_index(wins, uint64_t, i * 2);
		g_string_append_printf(j, "%s{\"from\":%llu,\"to\":%llu}",
				first ? "" : ",",
				(unsigned long long)w[0], (unsigned long long)w[1]);
		first = FALSE;
	}
	g_string_append(j, "]}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);

	for (GSList *c = cands; c; c = c->next)
		g_free(c->data);
	g_slist_free(cands);
	g_array_free(wins, TRUE);
	g_free(pcs);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}
