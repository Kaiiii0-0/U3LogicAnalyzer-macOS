/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * Capture: scan → open → config → trigger → datafeed → srzip output.
 *
 * Flow mirrors PulseView's pv/session/session.cpp::sample_thread_proc, but
 * runs headless: a GLib timeout stops the session (the libsigrok session
 * runs its main loop inside sr_session_run in the current thread).
 */

#include "u3ctl.h"

struct capture_ctx {
	struct sr_session *session;
	struct sr_dev_inst *sdi;
	const struct sr_output *output;
	uint64_t limit_samples;     /* 0 = unlimited */
	uint64_t samples_received;
	uint64_t trigger_pos;
	gboolean trigger_seen;
	gboolean stopped;
	gboolean hit_limit;
	gboolean hit_timeout;
};

/*---- options ------------------------------------------------------------ */

struct cap_opts {
	const char *driver_name;
	const char *conn;
	uint64_t samplerate;
	uint64_t limit_samples;
	uint64_t limit_time_samples;
	char *time_literal;          /* pending "--time 50ms" spec */
	uint32_t num_channels;       /* 0 = leave driver default */
	uint32_t pre_ratio;          /* capture ratio percent, 0-100 */
	double threshold;            /* volts, <0 = unset */
	const char *mode;            /* "logic"|"adc" or NULL */
	const char *demo_pattern;
	GSList *trigger_stages;      /* GSList of GSList of struct trig_spec */
	int timeout_s;
	char *out_path;
	const char *fmt;
	gboolean overwrite;
};

struct trig_spec {
	const char *ch_name;
	int match;
};

/* "D0:rising+D1:high" -> list of trig_spec */
static GSList *parse_trigger_stage(const char *spec)
{
	gchar **toks;
	GSList *specs = NULL;

	toks = g_strsplit(spec, "+", 0);
	for (gchar **t = toks; *t; t++) {
		gchar **kv = g_strsplit(*t, ":", 2);
		struct trig_spec *ts;
		int match;

		if (g_strv_length(kv) != 2 || trigger_match_from_name(kv[1], &match) != 0) {
			g_strfreev(kv);
			g_slist_free_full(specs, (GDestroyNotify)g_free);
			g_strfreev(toks);
			die_json("bad trigger spec '%s', want CH:high|low|rising|falling|edge", spec);
		}
		ts = g_new0(struct trig_spec, 1);
		ts->ch_name = g_strdup(kv[0]);
		ts->match = match;
		specs = g_slist_append(specs, ts);
		g_strfreev(kv);
	}
	g_strfreev(toks);
	return specs;
}

static void parse_capture_args(int argc, char **argv, struct cap_opts *o)
{
	int i;

	memset(o, 0, sizeof(*o));
	o->threshold = -1.0;
	o->timeout_s = 30;
	o->fmt = "srzip";

	for (i = 0; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--driver") && i + 1 < argc) {
			o->driver_name = argv[++i];
		} else if (!strcmp(a, "--conn") && i + 1 < argc) {
			o->conn = argv[++i];
		} else if (!strcmp(a, "--rate") && i + 1 < argc) {
			if (parse_si_u64(argv[++i], &o->samplerate) != 0 || !o->samplerate)
				die_json("bad --rate value");
		} else if (!strcmp(a, "--samples") && i + 1 < argc) {
			if (parse_si_u64(argv[++i], &o->limit_samples) != 0 || !o->limit_samples)
				die_json("bad --samples value");
		} else if (!strcmp(a, "--time") && i + 1 < argc) {
			g_free(o->time_literal);
			o->time_literal = g_strdup(argv[++i]);
		} else if (!strcmp(a, "--channels") && i + 1 < argc) {
			o->num_channels = (uint32_t)g_ascii_strtoull(argv[++i], NULL, 10);
		} else if (!strcmp(a, "--pre") && i + 1 < argc) {
			o->pre_ratio = (uint32_t)g_ascii_strtoull(argv[++i], NULL, 10);
			if (o->pre_ratio > 100)
				die_json("--pre must be 0-100");
		} else if (!strcmp(a, "--threshold") && i + 1 < argc) {
			o->threshold = g_ascii_strtod(argv[++i], NULL);
		} else if (!strcmp(a, "--mode") && i + 1 < argc) {
			o->mode = argv[++i];
		} else if (!strcmp(a, "--demo-pattern") && i + 1 < argc) {
			o->demo_pattern = argv[++i];
		} else if (!strcmp(a, "--trigger") && i + 1 < argc) {
			o->trigger_stages = g_slist_append(o->trigger_stages,
					parse_trigger_stage(argv[++i]));
		} else if (!strcmp(a, "--timeout") && i + 1 < argc) {
			o->timeout_s = (int)g_ascii_strtoull(argv[++i], NULL, 10);
			if (o->timeout_s <= 0)
				die_json("--timeout must be > 0");
		} else if (!strcmp(a, "--out") && i + 1 < argc) {
			o->out_path = g_strdup(argv[++i]);
		} else if (!strcmp(a, "--overwrite")) {
			o->overwrite = TRUE;
		} else if (!strcmp(a, "--format") && i + 1 < argc) {
			o->fmt = argv[++i];
		} else {
			die_json("unknown capture option '%s'", a);
		}
	}
}

