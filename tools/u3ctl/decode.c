/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * Protocol decoding via libsigrokdecode, mirroring the pipeline in
 * PulseView's pv/data/decodesignal.cpp: stack decoder instances, map
 * channels, feed sample chunks, collect annotations as JSON rows.
 */

#include "u3ctl.h"

/*---- annotation collector -------------------------------------------------- */

struct decode_filters {
	gchar **classes;          /* allowed annotation class ids, NULL = all */
	const char *search;       /* substring, NULL = none */
	char *from_window, *to_window; /* raw --from/--to literals */
	uint64_t from_sample, to_sample;
	gboolean have_from, have_to;
};

struct ann_row {
	uint64_t start, end;
	const char *proto_id;     /* borrowed from pdo */
	const char *class_id;     /* borrowed from decoder annotations list */
	char **text;              /* NULL-terminated, strdup'ed */
};

struct decode_ctx {
	GSList *rows;             /* struct ann_row */
	uint64_t total_rows;
	struct decode_filters *f;
};

static gboolean class_allowed(struct decode_filters *f, const char *class_id)
{
	if (!f->classes)
		return TRUE;
	for (gchar **c = f->classes; *c; c++) {
		if (!strcmp(*c, class_id))
			return TRUE;
	}
	return FALSE;
}

static gboolean text_matches(struct decode_filters *f, char **text)
{
	if (!f->search)
		return TRUE;
	for (char **t = text; *t; t++) {
		if (strstr(*t, f->search))
			return TRUE;
	}
	return FALSE;
}

/* Find the annotation class name (id) for a class index. */
static const char *ann_class_name(struct srd_decoder *dec, int ann_class)
{
	GSList *l;
	int idx = 0;

	for (l = dec->annotations; l; l = l->next, idx++) {
		if (idx == ann_class) {
			/* GSList of GSList of two strings: name, description. */
			GSList *pair = l->data;
			return pair ? pair->data : NULL;
		}
	}
	return NULL;
}

static void ann_row_free(gpointer data)
{
	struct ann_row *row = data;

	for (char **t = row->text; t && *t; t++)
		g_free(*t);
	g_free(row->text);
	g_free(row);
}

static void ann_cb(struct srd_proto_data *pdata, void *cb_data)
{
	struct decode_ctx *c = cb_data;
	struct srd_proto_data_annotation *ann = pdata->data;
	struct srd_decoder_inst *di;
	const char *class_id;
	int n;

	if (!ann)
		return;
	c->total_rows++;
	di = pdata->pdo ? pdata->pdo->di : NULL;
	if (!di)
		return;

	class_id = ann_class_name(di->decoder, ann->ann_class);
	if (!class_id)
		class_id = "?";
	if (!class_allowed(c->f, class_id))
		return;
	if (c->f->have_from && pdata->end_sample < c->f->from_sample)
		return;
	if (c->f->have_to && pdata->start_sample > c->f->to_sample)
		return;
	if (!text_matches(c->f, ann->ann_text))
		return;

	struct ann_row *row = g_new0(struct ann_row, 1);
	row->start = pdata->start_sample;
	row->end = pdata->end_sample;
	row->proto_id = pdata->pdo->proto_id;
	row->class_id = class_id;
	for (n = 0; ann->ann_text && ann->ann_text[n]; n++)
		;
	row->text = g_new0(char *, n + 1);
	for (int i = 0; i < n; i++)
		row->text[i] = g_strdup(ann->ann_text[i]);
	c->rows = g_slist_append(c->rows, row);
}

/*---- binary (payload) collector --------------------------------------------- */

struct bin_ctx {
	FILE *fp;
	uint64_t chunks;
};

static void bin_cb(struct srd_proto_data *pdata, void *cb_data)
{
	struct bin_ctx *b = cb_data;
	struct srd_proto_data_binary *pbin = pdata->data;

	if (!pbin || !pbin->size || !pbin->data)
		return;
	fwrite(pbin->data, 1, pbin->size, b->fp);
	b->chunks++;
}

/*---- decoders path ------------------------------------------------------------ */

/* Resolution order: $SIGROKDECODE_DIR, the repo install prefix, then the
 * library default. Mirrors start_pv.sh for GUI runs. */
