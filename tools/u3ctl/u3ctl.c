/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * Main entry, JSON output helpers, SI parsing, devices/capabilities.
 *
 * All subcommands print a single JSON document to stdout. Errors are
 * printed as {"error": "..."} with exit code 2, so agents can parse both.
 */

#include "u3ctl.h"

struct u3ctl_globals G;

/*---- init / shutdown ----------------------------------------------------- */

static int init_driver(struct sr_dev_driver *d)
{
	int ret = sr_driver_init(G.sr_ctx, d);
	if (ret != SR_OK) {
		g_critical("Failed to init driver '%s'.", d->name);
		return ret;
	}
	return SR_OK;
}

int u3ctl_init(int loglevel)
{
	sr_log_loglevel_set(loglevel);
	if (loglevel >= SR_LOG_DBG)
		srd_log_loglevel_set(SRD_LOG_DBG);
	else if (loglevel >= SR_LOG_INFO)
		srd_log_loglevel_set(SRD_LOG_WARN);
	else
		srd_log_loglevel_set(SRD_LOG_ERR);

	if ((sr_init(&G.sr_ctx) != SR_OK)) {
		g_critical("libsigrok init failed.");
		return SR_ERR;
	}

	G.drivers = sr_driver_list(G.sr_ctx);

	/* Init every driver once; scan/open happen per command. */
	for (struct sr_dev_driver **d = G.drivers; d && *d; d++) {
		if (init_driver(*d) != SR_OK)
			return SR_ERR;
	}

	return SR_OK;
}

void u3ctl_shutdown(void)
{
	if (G.sr_ctx)
		sr_exit(G.sr_ctx);
	G.sr_ctx = NULL;
}

/*---- driver / device lookup ---------------------------------------------- */

struct sr_dev_driver *find_driver(const char *name)
{
	for (struct sr_dev_driver **d = G.drivers; d && *d; d++) {
		if (!strcmp((*d)->name, name))
			return *d;
	}
	return NULL;
}

struct sr_dev_driver *find_default_capture_driver(void)
{
	gchar **names, **n;
	struct sr_dev_driver *d = NULL;

	names = g_strsplit(U3CTL_DEFAULT_DRIVERS, ",", 0);
	for (n = names; *n && !d; n++)
		d = find_driver(*n);
	g_strfreev(names);
	return d;
}

static gboolean device_matches_conn(struct sr_dev_inst *sdi, const char *conn)
{
	const char *cid = sr_dev_inst_connid_get(sdi);
	if (!cid)
		return conn == NULL;
	return conn && strstr(cid, conn) != NULL;
}

GSList *scan_driver_devices(struct sr_dev_driver *driver, const char *conn,
		GSList *scan_opts)
{
	GSList *devs, *l, *matched = NULL;

	devs = sr_driver_scan(driver, scan_opts);
	for (l = devs; l; l = l->next) {
		if (!conn || device_matches_conn(l->data, conn))
			matched = g_slist_prepend(matched, l->data);
	}
	return g_slist_reverse(matched);
}

struct sr_dev_inst *pick_device(struct sr_dev_driver *driver, const char *conn,
		GSList *scan_opts)
{
	GSList *devs;
	struct sr_dev_inst *sdi = NULL;

	devs = scan_driver_devices(driver, conn, scan_opts);
	if (devs)
		sdi = devs->data;
	g_slist_free(devs);
	return sdi;
}

/*---- JSON helpers --------------------------------------------------------- */

void json_str(GString *j, const char *s)
{
	if (!s) {
		g_string_append(j, "null");
		return;
	}
	g_string_append_c(j, '"');
	for (; *s; s++) {
		switch (*s) {
		case '"':  g_string_append(j, "\\\""); break;
		case '\\': g_string_append(j, "\\\\"); break;
		case '\n': g_string_append(j, "\\n");  break;
		case '\r': g_string_append(j, "\\r");  break;
		case '\t': g_string_append(j, "\\t");  break;
		default:
			if ((guchar)*s < 0x20)
				g_string_append_printf(j, "\\u%04x", *s);
			else
				g_string_append_c(j, *s);
		}
	}
	g_string_append_c(j, '"');
}