/*---- datafeed ------------------------------------------------------------ */

static void datafeed_in(const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet, void *cb_data)
{
	struct capture_ctx *c = cb_data;
	GString *junk = NULL;

	(void)sdi;
	switch (packet->type) {
	case SR_DF_HEADER:
		break;
	case SR_DF_META:
		if (c->output)
			sr_output_send(c->output, packet, &junk);
		break;
	case SR_DF_TRIGGER:
		/* This fork's DF_TRIGGER has no payload (session.c copies none);
		 * the trigger position is the number of pre-trigger samples
		 * delivered before this packet. */
		c->trigger_pos = c->samples_received;
		c->trigger_seen = TRUE;
		break;
	case SR_DF_LOGIC: {
		const struct sr_datafeed_logic *logic = packet->payload;
		if (c->output)
			sr_output_send(c->output, packet, &junk);
		c->samples_received += logic->length / logic->unitsize;
		if (c->limit_samples && c->samples_received >= c->limit_samples
				&& !c->hit_limit) {
			c->hit_limit = TRUE;
			c->stopped = TRUE;
			sr_session_stop(c->session);
		}
		break;
	}
	case SR_DF_ANALOG:
		if (c->output)
			sr_output_send(c->output, packet, &junk);
		break;
	case SR_DF_END:
		/* Do NOT call sr_session_stop here: dev_acquisition_stop (which
		 * this packet came from) sends DF_END again — sr_session_stop
		 * would recurse. The session's stop check quits the loop. */
		c->stopped = TRUE;
		if (c->output)
			sr_output_send(c->output, packet, &junk);
		break;
	}
	if (junk && junk->len)
		g_string_free(junk, TRUE);
}

static gboolean timeout_cb(gpointer data)
{
	struct capture_ctx *c = data;

	if (!c->stopped) {
		c->hit_timeout = TRUE;
		c->stopped = TRUE;
		sr_session_stop(c->session);
	}
	return G_SOURCE_REMOVE;
}

/*---- trigger building ------------------------------------------------------ */

static struct sr_trigger *build_trigger(struct cap_opts *o,
		struct sr_dev_inst *sdi)
{
	struct sr_trigger *trig;
	GSList *sl, *tl;
	int stage_idx = 0;

	if (!o->trigger_stages)
		return NULL;

	trig = sr_trigger_new("u3ctl");
	for (sl = o->trigger_stages; sl; sl = sl->next, stage_idx++) {
		struct sr_trigger_stage *stage = sr_trigger_stage_add(trig);

		for (tl = sl->data; tl; tl = tl->next) {
			struct trig_spec *ts = tl->data;
			GSList *cl;
			struct sr_channel *found = NULL;

			for (cl = sr_dev_inst_channels_get(sdi); cl; cl = cl->next) {
				struct sr_channel *ch = cl->data;
				if (ch->type == SR_CHANNEL_LOGIC && !strcmp(ch->name, ts->ch_name)) {
					found = ch;
					break;
				}
			}
			if (!found)
				die_json("trigger channel '%s' not found on device", ts->ch_name);
			sr_trigger_match_add(stage, found, ts->match, 0.0f);
		}
	}
	return trig;
}

/*---- capture command ------------------------------------------------------- */

static int set_config_u64(struct sr_dev_driver *d, struct sr_dev_inst *sdi,
		uint32_t key, uint64_t val, const char *what)
{
	GVariant *gv = g_variant_new_uint64(val);
	int ret = sr_config_set(sdi, NULL, key, gv);
	if (ret != SR_OK)
		die_json("failed to set %s: %s", what, sr_strerror(ret));
	(void)d;
	return ret;
}