const char *decoders_path(void)
{
	static char path_buf[512];
	const char *env = g_getenv("SIGROKDECODE_DIR");

	if (env && env[0])
		return env;
	snprintf(path_buf, sizeof(path_buf),
			U3CTL_PREFIX_PATH "/share/libsigrokdecode/decoders");
	if (g_file_test(path_buf, G_FILE_TEST_IS_DIR))
		return path_buf;
	return NULL;
}

/*---- decoder listing / info --------------------------------------------------- */

int cmd_decoders_list(int argc, char **argv)
{
	GString *j;
	const GSList *l;
	const char *search = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--search") && i + 1 < argc)
			search = argv[++i];
	}

	if (srd_init(decoders_path()) != SRD_OK)
		die_json("srd_init failed");
	srd_decoder_load_all();

	j = g_string_new(NULL);
	g_string_append(j, "{\"decoders\":[");
	gboolean first = TRUE;
	for (l = srd_decoder_list(); l; l = l->next) {
		struct srd_decoder *dec = l->data;
		if (search && !(strstr(dec->id, search) || strstr(dec->name, search)
					|| strstr(dec->longname, search)))
			continue;
		g_string_append(j, first ? "" : ",");
		g_string_append(j, "{\"id\":");
		json_str(j, dec->id);
		g_string_append(j, ",\"name\":");
		json_str(j, dec->name);
		g_string_append(j, ",\"longname\":");
		json_str(j, dec->longname);
		g_string_append_c(j, '}');
		first = FALSE;
	}
	g_string_append(j, "]}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	srd_exit();
	return 0;
}

int cmd_decoders_info(int argc, char **argv)
{
	GString *j;
	const char *pd = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (g_str_has_prefix(argv[i], "--")) {
			i++;
			continue;
		}
		pd = argv[i];
	}
	if (!pd)
		die_json("usage: u3ctl decoders info PD");

	if (srd_init(decoders_path()) != SRD_OK)
		die_json("srd_init failed");
	srd_decoder_load_all();

	struct srd_decoder *dec = srd_decoder_get_by_id(pd);
	if (!dec)
		die_json("decoder '%s' not found", pd);

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "id", dec->id);
	g_string_append_c(j, ',');
	json_kv_str(j, "name", dec->name);
	g_string_append_c(j, ',');
	json_kv_str(j, "longname", dec->longname);
	g_string_append_c(j, ',');
	json_kv_str(j, "desc", dec->desc);
	g_string_append_c(j, ',');
	g_string_append(j, "\"channels\":[");
	gboolean first = TRUE;
	for (GSList *l = dec->channels; l; l = l->next) {
		struct srd_channel *ch = l->data;
		g_string_append_printf(j, "%s{\"id\":", first ? "" : ",");
		json_str(j, ch->id);
		g_string_append(j, ",\"name\":");
		json_str(j, ch->name);
		g_string_append(j, ",\"desc\":");
		json_str(j, ch->desc);
		g_string_append_c(j, '}');
		first = FALSE;
	}
	g_string_append(j, "],\"opt_channels\":[");
	first = TRUE;
	for (GSList *l = dec->opt_channels; l; l = l->next) {
		struct srd_channel *ch = l->data;
		g_string_append_printf(j, "%s{\"id\":", first ? "" : ",");
		json_str(j, ch->id);
		g_string_append(j, ",\"name\":");
		json_str(j, ch->name);
		g_string_append_c(j, '}');
		first = FALSE;
	}
	g_string_append(j, "],\"options\":[");
	first = TRUE;
	for (GSList *l = dec->options; l; l = l->next) {
		struct srd_decoder_option *opt = l->data;
		g_string_append_printf(j, "%s{\"id\":", first ? "" : ",");
		json_str(j, opt->id);
		g_string_append(j, ",\"desc\":");
		json_str(j, opt->desc);
		g_string_append(j, ",\"default\":");
		if (opt->def)
			json_str(j, g_variant_print(opt->def, TRUE));
		else
			g_string_append(j, "null");
		g_string_append_c(j, '}');
		first = FALSE;
	}
	g_string_append(j, "],\"annotation_classes\":[");
	first = TRUE;
	for (GSList *l = dec->annotations; l; l = l->next) {
		GSList *pair = l->data;
		g_string_append(j, first ? "" : ",");
		json_str(j, pair ? pair->data : "?");
		first = FALSE;
	}
	g_string_append(j, "],\"inputs\":[");
	first = TRUE;
	for (GSList *l = dec->inputs; l; l = l->next) {
		g_string_append(j, first ? "" : ",");
		json_str(j, l->data);
		first = FALSE;
	}
	g_string_append(j, "],\"outputs\":[");
	first = TRUE;
	for (GSList *l = dec->outputs; l; l = l->next) {
		g_string_append(j, first ? "" : ",");
		json_str(j, l->data);
		first = FALSE;
	}
	g_string_append(j, "]}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	srd_exit();
	return 0;
}

