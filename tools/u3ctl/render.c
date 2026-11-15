/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * render: waveform PNG/SVG of a loaded session. Edges are drawn
 * sample-exact (no smoothing); a red line marks the trigger; the SVG
 * carries channel names and a labelled time ruler, the PNG a plain
 * ruler band. PNG is written through zlib's deflate into a minimal
 * RGB PNG — no image library dependency beyond libz.
 */

#include "u3ctl.h"

#include <math.h>
#include <zlib.h>

/*---- PNG writer ------------------------------------------------------------ */

static void png_chunk(FILE *f, const char *type, const uint8_t *data,
		size_t len)
{
	uint8_t hdr[8];
	uint32_t crc;

	hdr[0] = (uint8_t)(len >> 24);
	hdr[1] = (uint8_t)(len >> 16);
	hdr[2] = (uint8_t)(len >> 8);
	hdr[3] = (uint8_t)len;
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	crc = (uint32_t)crc32(crc32(0, NULL, 0), (const Bytef *)type, 4);
	if (len)
		crc = (uint32_t)crc32(crc, data, (uInt)len);
	uint8_t cb[4] = {
		(uint8_t)(crc >> 24), (uint8_t)(crc >> 16),
		(uint8_t)(crc >> 8), (uint8_t)crc
	};
	fwrite(cb, 1, 4, f);
}

static gboolean png_write(const char *path, uint8_t *rgb,
		uint32_t w, uint32_t h)
{
	FILE *f = fopen(path, "wb");
	uint8_t ihdr[13];
	uint8_t *raw, *comp;
	uLongf comp_len;
	size_t raw_len;
	gboolean ok = FALSE;
	int ret;

	if (!f)
		return FALSE;

	const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);

	memset(ihdr, 0, sizeof(ihdr));
	ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
	ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
	ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
	ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
	ihdr[8] = 8;  /* bit depth */
	ihdr[9] = 2;  /* color type: truecolor RGB */
	png_chunk(f, "IHDR", ihdr, 13);

	/* Scanlines, each prefixed with filter byte 0 (None). */
	raw_len = (size_t)h * (1 + (size_t)w * 3);
	raw = g_malloc(raw_len);
	for (uint32_t y = 0; y < h; y++) {
		raw[y * (1 + (size_t)w * 3)] = 0;
		memcpy(raw + y * (1 + (size_t)w * 3) + 1,
				rgb + (size_t)y * w * 3, (size_t)w * 3);
	}

	comp_len = compressBound(raw_len);
	comp = g_malloc(comp_len);
	ret = compress2(comp, &comp_len, raw, raw_len, Z_DEFAULT_COMPRESSION);
	g_free(raw);
	if (ret != Z_OK) {
		g_free(comp);
		fclose(f);
		return FALSE;
	}
	png_chunk(f, "IDAT", comp, comp_len);
	g_free(comp);

	png_chunk(f, "IEND", NULL, 0);
	ok = !ferror(f);
	fclose(f);
	return ok;
}

/*---- canvas ---------------------------------------------------------------- */

struct canvas {
	uint8_t *px;
	uint32_t w, h;
};

static void px_set(struct canvas *c, uint32_t x, uint32_t y,
		uint8_t r, uint8_t g, uint8_t b)
{
	if (x >= c->w || y >= c->h)
		return;
	uint8_t *p = c->px + ((size_t)y * c->w + x) * 3;
	p[0] = r; p[1] = g; p[2] = b;
}

static void hline(struct canvas *c, uint32_t y, uint32_t x0, uint32_t x1,
		uint8_t r, uint8_t g, uint8_t b)
{
	if (x1 < x0) {
		uint32_t t = x0; x0 = x1; x1 = t;
	}
	for (uint32_t x = x0; x <= x1; x++)
		px_set(c, x, y, r, g, b);
}

static void vline(struct canvas *c, uint32_t x, uint32_t y0, uint32_t y1,
		uint8_t r, uint8_t g, uint8_t b)
{
	if (y1 < y0) {
		uint32_t t = y0; y0 = y1; y1 = t;
	}
	for (uint32_t y = y0; y <= y1; y++)
		px_set(c, x, y, r, g, b);
}

/*---- edge extraction --------------------------------------------------------- */

struct edge_pt {
	uint64_t sample;
	int level;   /* level AFTER the edge */
};

/* All transitions of one channel within [from, to). */
static GSList *channel_edges(struct loaded_session *ls, int index,
		uint64_t from, uint64_t to, int *first_level)
{
	GSList *edges = NULL;
	int prev = -1;

	*first_level = 0;
	for (uint64_t v = from; v < to; v++) {
		const uint8_t *s = sample_ptr(ls, v);
		if (!s)
			break;
		int cur = sample_bit(s, ls->unitsize, index);
		if (prev == -1)
			*first_level = cur;
		else if (cur != prev) {
			struct edge_pt *e = g_new(struct edge_pt, 1);
			e->sample = v;
			e->level = cur;
			edges = g_slist_append(edges, e);
		}
		prev = cur;
	}
	return edges;
}