static uint64_t get_samplerate(struct sr_dev_driver *d, struct sr_dev_inst *sdi)
{
	GVariant *gv = NULL;
	uint64_t rate = 0;

	if (sr_config_get(d, sdi, NULL, SR_CONF_SAMPLERATE, &gv) == SR_OK && gv) {
		rate = g_variant_get_uint64(gv);
		g_variant_unref(gv);
	}
	return rate;
}

static void resolve_channels(struct cap_opts *o, struct sr_dev_inst *sdi)
{
	GSList *l;
	int n_logic = 0;

	for (l = sr_dev_inst_channels_get(sdi); l; l = l->next) {
		struct sr_channel *ch = l->data;
		if (ch->type != SR_CHANNEL_LOGIC)
			continue;
		n_logic++;
		if (o->num_channels > 0)
			sr_dev_channel_enable(ch, ch->index < (int)o->num_channels);
	}
	if (o->num_channels == 0)
		o->num_channels = n_logic;
}

/* Set a config key on the device, falling back to channel groups for
 * drivers that scope it per-group (e.g. demo's pattern mode). */
static int config_set_anywhere(struct sr_dev_inst *sdi, uint32_t key,
		GVariant *gv)
{
	int ret = sr_config_set(sdi, NULL, key, gv);
	if (ret != SR_OK) {
		for (GSList *l = sr_dev_inst_channel_groups_get(sdi); l; l = l->next) {
			struct sr_channel_group *cg = l->data;
			GVariant *gv2 = g_variant_ref(gv);
			ret = sr_config_set(sdi, cg, key, gv2);
			if (ret == SR_OK)
				break;
		}
	}
	g_variant_unref(gv);
	return ret;
}

static gboolean config_can_set(struct sr_dev_inst *sdi, uint32_t key)
{
	return sr_dev_config_capabilities_list(sdi, NULL, key) & SR_CONF_SET;
}

static void set_device_config(struct cap_opts *o, struct sr_dev_driver *d,
		struct sr_dev_inst *sdi)
{
	int ret;

	/* Channel count first: it re-picks rate tables / unitsize on the U3. */
	if (o->num_channels > 0 && config_can_set(sdi, SR_CONF_NUM_LOGIC_CHANNELS))
		set_config_u64(d, sdi, SR_CONF_NUM_LOGIC_CHANNELS, o->num_channels,
				"num_logic_channels");

	if (o->samplerate)
		set_config_u64(d, sdi, SR_CONF_SAMPLERATE, o->samplerate, "samplerate");

	if (o->limit_samples)
		set_config_u64(d, sdi, SR_CONF_LIMIT_SAMPLES, o->limit_samples,
				"limit_samples");

	if (o->limit_time_samples)
		set_config_u64(d, sdi, SR_CONF_LIMIT_SAMPLES, o->limit_time_samples,
				"limit_samples(time)");

	if (o->pre_ratio)
		set_config_u64(d, sdi, SR_CONF_CAPTURE_RATIO, o->pre_ratio, "capture_ratio");

	if (o->threshold >= 0.0) {
		ret = config_set_anywhere(sdi, SR_CONF_VOLTAGE_THRESHOLD,
				g_variant_new_double(o->threshold));
		if (ret != SR_OK)
			die_json("failed to set voltage threshold: %s", sr_strerror(ret));
	}

	const char *pattern = o->mode ? o->mode : o->demo_pattern;
	if (pattern) {
		ret = config_set_anywhere(sdi, SR_CONF_PATTERN_MODE,
				g_variant_new_string(pattern));
		if (ret != SR_OK)
			die_json("failed to set pattern/mode '%s': %s", pattern,
					sr_strerror(ret));
	}
}

static void resolve_time_limit(struct cap_opts *o, struct sr_dev_driver *d,
		struct sr_dev_inst *sdi)
{
	uint64_t rate;

	if (!o->time_literal)
		return;
	rate = get_samplerate(d, sdi);
	if (!rate)
		die_json("cannot resolve --time without a samplerate");
	if (parse_time_samples(o->time_literal, rate, &o->limit_time_samples) != 0)
		die_json("bad --time literal '%s'", o->time_literal);
}