void json_kv_str(GString *j, const char *key, const char *val)
{
	g_string_append_printf(j, "\"%s\":", key);
	json_str(j, val);
}

void json_kv_u64(GString *j, const char *key, uint64_t val)
{
	g_string_append_printf(j, "\"%s\":%llu", key, (unsigned long long)val);
}

void json_kv_i64(GString *j, const char *key, int64_t val)
{
	g_string_append_printf(j, "\"%s\":%lld", key, (long long)val);
}

void json_kv_f(GString *j, const char *key, double val)
{
	g_string_append_printf(j, "\"%s\":%.10g", key, val);
}

void json_kv_bool(GString *j, const char *key, gboolean val)
{
	g_string_append_printf(j, "\"%s\":%s", key, val ? "true" : "false");
}

void die_json(const char *fmt, ...)
{
	va_list args;
	char msg[1024];
	GString *j = g_string_new(NULL);

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	g_string_append_printf(j, "{\"error\":");
	json_str(j, msg);
	g_string_append(j, ",\"command\":");
	GString *cmd = g_string_new(NULL);
	for (char **a = G.argv + 1; a && *a; a++) {
		g_string_append(cmd, *a);
		if (*(a + 1))
			g_string_append_c(cmd, ' ');
	}
	json_str(j, cmd->str);
	g_string_free(cmd, TRUE);
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	exit(2);
}

void die(const char *fmt, ...)
{
	va_list args;
	char msg[1024];

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	fprintf(stderr, "u3ctl: %s\n", msg);
	exit(2);
}

/*---- SI parsing ----------------------------------------------------------- */

static int si_mult(char c, uint64_t *mult)
{
	/* Decimal SI kilo/mega/giga; "i" suffix picks 1024-based. */
	switch (g_ascii_tolower(c)) {
	case 'k': *mult = 1000; break;
	case 'm': *mult = 1000 * 1000; break;
	case 'g': *mult = 1000 * 1000 * 1000; break;
	case 't': *mult = 1000ULL * 1000 * 1000 * 1000; break;
	default:  return -1;
	}
	return 0;
}

int parse_si_u64(const char *s, uint64_t *out)
{
	double val;
	char *end;
	uint64_t mult = 1;
	size_t len;

	if (!s || !*s)
		return -1;
	len = strlen(s);

	/* Binary suffix: Ki, Mi, Gi, Ti — 1024-based. */
	if (len >= 2 && g_ascii_tolower(s[len - 1]) == 'i') {
		uint64_t bin_mult;
		if (si_mult(s[len - 2], &bin_mult) == 0) {
			mult = bin_mult * 1024;
		} else {
			return -1; /* stray trailing 'i' */
		}
	} else if (si_mult(s[len - 1], &mult) != 0) {
		mult = 1; /* no recognized suffix */
	}

	val = g_ascii_strtod(s, &end);
	if (end == s)
		return -1;
	*out = (uint64_t)(val * mult + 0.5);
	return 0;
}

/* Time spec -> samples. Accepts "50ms", "1.2us", "2ns", "0.5s", "3m". */
int parse_time_samples(const char *s, uint64_t samplerate, uint64_t *out)
{
	double val;
	char *end;
	const char *unit;
	double factor = 1.0; /* seconds */

	val = g_ascii_strtod(s, &end);
	if (end == s)
		return -1;
	unit = end;
	if (!strcmp(unit, "s"))
		factor = 1.0;
	else if (!strcmp(unit, "ms"))
		factor = 1e-3;
	else if (!strcmp(unit, "us") || !strcmp(unit, "µs"))
		factor = 1e-6;
	else if (!strcmp(unit, "ns"))
		factor = 1e-9;
	else if (!strcmp(unit, "m"))
		factor = 60.0;
	else if (!strcmp(unit, "h"))
		factor = 3600.0;
	else if (!strcmp(unit, ""))
		return -1; /* bare number: caller treats as samples instead */
	else
		return -1;

	if (samplerate == 0)
		return -1;
	*out = (uint64_t)(val * factor * samplerate + 0.5);
	return 0;
}