static void ruler_ticks(struct loaded_session *ls, uint64_t from, uint64_t to,
		double *tps_out, double *step_out)
{
	double span = (double)(to - from);
	double tps = ls->samplerate ? span / (double)ls->samplerate : 0;
	double step_t = tps / 8;
	double mag = pow(10.0, floor(log10(step_t > 0 ? step_t : 1e-12)));
	double norm = step_t / mag;
	double mult = norm < 1.5 ? 1 : norm < 3.5 ? 2 : norm < 7.5 ? 5 : 10;

	*tps_out = tps;
	/* Seconds — sub-sample tick steps are fine for the time axis. */
	*step_out = mag * mult > 0 ? mag * mult : tps / 8;
}

/*---- render -------------------------------------------------------------------- */

int cmd_render(int argc, char **argv)
{
	GString *j;
	struct loaded_session ls;
	uint64_t from, to, span;
	uint32_t width = 1600, row_height = 40;
	const char *out_path = NULL, *format = NULL;
	GSList *chans, *l;
	uint32_t height, ruler = 24, y;
	uint8_t *rgb;
	struct canvas cv;
	gboolean is_svg;
	int nch, i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--width") && i + 1 < argc) {
			width = (uint32_t)atoi(argv[i + 1]);
			if (width < 100 || width > 8192)
				width = 1600;
			i++;
		} else if (!strcmp(argv[i], "--row-height") && i + 1 < argc) {
			row_height = (uint32_t)atoi(argv[i + 1]);
			if (row_height < 8 || row_height > 200)
				row_height = 40;
			i++;
		} else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			out_path = argv[i + 1];
			i++;
		} else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
			format = argv[i + 1];
			i++;
		}
	}
	if (!out_path)
		die_json("--out FILE is required");
	is_svg = format ? !strcmp(format, "svg")
			: g_str_has_suffix(out_path, ".svg");
	if (format && strcmp(format, "png") && strcmp(format, "svg"))
		die_json("bad --format '%s' (png|svg)", format);

	const char *path = query_arg_file(argc, argv);
	if (!path)
		die_json("missing FILE.sr argument");
	if (load_session_file(path, &ls) != SR_OK)
		die_json("cannot load session file '%s'", path);
	if (replay_collect(&ls, NULL, NULL) != SR_OK)
		die_json("failed to replay session '%s'", path);
	if (!ls.chunks)
		die_json("session '%s' contains no logic data", path);
	resolve_window(&ls, argc, argv, &from, &to);
	span = to - from;
	chans = resolve_chans(&ls, argc, argv);
	if (!chans)
		die_json("no logic channels");
	nch = g_slist_length(chans);

	height = ruler + (uint32_t)nch * row_height;
	cv.w = width;
	cv.h = height;
	cv.px = g_malloc((size_t)cv.w * cv.h * 3);
	memset(cv.px, 0xff, (size_t)cv.w * cv.h * 3); /* white */

	double tps;
	double tick_step;
	ruler_ticks(&ls, from, to, &tps, &tick_step);

	/* Light ruler band + ticks (PNG; the SVG draws its own). */
	for (uint32_t x = 0; x < cv.w; x++)
		for (y = 0; y < ruler; y++)
			px_set(&cv, x, y, 246, 246, 248);
	for (double ts = 0; ts <= tps * (1 + 1e-9); ts += tick_step) {
		uint64_t s = from + (uint64_t)(ts * ls.samplerate);
		uint32_t x = (uint32_t)((double)(s - from) * cv.w / span);
		for (y = ruler - 6; y < ruler; y++)
			px_set(&cv, x, y, 140, 140, 150);
	}

	GString *svg = is_svg ? g_string_new(NULL) : NULL;

	uint32_t row = 0;
	for (l = chans; l; l = l->next, row++) {
		struct sr_channel *ch = l->data;
		int first_level, level;
		GSList *edges = channel_edges(&ls, chan_index(ch), from, to,
				&first_level);
		uint32_t top = ruler + row * row_height;
		uint32_t y_high = top + row_height / 4;
		uint32_t y_low = top + row_height * 3 / 4;
		uint32_t x_prev = 0;

		level = first_level;
		for (GSList *e = edges; e; e = e->next) {
			struct edge_pt *ep = e->data;
			uint32_t x = (uint32_t)((double)(ep->sample - from) * cv.w / span);
			if (x >= cv.w)
				x = cv.w - 1;
			hline(&cv, level ? y_high : y_low, x_prev, x, 40, 40, 45);
			vline(&cv, x, y_high, y_low, 40, 40, 45);
			level = ep->level;
			x_prev = x;
		}
		hline(&cv, level ? y_high : y_low, x_prev, cv.w - 1, 40, 40, 45);

		if (svg) {
			g_string_append_printf(svg,
					"<text x=\"4\" y=\"%u\" font-size=\"11\" "
					"fill=\"#333\">%s</text>\n",
					y_high + 3, chan_name(ch));
			g_string_append_printf(svg,
					"<path d=\"M0,%d ", first_level ? y_high : y_low);
			uint32_t xp = 0;
			int lv = first_level;
			for (GSList *e = edges; e; e = e->next) {
				struct edge_pt *ep = e->data;
				uint32_t x = (uint32_t)((double)(ep->sample - from)
						* cv.w / span);
				g_string_append_printf(svg, "L%u,%d L%u,%d L%u,%d ",
						xp, lv ? y_high : y_low,
						x, lv ? y_high : y_low,
						x, !lv ? y_high : y_low);
				lv = ep->level;
				xp = x;
			}
			g_string_append_printf(svg, "L%u,%d\" stroke=\"#28282d\" "
					"fill=\"none\" stroke-width=\"1\"/>\n",
					cv.w - 1, lv ? y_high : y_low);
		}

		g_slist_free_full(edges, g_free);
	}

	/* Trigger marker line. */
	gboolean trig_in = ls.trigger_seen && ls.trigger_pos >= from &&
			ls.trigger_pos < to;
	uint32_t trig_x = 0;
	if (trig_in) {
		trig_x = (uint32_t)((double)(ls.trigger_pos - from) * cv.w / span);
		for (y = 0; y < cv.h; y++)
			px_set(&cv, trig_x, y, 210, 40, 40);
	}

	if (svg) {
		GString *doc = g_string_new(NULL);
		g_string_append_printf(doc,
				"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				"<svg xmlns=\"http://www.w3.org/2000/svg\" "
				"width=\"%u\" height=\"%u\" viewBox=\"0 0 %u %u\">\n"
				"<rect width=\"%u\" height=\"%u\" fill=\"white\"/>\n",
				cv.w, cv.h, cv.w, cv.h, cv.w, cv.h);
		g_string_append_printf(doc,
				"<rect x=\"0\" y=\"0\" width=\"%u\" height=\"%u\" "
				"fill=\"#f6f6f8\"/>\n", cv.w, ruler);
		for (double ts = 0; ts <= tps * (1 + 1e-9); ts += tick_step) {
			uint64_t s = from + (uint64_t)(ts * ls.samplerate);
			uint32_t x = (uint32_t)((double)(s - from) * cv.w / span);
			g_string_append_printf(doc,
					"<line x1=\"%u\" y1=\"%u\" x2=\"%u\" y2=\"%u\" "
					"stroke=\"#8c8c96\"/>\n",
					x, ruler - 6, x, ruler);
			g_string_append_printf(doc,
					"<text x=\"%u\" y=\"%u\" font-size=\"10\" fill=\"#555\" "
					"text-anchor=\"middle\">%.10g s</text>\n",
					x, ruler - 8, ts);
		}
		if (trig_in) {
			g_string_append_printf(doc,
					"<line x1=\"%u\" y1=\"0\" x2=\"%u\" y2=\"%u\" "
					"stroke=\"#d22828\" stroke-dasharray=\"4,3\"/>\n"
					"<text x=\"%u\" y=\"12\" font-size=\"10\" fill=\"#d22828\" "
					"text-anchor=\"middle\">trigger</text>\n",
					trig_x, trig_x, cv.h, trig_x);
		}
		g_string_append(doc, svg->str);
		g_string_append(doc, "</svg>\n");

		FILE *f = fopen(out_path, "wb");
		if (!f)
			die_json("cannot open '%s' for writing", out_path);
		fwrite(doc->str, 1, doc->len, f);
		fclose(f);
		g_string_free(doc, TRUE);
		g_string_free(svg, TRUE);
	} else {
		if (!png_write(out_path, cv.px, cv.w, cv.h))
			die_json("failed to write PNG '%s'", out_path);
	}

	j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "file", out_path);
	g_string_append_c(j, ',');
	json_kv_str(j, "format", is_svg ? "svg" : "png");
	g_string_append_c(j, ',');
	json_kv_u64(j, "width", cv.w);
	g_string_append_c(j, ',');
	json_kv_u64(j, "height", cv.h);
	g_string_append_c(j, ',');
	json_kv_u64(j, "from_sample", from);
	g_string_append_c(j, ',');
	json_kv_u64(j, "to_sample", to);
	g_string_append_c(j, ',');
	json_kv_f(j, "from_s", ls.samplerate ? (double)from / ls.samplerate : 0);
	g_string_append_c(j, ',');
	json_kv_f(j, "to_s", ls.samplerate ? (double)to / ls.samplerate : 0);
	g_string_append_c(j, ',');
	json_kv_bool(j, "trigger_marked", trig_in);
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);

	g_free(cv.px);
	g_slist_free(chans);
	free_loaded_session(&ls);
	return 0;
}
