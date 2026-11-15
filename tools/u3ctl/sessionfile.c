/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * .sr session files: load, info, list, delete, export.
 *
 * Loading uses sr_session_load(); the virtual session_driver device then
 * replays the stored logic data through the datafeed when the session runs.
 */

#include "u3ctl.h"

/*---- replay infrastructure ------------------------------------------------ */

static void collect_datafeed(const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet, void *cb_data)
{
	struct loaded_session *ls = cb_data;

	(void)sdi;
	switch (packet->type) {
	case SR_DF_HEADER:
		break;
	case SR_DF_META: {
		const struct sr_datafeed_meta *meta = packet->payload;
		for (GSList *l = meta->config; l; l = l->next) {
			const struct sr_config *src = l->data;
			if (src->key == SR_CONF_SAMPLERATE)
				ls->samplerate = g_variant_get_uint64(src->data);
		}
		break;
	}
	case SR_DF_TRIGGER:
		if (packet->payload) {
			ls->trigger_pos = *(const uint64_t *)packet->payload;
			ls->trigger_seen = TRUE;
		}
		break;
	case SR_DF_LOGIC: {
		const struct sr_datafeed_logic *logic = packet->payload;
		struct logic_chunk *chunk = g_new0(struct logic_chunk, 1);

		chunk->data = g_malloc(logic->length);
		memcpy(chunk->data, logic->data, logic->length);
		chunk->length = logic->length;
		chunk->start_sample = ls->total_samples;
		ls->unitsize = logic->unitsize;
		ls->total_samples += logic->length / logic->unitsize;
		ls->chunks = g_slist_append(ls->chunks, chunk);
		break;
	}
	case SR_DF_END:
		break;
	}
}

int replay_collect(struct loaded_session *ls, GSList **chunks,
		uint64_t *total_samples)
{
	int ret;

	ret = sr_session_datafeed_callback_add(ls->session, collect_datafeed, ls);
	if (ret != SR_OK)
		return ret;
	ret = sr_session_start(ls->session);
	if (ret != SR_OK)
		return ret;
	ret = sr_session_run(ls->session);
	if (ret != SR_OK)
		return ret;
	if (chunks)
		*chunks = ls->chunks;
	if (total_samples)
		*total_samples = ls->total_samples;
	return SR_OK;
}

/*---- load / free ------------------------------------------------------------ */

int load_session_file(const char *path, struct loaded_session *ls)
{
	GSList *devlist = NULL;
	int ret;

	memset(ls, 0, sizeof(*ls));

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
		return SR_ERR_ARG; /* caller reports */

	ret = sr_session_load(G.sr_ctx, path, &ls->session);
	if (ret != SR_OK)
		return ret;

	ret = sr_session_dev_list(ls->session, &devlist);
	if (ret != SR_OK || !devlist)
		return SR_ERR_NA;
	ls->sdi = devlist->data;
	g_slist_free(devlist);

	/* Samplerate: session_driver exposes it via config_get. */
	struct sr_dev_driver *d = sr_dev_inst_driver_get(ls->sdi);
	if (d) {
		GVariant *gv = NULL;
		if (sr_config_get(d, ls->sdi, NULL, SR_CONF_SAMPLERATE, &gv) == SR_OK
				&& gv) {
			ls->samplerate = g_variant_get_uint64(gv);
			g_variant_unref(gv);
		}
	}
	return SR_OK;
}

void free_loaded_session(struct loaded_session *ls)
{
	for (GSList *l = ls->chunks; l; l = l->next) {
		struct logic_chunk *c = l->data;
		g_free(c->data);
		g_free(c);
	}
	g_slist_free(ls->chunks);
	ls->chunks = NULL;
	if (ls->session)
		sr_session_destroy(ls->session);
	ls->session = NULL;
}

/*---- session info ------------------------------------------------------------ */

int cmd_session_info(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	const char *path = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (g_str_has_prefix(argv[i], "--")) {
			i++; /* skip unknown option value */
			continue;
		}
		path = argv[i];
	}
	if (!path)
		die_json("usage: u3ctl session info FILE.sr");

	if (load_session_file(path, &ls) != SR_OK)
		die_json("cannot load session file '%s'", path);

	if (replay_collect(&ls, NULL, NULL) != SR_OK)
		die_json("failed to replay session '%s'", path);

	double duration = ls.samplerate ? (double)ls.total_samples / ls.samplerate : 0.0;

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "file", path);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samplerate", ls.samplerate);
	g_string_append_c(j, ',');
	json_kv_u64(j, "total_samples", ls.total_samples);
	g_string_append_c(j, ',');
	json_kv_f(j, "duration_s", duration);
	g_string_append_c(j, ',');
	json_kv_i64(j, "unitsize", ls.unitsize);
	g_string_append_c(j, ',');
	json_kv_bool(j, "trigger", ls.trigger_seen);
	if (ls.trigger_seen) {
		g_string_append_c(j, ',');
		json_kv_u64(j, "trigger_position_sample", ls.trigger_pos);
	}
	g_string_append_c(j, ',');
	g_string_append(j, "\"channels\":[");
	gboolean first = TRUE;
	for (GSList *l = sr_dev_inst_channels_get(ls.sdi); l; l = l->next) {
		struct sr_channel *ch = l->data;
		g_string_append_printf(j, "%s{\"name\":", first ? "" : ",");
		json_str(j, ch->name);
		g_string_append_printf(j, ",\"type\":\"%s\",\"index\":%d,\"enabled\":%s}",
				ch->type == SR_CHANNEL_LOGIC ? "logic" : "analog",
				ch->index, ch->enabled ? "true" : "false");
		first = FALSE;
	}
	g_string_append(j, "]}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	free_loaded_session(&ls);
	return 0;
}