int parse_window(const char *s, uint64_t samplerate, uint64_t *out)
{
	if (!s || !*s)
		return -1;
	if (g_ascii_isdigit(s[strlen(s) - 1])) {
		/* Either a plain sample number, or a time with no unit —
		 * treat digits-only as sample number. */
		if (strchr(s, '.') == NULL)
			return parse_si_u64(s, out);
	}
	if (parse_time_samples(s, samplerate, out) == 0)
		return 0;
	return parse_si_u64(s, out);
}

/*---- trigger names --------------------------------------------------------- */

const char *trigger_match_name(int match)
{
	switch (match) {
	case SR_TRIGGER_ZERO:    return "low";
	case SR_TRIGGER_ONE:     return "high";
	case SR_TRIGGER_RISING:  return "rising";
	case SR_TRIGGER_FALLING: return "falling";
	case SR_TRIGGER_EDGE:    return "edge";
	case SR_TRIGGER_OVER:    return "over";
	case SR_TRIGGER_UNDER:   return "under";
	default:                 return "?";
	}
}

int trigger_match_from_name(const char *name, int *match)
{
	if (!strcmp(name, "low"))      *match = SR_TRIGGER_ZERO;
	else if (!strcmp(name, "high"))     *match = SR_TRIGGER_ONE;
	else if (!strcmp(name, "rising"))   *match = SR_TRIGGER_RISING;
	else if (!strcmp(name, "fall"))     *match = SR_TRIGGER_FALLING;
	else if (!strcmp(name, "falling"))  *match = SR_TRIGGER_FALLING;
	else if (!strcmp(name, "edge"))     *match = SR_TRIGGER_EDGE;
	else return -1;
	return 0;
}

/*---- devices list ----------------------------------------------------------- */

static void print_channel_json(GString *j, struct sr_dev_inst *sdi)
{
	GSList *l;
	gboolean first = TRUE;

	g_string_append(j, "\"channels\":[");
	for (l = sr_dev_inst_channels_get(sdi); l; l = l->next) {
		struct sr_channel *ch = l->data;
		g_string_append_printf(j, "%s{\"name\":", first ? "" : ",");
		json_str(j, ch->name);
		g_string_append_printf(j, ",\"type\":\"%s\",\"index\":%d,"
			"\"enabled\":%s}",
			ch->type == SR_CHANNEL_LOGIC ? "logic" : "analog",
			ch->index, ch->enabled ? "true" : "false");
		first = FALSE;
	}
	g_string_append(j, "]");
}

int cmd_devices_list(int argc, char **argv)
{
	GString *j;
	const char *conn = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--conn") && i + 1 < argc)
			conn = argv[++i];
	}

	j = g_string_new(NULL);
	g_string_append(j, "{\"devices\":[");
	gboolean first_dev = TRUE;

	for (struct sr_dev_driver **dl = G.drivers; dl && *dl; dl++) {
		struct sr_dev_driver *d = *dl;
		GSList *devs, *devl;

		devs = scan_driver_devices(d, conn, NULL);
		for (devl = devs; devl; devl = devl->next) {
			struct sr_dev_inst *sdi = devl->data;
			g_string_append(j, first_dev ? "" : ",");
			g_string_append(j, "{");
			json_kv_str(j, "driver", d->name);
			g_string_append_c(j, ',');
			json_kv_str(j, "vendor", sr_dev_inst_vendor_get(sdi));
			g_string_append_c(j, ',');
			json_kv_str(j, "model", sr_dev_inst_model_get(sdi));
			g_string_append_c(j, ',');
			json_kv_str(j, "serial", sr_dev_inst_sernum_get(sdi));
			g_string_append_c(j, ',');
			json_kv_str(j, "version", sr_dev_inst_version_get(sdi));
			g_string_append_c(j, ',');
			json_kv_str(j, "conn", sr_dev_inst_connid_get(sdi));
			g_string_append_c(j, ',');
			print_channel_json(j, sdi);
			g_string_append(j, "}");
			first_dev = FALSE;
		}
		g_slist_free(devs);
	}


	g_string_append(j, "]}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	return 0;
}