/*---- decode command ------------------------------------------------------------ */

/* key=value list collector (channels / options) */
struct kv_pair {
	char *k;
	char *v;
};

/* Resolve a device channel name ("D0") or plain number ("3") to its index. */
static int dev_channel_index(struct sr_dev_inst *sdi, const char *name)
{
	for (GSList *cl = sr_dev_inst_channels_get(sdi); cl; cl = cl->next) {
		struct sr_channel *ch = cl->data;
		if (ch->type == SR_CHANNEL_LOGIC && !strcmp(ch->name, name))
			return ch->index;
	}
	return (int)g_ascii_strtoull(name, NULL, 10);
}

static int pd_opt_value(GVariant *def, const char *val, GVariant **out)
{
	if (!def) {
		*out = g_variant_new_string(val);
		return 0;
	}
	if (g_variant_is_of_type(def, G_VARIANT_TYPE_INT64) ||
			g_variant_is_of_type(def, G_VARIANT_TYPE_UINT64)) {
		uint64_t v;
		if (parse_si_u64(val, &v) != 0)
			return -1;
		*out = g_variant_new_int64((int64_t)v);
	} else if (g_variant_is_of_type(def, G_VARIANT_TYPE_DOUBLE)) {
		*out = g_variant_new_double(g_ascii_strtod(val, NULL));
	} else if (g_variant_is_of_type(def, G_VARIANT_TYPE_BOOLEAN)) {
		*out = g_variant_new_boolean(!strcmp(val, "true") || !strcmp(val, "1") ||
				!strcmp(val, "yes"));
	} else {
		*out = g_variant_new_string(val);
	}
	return 0;
}