/*---- sessions list / delete ---------------------------------------------------- */

int cmd_sessions_list(int argc, char **argv)
{
	GString *j;
	const char *dir = ".";
	int i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--dir") && i + 1 < argc)
			dir = argv[++i];
	}

	j = g_string_new(NULL);
	g_string_append(j, "{\"dir\":");
	json_str(j, dir);
	g_string_append(j, ",\"sessions\":[");
	gboolean first = TRUE;
	GDir *d = g_dir_open(dir, 0, NULL);
	if (d) {
		const gchar *name;
		while ((name = g_dir_read_name(d))) {
			if (!g_str_has_suffix(name, ".sr"))
				continue;
			gchar *full = g_build_filename(dir, name, NULL);
			if (!g_file_test(full, G_FILE_TEST_IS_REGULAR)) {
				g_free(full);
				continue;
			}
			g_string_append(j, first ? "" : ",");
			json_str(j, full);
			first = FALSE;
			g_free(full);
		}
		g_dir_close(d);
	}
	g_string_append(j, "]}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	return 0;
}

int cmd_session_delete(int argc, char **argv)
{
	const char *path = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (g_str_has_prefix(argv[i], "--")) {
			i++;
			continue;
		}
		path = argv[i];
	}
	if (!path)
		die_json("usage: u3ctl session delete FILE.sr");
	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
		die_json("no such session file '%s'", path);

	GString *j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "deleted", path);
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	return remove(path) == 0 ? 0 : 1;
}

/*---- export --------------------------------------------------------------------- */

struct export_ctx {
	const struct sr_output *output;
	FILE *fp;                 /* for modules that return text (csv/vcd/...) */
	gboolean writes_own_file; /* srzip & friends (SR_OUTPUT_INTERNAL_IO_HANDLING) */
	uint64_t samples_sent;
};

static void export_datafeed(const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet, void *cb_data)
{
	struct export_ctx *e = cb_data;
	GString *out = NULL;

	(void)sdi;
	if (!e->output)
		return;
	/* Streaming modules expect the full stream including META and END. */
	sr_output_send(e->output, packet, &out);
	if (out) {
		if (out->len && e->fp)
			fwrite(out->str, 1, out->len, e->fp);
		g_string_free(out, TRUE);
	}
	if (packet->type == SR_DF_LOGIC) {
		const struct sr_datafeed_logic *logic = packet->payload;
		e->samples_sent += logic->length / logic->unitsize;
	}
}

int cmd_export(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	struct export_ctx e;
	const struct sr_output_module *omod;
	const char *path = NULL, *fmt = NULL, *out_path = NULL;
	gboolean overwrite = FALSE;
	int i, ret;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--format") && i + 1 < argc) {
			fmt = argv[++i];
		} else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			out_path = argv[++i];
		} else if (!strcmp(argv[i], "--overwrite")) {
			overwrite = TRUE;
		} else if (!g_str_has_prefix(argv[i], "--")) {
			path = argv[i];
		}
	}
	if (!path || !fmt || !out_path)
		die_json("usage: u3ctl export FILE.sr --format csv --out FILE");

	omod = sr_output_find((char *)fmt);
	if (!omod)
		die_json("unknown output format '%s'", fmt);
	if (!overwrite && g_file_test(out_path, G_FILE_TEST_EXISTS))
		die_json("output file '%s' exists, use --overwrite", out_path);
	if (overwrite && g_file_test(out_path, G_FILE_TEST_EXISTS))
		remove(out_path);

	if (load_session_file(path, &ls) != SR_OK)
		die_json("cannot load session file '%s'", path);

	memset(&e, 0, sizeof(e));
	e.writes_own_file = sr_output_test_flag(omod, SR_OUTPUT_INTERNAL_IO_HANDLING);
	if (!e.writes_own_file) {
		e.fp = fopen(out_path, "wb");
		if (!e.fp)
			die_json("cannot open '%s' for writing", out_path);
	}
	e.output = sr_output_new(omod, NULL, ls.sdi,
			e.writes_own_file ? out_path : NULL);
	if (!e.output)
		die_json("failed to create '%s' output", fmt);

	ret = sr_session_datafeed_callback_add(ls.session, export_datafeed, &e);
	if (ret == SR_OK)
		ret = sr_session_start(ls.session);
	if (ret == SR_OK)
		ret = sr_session_run(ls.session);
	sr_output_free(e.output);
	if (e.fp)
		fclose(e.fp);

	if (ret != SR_OK)
		die_json("export failed: %s", sr_strerror(ret));

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "file", out_path);
	g_string_append_c(j, ',');
	json_kv_str(j, "format", fmt);
	g_string_append_c(j, ',');
	json_kv_str(j, "source", path);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samples", e.samples_sent);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samplerate", ls.samplerate);
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	free_loaded_session(&ls);
	return 0;
}