/*---- capabilities ---------------------------------------------------------- */

/* Extract a uint64 from a GVariant holding either uint64 or int32/int64. */
static uint64_t variant_to_u64(GVariant *v)
{
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))
		return g_variant_get_uint64(v);
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
		return (uint64_t)g_variant_get_int64(v);
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
		return (uint64_t)g_variant_get_int32(v);
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
		return g_variant_get_uint32(v);
	return 0;
}

static void append_samplerate_list(GString *j, struct sr_dev_driver *d,
		struct sr_dev_inst *sdi)
{
	GVariant *data;
	gboolean first = TRUE;

	if (sr_config_list(d, sdi, NULL, SR_CONF_SAMPLERATE, &data) != SR_OK) {
		g_string_append(j, "\"samplerates\":[]");
		return;
	}
	g_string_append(j, "\"samplerates\":[");

	/* std_gvar_samplerates() wraps the rate array in a dict:
	 * a{sv} with key "samplerates" -> array of uint64. */
	if (g_variant_is_of_type(data, G_VARIANT_TYPE_DICTIONARY)) {
		GVariant *rates = g_variant_lookup_value(data, "samplerates",
				G_VARIANT_TYPE_ARRAY);
		if (!rates)
			rates = g_variant_lookup_value(data, "samplerate-steps",
					G_VARIANT_TYPE_ARRAY);
		if (rates) {
			GVariantIter it;
			GVariant *child;
			g_variant_iter_init(&it, rates);
			while ((child = g_variant_iter_next_value(&it))) {
				g_string_append_printf(j, "%s%llu", first ? "" : ",",
						(unsigned long long)variant_to_u64(child));
				first = FALSE;
				g_variant_unref(child);
			}
			g_variant_unref(rates);
		}
	} else if (g_variant_is_of_type(data, G_VARIANT_TYPE_ARRAY)) {
		/* Plain array of uint64 (session_driver, others). */
		GVariantIter it;
		GVariant *child;
		g_variant_iter_init(&it, data);
		while ((child = g_variant_iter_next_value(&it))) {
			g_string_append_printf(j, "%s%llu", first ? "" : ",",
					(unsigned long long)variant_to_u64(child));
			first = FALSE;
			g_variant_unref(child);
		}
	}
	g_string_append(j, "]");
	g_variant_unref(data);
}

static void append_trigger_matches(GString *j, struct sr_dev_driver *d,
		struct sr_dev_inst *sdi)
{
	GVariant *data;
	GVariantIter iter;
	GVariant *child;
	gboolean first = TRUE;

	if (sr_config_list(d, sdi, NULL, SR_CONF_TRIGGER_MATCH, &data) != SR_OK) {
		g_string_append(j, "\"trigger_matches\":[]");
		return;
	}
	g_string_append(j, "\"trigger_matches\":[");
	g_variant_iter_init(&iter, data);
	while ((child = g_variant_iter_next_value(&iter))) {
		int m = g_variant_get_int32(child);
		g_string_append_printf(j, "%s", first ? "" : ",");
		json_str(j, trigger_match_name(m));
		first = FALSE;
		g_variant_unref(child);
	}
	g_string_append(j, "]");
	g_variant_unref(data);
}