int cmd_decode(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	struct decode_ctx c;
	struct decode_filters f;
	struct bin_ctx b;
	GHashTable *chanmap = NULL;
	gchar **stack_names = NULL;
	const char *path = NULL;
	const char *out_bin = NULL;
	GSList *kv_channels = NULL, *kv_opts = NULL;
	GSList *instances = NULL;
	struct srd_session *sess = NULL;
	int max_rows = 500;
	int i, ret, rc = 2;

	memset(&c, 0, sizeof(c));
	memset(&b, 0, sizeof(b));
	memset(&f, 0, sizeof(f));

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--stack") && i + 1 < argc) {
			stack_names = g_strsplit(argv[++i], ",", 0);
		} else if (!strcmp(argv[i], "--channel") && i + 1 < argc) {
			gchar **kv = g_strsplit(argv[++i], "=", 2);
			if (g_strv_length(kv) != 2)
				die_json("bad --channel '%s', want pdchannel=devicechannel", argv[i]);
			struct kv_pair *p = g_new0(struct kv_pair, 1);
			p->k = g_strdup(kv[0]);
			p->v = g_strdup(kv[1]);
			kv_channels = g_slist_append(kv_channels, p);
			g_strfreev(kv);
		} else if (!strcmp(argv[i], "--pdopt") && i + 1 < argc) {
			gchar **kv = g_strsplit(argv[++i], "=", 2);
			if (g_strv_length(kv) != 2)
				die_json("bad --pdopt '%s', want key=value", argv[i]);
			struct kv_pair *p = g_new0(struct kv_pair, 1);
			p->k = g_strdup(kv[0]);
			p->v = g_strdup(kv[1]);
			kv_opts = g_slist_append(kv_opts, p);
			g_strfreev(kv);
		} else if (!strcmp(argv[i], "--from") && i + 1 < argc) {
			f.have_from = TRUE;
			f.from_window = argv[++i];
		} else if (!strcmp(argv[i], "--to") && i + 1 < argc) {
			f.have_to = TRUE;
			f.to_window = argv[++i];
		} else if (!strcmp(argv[i], "--class") && i + 1 < argc) {
			f.classes = g_strsplit(argv[++i], ",", 0);
		} else if (!strcmp(argv[i], "--search") && i + 1 < argc) {
			f.search = argv[++i];
		} else if (!strcmp(argv[i], "--max-rows") && i + 1 < argc) {
			max_rows = atoi(argv[++i]);
			if (max_rows <= 0 || max_rows > 10000)
				max_rows = 10000;
		} else if (!strcmp(argv[i], "--binary") && i + 1 < argc) {
			out_bin = argv[++i];
		} else if (!g_str_has_prefix(argv[i], "--")) {
			path = argv[i];
		}
	}
	if (!path)
		die_json("usage: u3ctl decode FILE.sr --stack pd1[,pd2]");
	if (!stack_names)
		die_json("--stack is required, e.g. --stack i2c");

	if (load_session_file(path, &ls) != SR_OK)
		die_json("cannot load session file '%s'", path);

	/* Resolve window specs (sample number or time literal) now that the
	 * samplerate is known. */
	if (f.have_from)
		parse_window(f.from_window, ls.samplerate, &f.from_sample);
	if (f.have_to)
		parse_window(f.to_window, ls.samplerate, &f.to_sample);

	/* Collect the samples (replay through the virtual session device). */
	if (replay_collect(&ls, NULL, NULL) != SR_OK)
		die_json("failed to replay session '%s'", path);
	if (!ls.chunks)
		die_json("session '%s' contains no logic data", path);

	/* Init srd with the resolved decoders path. */
	ret = srd_init(decoders_path());
	if (ret != SRD_OK)
		die_json("srd_init failed");
	srd_decoder_load_all();

	ret = srd_session_new(&sess);
	if (ret != SRD_OK)
		die_json("srd_session_new failed");

	/* Build the stack: first decoder feeds the next via srd_inst_stack. */
	struct srd_decoder_inst *prev = NULL;
	for (gchar **p = stack_names; *p; p++) {
		struct srd_decoder_inst *di = srd_inst_new(sess, *p, NULL);
		if (!di)
			die_json("decoder '%s' not found", *p);
		instances = g_slist_append(instances, di);
		if (prev)
			srd_inst_stack(sess, prev, di);
		prev = di;
	}

	/* Channel map: keys are PD channel ids ("scl"), values are sample
	 * indexes (GVariant int32), as srd_inst_channel_set_all expects. */
	struct srd_decoder_inst *root = instances->data;
	chanmap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)g_variant_unref);
	for (GSList *l = kv_channels; l; l = l->next) {
		struct kv_pair *p = l->data;
		g_hash_table_insert(chanmap, g_strdup(p->k),
				g_variant_new_int32(dev_channel_index(ls.sdi, p->v)));
	}
	/* No explicit mapping: try PD channel id == device channel name. */
	if (g_hash_table_size(chanmap) == 0) {
		for (GSList *l = root->decoder->channels; l; l = l->next) {
			struct srd_channel *ch = l->data;
			for (GSList *cl = sr_dev_inst_channels_get(ls.sdi); cl; cl = cl->next) {
				struct sr_channel *srch = cl->data;
				if (srch->type == SR_CHANNEL_LOGIC && !strcmp(srch->name, ch->id)) {
					g_hash_table_insert(chanmap, g_strdup(ch->id),
							g_variant_new_int32(srch->index));
					break;
				}
			}
		}
	}
	if (g_hash_table_size(chanmap))
		srd_inst_channel_set_all(root, chanmap);

	/* Options: PulseView always sets ALL options (defaults included) —
	 * an unset option dict breaks PDs that index self.options. */
	GHashTable *opts = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)g_variant_unref);
	for (GSList *l = root->decoder->options; l; l = l->next) {
		struct srd_decoder_option *opt = l->data;
		if (opt->def)
			g_hash_table_insert(opts, g_strdup(opt->id),
					g_variant_ref(opt->def));
	}
	for (GSList *l = kv_opts; l; l = l->next) {
		struct kv_pair *p = l->data;
		GVariant *def = NULL;
		for (GSList *ol = root->decoder->options; ol; ol = ol->next) {
			struct srd_decoder_option *opt = ol->data;
			if (!strcmp(opt->id, p->k))
				def = opt->def;
		}
		GVariant *val = NULL;
		if (pd_opt_value(def, p->v, &val) != 0)
			die_json("bad value for --pdopt %s", p->k);
		g_hash_table_insert(opts, g_strdup(p->k), val);
	}
	srd_inst_option_set(root, opts);
	g_hash_table_destroy(opts);

	/* Samplerate must be set after instance creation — metadata_set pushes
	 * to the instances that exist at that point. */
	srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE,
			g_variant_new_uint64(ls.samplerate));

	/* Register callbacks. */
	c.f = &f;
	srd_pd_output_callback_add(sess, SRD_OUTPUT_ANN, ann_cb, &c);
	if (out_bin) {
		b.fp = fopen(out_bin, "wb");
		if (!b.fp)
			die_json("cannot open '%s' for writing", out_bin);
		srd_pd_output_callback_add(sess, SRD_OUTPUT_BINARY, bin_cb, &b);
	}

	/* Run: feed chunk by chunk, then EOF. */
	srd_session_start(sess);
	uint64_t sample = 0;
	for (GSList *l = ls.chunks; l; l = l->next) {
		struct logic_chunk *chunk = l->data;
		uint64_t chunk_samples = chunk->length / (uint64_t)ls.unitsize;
		srd_session_send(sess, sample, sample + chunk_samples,
				chunk->data, chunk->length, (uint64_t)ls.unitsize,
				0, 0);
		sample += chunk_samples;
	}
	srd_session_send_eof(sess);

	/* Emit JSON. */
	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "file", path);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samplerate", ls.samplerate);
	g_string_append_c(j, ',');
	json_kv_u64(j, "total_samples", ls.total_samples);
	g_string_append_c(j, ',');
	g_string_append(j, "\"stack\":[");
	for (gchar **p = stack_names; *p; p++) {
		g_string_append(j, p == stack_names ? "" : ",");
		json_str(j, *p);
	}
	g_string_append(j, "]");
	g_string_append_c(j, ',');
	json_kv_u64(j, "total_rows", c.total_rows);
	g_string_append_c(j, ',');
	json_kv_u64(j, "filtered_rows", g_slist_length(c.rows));
	g_string_append_c(j, ',');
	json_kv_u64(j, "returned", (uint64_t)MIN(g_slist_length(c.rows), (guint)max_rows));
	g_string_append_c(j, ',');
	json_kv_bool(j, "truncated", g_slist_length(c.rows) > (guint)max_rows);
	g_string_append_c(j, ',');
	json_kv_bool(j, "windowed", f.have_from || f.have_to);
	g_string_append_c(j, ',');
	g_string_append(j, "\"rows\":[");
	guint emitted = 0;
	gboolean first = TRUE;
	for (GSList *l = c.rows; l && emitted < (guint)max_rows; l = l->next, emitted++) {
		struct ann_row *row = l->data;
		double st = ls.samplerate ? (double)row->start / ls.samplerate : 0;
		double en = ls.samplerate ? (double)row->end / ls.samplerate : 0;
		g_string_append_printf(j, "%s{\"start_sample\":%llu,\"end_sample\":%llu,"
				"\"start_s\":%.10g,\"end_s\":%.10g,\"decoder\":",
				first ? "" : ",", (unsigned long long)row->start,
				(unsigned long long)row->end, st, en);
		json_str(j, row->proto_id);
		g_string_append(j, ",\"class\":");
		json_str(j, row->class_id);
		g_string_append(j, ",\"text\":[");
		for (char **t = row->text; *t; t++) {
			g_string_append(j, t == row->text ? "" : ",");
			json_str(j, *t);
		}
		g_string_append(j, "]}");
		first = FALSE;
	}
	g_string_append(j, "]");
	if (out_bin) {
		g_string_append_c(j, ',');
		json_kv_str(j, "binary", out_bin);
		g_string_append_c(j, ',');
		json_kv_u64(j, "binary_chunks", b.chunks);
	}
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	rc = 0;

	if (b.fp)
		fclose(b.fp);
	g_hash_table_destroy(chanmap);
	/* Instances are owned by the session and freed with it. */
	g_slist_free(instances);
	srd_session_destroy(sess);
	srd_exit();
	free_loaded_session(&ls);
	g_strfreev(stack_names);
	g_strfreev(f.classes);
	g_slist_free_full(kv_channels, (GDestroyNotify)g_free);
	g_slist_free_full(kv_opts, (GDestroyNotify)g_free);
	return rc;
}