int cmd_capture(int argc, char **argv)
{
	struct cap_opts o;
	struct capture_ctx c;
	struct sr_dev_driver *d;
	struct sr_dev_inst *sdi;
	struct sr_trigger *trig;
	struct sr_session *session = NULL;
	const struct sr_output_module *omod;
	GString *j;
	time_t now;
	char auto_name[128];
	int ret;

	parse_capture_args(argc, argv, &o);

	d = o.driver_name ? find_driver(o.driver_name) : find_default_capture_driver();
	if (!d)
		die_json("driver '%s' not found", o.driver_name ? o.driver_name : U3CTL_DEFAULT_DRIVERS);

	/* Channel-count request: pass as scan option too (demo picks its
	 * channel count at scan time; the wch driver ignores unknown scan
	 * options and gets it via config instead). */
	GSList *scan_opts = NULL;
	if (o.num_channels > 0) {
		struct sr_config *sopt = g_new0(struct sr_config, 1);
		sopt->key = SR_CONF_NUM_LOGIC_CHANNELS;
		sopt->data = g_variant_new_int32((int32_t)o.num_channels);
		scan_opts = g_slist_append(scan_opts, sopt);
	}

	sdi = pick_device(d, o.conn, scan_opts);
	if (!sdi)
		die_json("no device found for driver '%s'%s%s", d->name,
				o.conn ? " matching conn " : "", o.conn ? o.conn : "");

	/* Open, then configure. */
	ret = sr_dev_open(sdi);
	if (ret != SR_OK)
		die_json("failed to open device (GUI running? device busy?): %s",
				sr_strerror(ret));

	resolve_channels(&o, sdi);
	set_device_config(&o, d, sdi);
	resolve_time_limit(&o, d, sdi);

	/* Output path. */
	if (!o.out_path) {
		now = time(NULL);
		strftime(auto_name, sizeof(auto_name), "captures/cap-%Y%m%d-%H%M%S.sr",
				localtime(&now));
		o.out_path = g_strdup(auto_name);
	}
	if (!o.overwrite && g_file_test(o.out_path, G_FILE_TEST_EXISTS))
		die_json("output file '%s' exists, use --overwrite", o.out_path);
	gchar *dir = g_path_get_dirname(o.out_path);
	g_mkdir_with_parents(dir, 0755);
	g_free(dir);

	omod = sr_output_find("srzip");
	if (!omod)
		die_json("srzip output module missing");

	memset(&c, 0, sizeof(c));
	c.output = sr_output_new(omod, NULL, sdi, o.out_path);
	if (!c.output)
		die_json("failed to create srzip output");

	trig = build_trigger(&o, sdi);

	ret = sr_session_new(G.sr_ctx, &session);
	if (ret != SR_OK)
		die_json("session create failed: %s", sr_strerror(ret));
	c.session = session;
	sr_session_dev_add(session, sdi);
	sr_session_datafeed_callback_add(session, datafeed_in, &c);
	if (trig)
		sr_session_trigger_set(session, trig);

	g_timeout_add_seconds(o.timeout_s, timeout_cb, &c);

	ret = sr_session_start(session);
	if (ret != SR_OK)
		die_json("capture start failed: %s (device busy or misconfigured)",
				sr_strerror(ret));
	sr_session_run(session);

	/* Finish output file (srzip flushes on DF_END which we forwarded). */
	sr_output_free(c.output);

	uint64_t rate = get_samplerate(d, sdi);
	double duration = rate ? (double)c.samples_received / rate : 0.0;

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "file", o.out_path);
	g_string_append_c(j, ',');
	json_kv_str(j, "driver", d->name);
	g_string_append_c(j, ',');
	json_kv_str(j, "conn", sr_dev_inst_connid_get(sdi));
	g_string_append_c(j, ',');
	json_kv_u64(j, "samplerate", rate);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samples", c.samples_received);
	g_string_append_c(j, ',');
	json_kv_f(j, "duration_s", duration);
	g_string_append_c(j, ',');
	json_kv_u64(j, "channels", o.num_channels);
	g_string_append_c(j, ',');
	json_kv_bool(j, "trigger", c.trigger_seen);
	if (c.trigger_seen) {
		g_string_append_c(j, ',');
		json_kv_u64(j, "trigger_position_sample", c.trigger_pos);
	}
	g_string_append_c(j, ',');
	json_kv_bool(j, "stopped_by_limit", c.hit_limit);
	g_string_append_c(j, ',');
	json_kv_bool(j, "stopped_by_timeout", c.hit_timeout);
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	g_free(o.out_path);
	return 0;
}