static void append_pattern_modes(GString *j, struct sr_dev_driver *d,
		struct sr_dev_inst *sdi)
{
	GVariant *data;
	GVariantIter iter;
	GVariant *child;
	gboolean first = TRUE;

	if (sr_config_list(d, sdi, NULL, SR_CONF_PATTERN_MODE, &data) != SR_OK) {
		g_string_append(j, "\"pattern_modes\":[]");
		return;
	}
	g_string_append(j, "\"pattern_modes\":[");
	g_variant_iter_init(&iter, data);
	while ((child = g_variant_iter_next_value(&iter))) {
		g_string_append(j, first ? "" : ",");
		json_str(j, g_variant_get_string(child, NULL));
		first = FALSE;
		g_variant_unref(child);
	}
	g_string_append(j, "]");
	g_variant_unref(data);
}

/* Voltage threshold list: (a(tt)t) — array of (min,max) tuples + step. */
static void append_threshold(GString *j, struct sr_dev_driver *d,
		struct sr_dev_inst *sdi)
{
	GVariant *data;

	if (sr_config_list(d, sdi, NULL, SR_CONF_VOLTAGE_THRESHOLD, &data) != SR_OK) {
		g_string_append(j, "\"voltage_threshold\":null");
		return;
	}
	g_string_append(j, "\"voltage_threshold\":{");
	if (g_variant_is_of_type(data, G_VARIANT_TYPE_TUPLE)) {
		GVariant *inner = g_variant_get_child_value(data, 0);
		GVariant *step = g_variant_get_child_value(data, 1);
		GVariantIter it;
		GVariant *child;
		gboolean first = TRUE;

		g_string_append(j, "\"range\":[");
		g_variant_iter_init(&it, inner);
		while ((child = g_variant_iter_next_value(&it))) {
			GVariant *lo = g_variant_get_child_value(child, 0);
			GVariant *hi = g_variant_get_child_value(child, 1);
			g_string_append_printf(j, "%s{\"min\":%.6g,\"max\":%.6g}",
					first ? "" : ",",
					g_variant_get_double(lo),
					g_variant_get_double(hi));
			first = FALSE;
			g_variant_unref(lo);
			g_variant_unref(hi);
			g_variant_unref(child);
		}
		g_string_append_printf(j, "],\"step\":%.6g}",
				g_variant_get_double(step));
		g_variant_unref(inner);
		g_variant_unref(step);
	} else {
		g_string_append(j, "\"raw\":1}");
	}
	g_variant_unref(data);
}

/* Config capabilities as readable list. */
static void append_config_caps(GString *j, struct sr_dev_driver *d,
		struct sr_dev_inst *sdi)
{
	static const struct {
		uint32_t key;
		const char *name;
	} keys[] = {
		{SR_CONF_SAMPLERATE,         "samplerate"},
		{SR_CONF_LIMIT_SAMPLES,      "limit_samples"},
		{SR_CONF_LIMIT_FRAMES,       "limit_frames"},
		{SR_CONF_NUM_LOGIC_CHANNELS, "num_logic_channels"},
		{SR_CONF_CAPTURE_RATIO,      "capture_ratio"},
		{SR_CONF_VOLTAGE_THRESHOLD,  "voltage_threshold"},
		{SR_CONF_PATTERN_MODE,       "pattern_mode"},
		{SR_CONF_CONTINUOUS,         "continuous"},
		{SR_CONF_ADC_PRECISION,      "adc_precision"},
		{SR_CONF_ADC_CHANNEL,        "adc_channel"},
	};
	gboolean first = TRUE;

	g_string_append(j, "\"configs\":{");
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		uint32_t caps = sr_dev_config_capabilities_list(sdi, NULL, keys[i].key);
		if (!caps)
			continue;
		g_string_append_printf(j, "%s\"%s\":\"%s%s%s\"",
				first ? "" : ",", keys[i].name,
				(caps & SR_CONF_GET) ? "get " : "",
				(caps & SR_CONF_SET) ? "set " : "",
				(caps & SR_CONF_LIST) ? "list" : "");
		first = FALSE;
	}
	g_string_append(j, "}");
}

int cmd_capabilities(int argc, char **argv)
{
	GString *j;
	struct sr_dev_driver *d;
	struct sr_dev_inst *sdi;
	const char *conn = NULL, *drvname = NULL;
	GSList *cg_l;
	int i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--conn") && i + 1 < argc)
			conn = argv[++i];
		else if (!strcmp(argv[i], "--driver") && i + 1 < argc)
			drvname = argv[++i];
	}

	d = drvname ? find_driver(drvname) : find_default_capture_driver();
	if (!d)
		die_json("driver '%s' not found", drvname ? drvname : U3CTL_DEFAULT_DRIVERS);

	sdi = pick_device(d, conn, NULL);
	if (!sdi)
		die_json("no device found for driver '%s'%s%s", d->name,
				conn ? " matching conn " : "", conn ? conn : "");

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "driver", d->name);
	g_string_append_c(j, ',');
	json_kv_str(j, "vendor", sr_dev_inst_vendor_get(sdi));
	g_string_append_c(j, ',');
	json_kv_str(j, "model", sr_dev_inst_model_get(sdi));
	g_string_append_c(j, ',');
	json_kv_str(j, "serial", sr_dev_inst_sernum_get(sdi));
	g_string_append_c(j, ',');
	json_kv_str(j, "conn", sr_dev_inst_connid_get(sdi));
	g_string_append_c(j, ',');
	append_samplerate_list(j, d, sdi);
	g_string_append_c(j, ',');
	append_trigger_matches(j, d, sdi);
	g_string_append_c(j, ',');
	append_pattern_modes(j, d, sdi);
	g_string_append_c(j, ',');
	append_threshold(j, d, sdi);
	g_string_append_c(j, ',');
	append_config_caps(j, d, sdi);
	g_string_append_c(j, ',');
	g_string_append(j, "\"channel_groups\":{");
	cg_l = sr_dev_inst_channel_groups_get(sdi);
	gboolean first_cg = TRUE;
	for (; cg_l; cg_l = cg_l->next) {
		struct sr_channel_group *cg = cg_l->data;
		g_string_append(j, first_cg ? "" : ",");
		json_kv_str(j, cg->name, "group");
		first_cg = FALSE;
	}
	g_string_append(j, "}");
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	return 0;
}

/*---- main ------------------------------------------------------------------- */

void usage(void)
{
	fprintf(stderr,
"u3ctl — headless controller for the WeAct U3 logic analyzer\n"
"\n"
"Usage:\n"
"  u3ctl devices list [--conn SUBSTR]\n"
"  u3ctl capabilities [--driver NAME] [--conn SUBSTR]\n"
"  u3ctl capture [--driver NAME] [--conn SUBSTR] [--rate 16M]\n"
"                [--channels N] [--samples 1M | --time 50ms]\n"
"                [--trigger \"D0:rising+D1:high\"] (repeatable, = stages)\n"
"                [--pre 10] [--threshold 1.2V] [--mode logic|adc]\n"
"                [--timeout 15] [--out FILE.sr] [--demo-pattern random]\n"
"  u3ctl session info FILE.sr\n"
"  u3ctl sessions list [--dir DIR]\n"
"  u3ctl session delete FILE.sr\n"
"  u3ctl decode FILE.sr --stack pd1,pd2 [--channel scl=D0] [--pdopt k=v]\n"
"                [--from S --to S] [--class a,b] [--search TEXT]\n"
"                [--max-rows N] [--binary FILE]\n"
"  u3ctl decoders list [--search SUBSTR]\n"
"  u3ctl decoders info PD\n"
"  u3ctl activity FILE.sr [--bucket N] [--channels D0,D1]\n"
"  u3ctl measure FILE.sr [--channels D0,D1] [--from --to]\n"
"  u3ctl edges FILE.sr --channels D0 [--type rising] [--max 200]\n"
"  u3ctl sample FILE.sr --at 8352\n"
"  u3ctl slice FILE.sr --from A --to B [--format hex|bits|base64]\n"
"  u3ctl overview FILE.sr [--points 2000]\n"
"  u3ctl profile FILE.sr\n"
"  u3ctl render FILE.sr --out wave.png [--channels D0,D1] [--from --to]\n"
"                [--width 1600] [--row-height 40] [--format png|svg]\n"
"  u3ctl export FILE.sr --format csv|vcd|... --out FILE\n"
"\n"
"All commands print JSON to stdout. Errors: {\"error\":...} + exit 2.\n");
}

int main(int argc, char **argv)
{
	int loglevel = SR_LOG_WARN;
	int ret;
	const char *cmd, *sub;
	char **clean_argv;
	int clean_argc = 0;

	memset(&G, 0, sizeof(G));
	g_log_set_default_handler(g_log_default_handler, NULL);

	/* Strip global flags (-v/-vv/-h) from a filtered copy of argv. */
	clean_argv = g_new0(char *, argc + 1);
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			loglevel = SR_LOG_INFO;
		else if (!strcmp(argv[i], "-vv") || !strcmp(argv[i], "--debug"))
			loglevel = SR_LOG_DBG;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else
			clean_argv[clean_argc++] = argv[i];
	}

	if (clean_argc < 2) {
		usage();
		return 2;
	}

	G.argv = clean_argv;

	if (u3ctl_init(loglevel) != SR_OK)
		die("initialization failed");

	ret = 2;
	cmd = clean_argv[1];
	sub = clean_argc >= 3 ? clean_argv[2] : NULL;

	if (!strcmp(cmd, "devices") && sub && !strcmp(sub, "list")) {
		ret = cmd_devices_list(clean_argc - 3, clean_argv + 3);
	} else if (!strcmp(cmd, "capabilities")) {
		ret = cmd_capabilities(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "capture")) {
		ret = cmd_capture(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "session") && sub && !strcmp(sub, "info")) {
		ret = cmd_session_info(clean_argc - 3, clean_argv + 3);
	} else if (!strcmp(cmd, "sessions") && sub && !strcmp(sub, "list")) {
		ret = cmd_sessions_list(clean_argc - 3, clean_argv + 3);
	} else if (!strcmp(cmd, "session") && sub && !strcmp(sub, "delete")) {
		ret = cmd_session_delete(clean_argc - 3, clean_argv + 3);
	} else if (!strcmp(cmd, "decode")) {
		ret = cmd_decode(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "decoders") && sub && !strcmp(sub, "list")) {
		ret = cmd_decoders_list(clean_argc - 3, clean_argv + 3);
	} else if (!strcmp(cmd, "decoders") && sub && !strcmp(sub, "info")) {
		ret = cmd_decoders_info(clean_argc - 3, clean_argv + 3);
	} else if (!strcmp(cmd, "activity")) {
		ret = cmd_activity(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "measure")) {
		ret = cmd_measure(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "edges")) {
		ret = cmd_edges(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "sample")) {
		ret = cmd_sample(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "slice")) {
		ret = cmd_slice(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "overview")) {
		ret = cmd_overview(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "profile")) {
		ret = cmd_profile(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "render")) {
		ret = cmd_render(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "export")) {
		ret = cmd_export(clean_argc - 2, clean_argv + 2);
	} else if (!strcmp(cmd, "open")) {
		ret = cmd_open(clean_argc - 2, clean_argv + 2);
	} else {
		usage();
	}

	u3ctl_shutdown();
	return ret;
}
