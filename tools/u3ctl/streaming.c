/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * Streaming capture (plan doc Appendix C): a detached worker process
 * runs the very same capture pipeline as one-shot `capture`, while the
 * caller starts, watches, and stops it through capture-start/status/
 * stop/list. All coordination goes through a per-job state file next to
 * the capture; no daemon, no sockets — the state on disk is the truth.
 */

#include "u3ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/*---- job state --------------------------------------------------------- */

struct job_state {
	char id[80];
	char status[16];        /* arming running stopping done failed interrupted */
	char out[PATH_MAX];
	char note[192];
	int64_t pid;
	int64_t started_ms;
	int64_t updated_ms;
	uint64_t samplerate;
	uint64_t samples;
	uint64_t trigger_pos;
	gboolean trigger_seen;
	gboolean will_auto_stop;
};

static int64_t now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static gboolean status_is_terminal(const char *s)
{
	return !strcmp(s, "done") || !strcmp(s, "failed")
			|| !strcmp(s, "interrupted");
}

static gboolean pid_alive(int64_t pid)
{
	if (pid <= 0)
		return FALSE;
	if (kill((pid_t)pid, 0) == 0)
		return TRUE;
	return errno != ESRCH; /* EPERM: exists, owned elsewhere */
}

/* Flat-JSON field readers — safe against our own writer's format only,
 * which is exactly who writes these files. */
static gboolean st_get_str(const char *body, const char *key,
		char *out, size_t n)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(body, pat);
	if (!p)
		return FALSE;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '"')
		return FALSE;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < n) {
		if (*p == '\\' && p[1])
			p++; /* our writer escapes; keep the raw char */
		out[i++] = *p++;
	}
	out[i] = '\0';
	return TRUE;
}

static int64_t st_get_int(const char *body, const char *key)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(body, pat);
	if (!p)
		return 0;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t')
		p++;
	return (int64_t)g_ascii_strtoll(p, NULL, 10);
}

static gboolean st_get_bool(const char *body, const char *key)
{
	char pat[64];

	snprintf(pat, sizeof(pat), "\"%s\":true", key);
	return strstr(body, pat) != NULL;
}

static gboolean state_write(const char *path, const struct job_state *st)
{
	GString *j = g_string_new(NULL);
	char tmp[PATH_MAX + 8];
	FILE *f;

	g_string_append(j, "{");
	json_kv_str(j, "id", st->id);
	g_string_append_c(j, ',');
	json_kv_str(j, "status", st->status);
	g_string_append_c(j, ',');
	json_kv_str(j, "out", st->out);
	g_string_append_c(j, ',');
	json_kv_i64(j, "pid", st->pid);
	g_string_append_c(j, ',');
	json_kv_i64(j, "started_ms", st->started_ms);
	g_string_append_c(j, ',');
	json_kv_i64(j, "updated_ms", st->updated_ms);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samplerate", st->samplerate);
	g_string_append_c(j, ',');
	json_kv_u64(j, "samples", st->samples);
	g_string_append_c(j, ',');
	json_kv_bool(j, "trigger_seen", st->trigger_seen);
	g_string_append_c(j, ',');
	json_kv_u64(j, "trigger_pos", st->trigger_pos);
	g_string_append_c(j, ',');
	json_kv_bool(j, "will_auto_stop", st->will_auto_stop);
	g_string_append_c(j, ',');
	json_kv_str(j, "note", st->note);
	g_string_append(j, "}");

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "wb");
	if (!f) {
		g_string_free(j, TRUE);
		return FALSE;
	}
	fwrite(j->str, 1, j->len, f);
	fclose(f);
	g_string_free(j, TRUE);
	return rename(tmp, path) == 0;
}

static gboolean state_read(const char *path, struct job_state *st,
		char **body_out)
{
	char *body;
	gsize len;

	if (!g_file_get_contents(path, &body, &len, NULL))
		return FALSE;
	memset(st, 0, sizeof(*st));
	st_get_str(body, "id", st->id, sizeof(st->id));
	st_get_str(body, "status", st->status, sizeof(st->status));
	st_get_str(body, "out", st->out, sizeof(st->out));
	st_get_str(body, "note", st->note, sizeof(st->note));
	st->pid = st_get_int(body, "pid");
	st->started_ms = st_get_int(body, "started_ms");
	st->updated_ms = st_get_int(body, "updated_ms");
	st->samplerate = (uint64_t)st_get_int(body, "samplerate");
	st->samples = (uint64_t)st_get_int(body, "samples");
	st->trigger_pos = (uint64_t)st_get_int(body, "trigger_pos");
	st->trigger_seen = st_get_bool(body, "trigger_seen");
	st->will_auto_stop = st_get_bool(body, "will_auto_stop");
	if (body_out)
		*body_out = body;
	else
		g_free(body);
	return st->id[0] != '\0';
}

/* Directory that holds the job files: the capture's own directory. */
#define U3CTL_JOB_DIR "captures"

static void state_dir_of(const char *out_path, char *dir, size_t n)
{
	if (out_path && out_path[0]) {
		gchar *d = g_path_get_dirname(out_path);
		snprintf(dir, n, "%s", d);
		g_free(d);
	} else {
		snprintf(dir, n, "captures");
	}
}

static void job_paths(const char *dir, const char *id,
		char *state, size_t ns, char *result, size_t nr, char *log, size_t nl)
{
	snprintf(state, ns, "%s/.job-%s.json", dir, id);
	if (result)
		snprintf(result, nr, "%s/.job-%s.result", dir, id);
	if (log)
		snprintf(log, nl, "%s/.job-%s.log", dir, id);
}

/* All job state files in dir, sorted by started_ms ascending. */
static GSList *scan_jobs(const char *dir)
{
	GDir *d;
	const char *name;
	GSList *jobs = NULL;

	d = g_dir_open(dir, 0, NULL);
	if (!d)
		return NULL;
	while ((name = g_dir_read_name(d))) {
		if (!g_str_has_prefix(name, ".job-")
				|| !g_str_has_suffix(name, ".json"))
			continue;
		struct job_state *st = g_new0(struct job_state, 1);
		gchar *p = g_build_filename(dir, name, NULL);
		if (!state_read(p, st, NULL)) {
			g_free(st);
			g_free(p);
			continue;
		}
		st->note[0] = '\0'; /* display only */
		g_free(p);
		jobs = g_slist_prepend(jobs, st);
	}
	g_dir_close(d);
	return jobs;
}

/* The job a bare command means: latest active, else latest overall. */
static gboolean find_job(const char *dir, const char *want_id,
		struct job_state *st_out, char *state_path, size_t nsp)
{
	GSList *jobs = scan_jobs(dir), *l;
	gboolean found_any = FALSE;
	struct job_state latest_any;

	memset(&latest_any, 0, sizeof(latest_any));

	for (l = jobs; l; l = l->next) {
		struct job_state *st = l->data;
		if (want_id && strcmp(st->id, want_id))
			continue;
		if (!found_any || st->started_ms > latest_any.started_ms) {
			latest_any = *st;
			found_any = TRUE;
		}
		if (!status_is_terminal(st->status) && pid_alive(st->pid)) {
			/* latest active wins immediately */
			*st_out = *st;
			job_paths(dir, st->id, state_path, nsp, NULL, 0, NULL, 0);
			g_slist_free_full(jobs, g_free);
			return TRUE;
		}
	}
	g_slist_free_full(jobs, g_free);
	if (found_any) {
		*st_out = latest_any;
		job_paths(dir, latest_any.id, state_path, nsp, NULL, 0, NULL, 0);
		return TRUE;
	}
	return FALSE;
}

/*---- worker --------------------------------------------------------------- */

static volatile sig_atomic_t worker_stop_flag = 0;

static void worker_sig_handler(int sig)
{
	(void)sig;
	worker_stop_flag = 1;
}

struct worker_ctx {
	struct job_state *st;
	char state_path[PATH_MAX];
	int64_t last_write_ms;
};

static void worker_progress(uint64_t samples, gboolean trigger_seen,
		uint64_t trigger_pos, void *user)
{
	struct worker_ctx *w = user;
	int64_t t = now_ms();

	w->st->samples = samples;
	w->st->trigger_seen = trigger_seen;
	w->st->trigger_pos = trigger_pos;

	if (!strcmp(w->st->status, "arming")) {
		snprintf(w->st->status, sizeof(w->st->status), "running");
		w->last_write_ms = 0; /* first sample batch writes at once */
	}
	if (t - w->last_write_ms < 500)
		return;
	w->last_write_ms = t;
	w->st->updated_ms = t;
	state_write(w->state_path, w->st);
}

int cmd_worker(int argc, char **argv)
{
	struct cap_opts o;
	struct capture_result res;
	struct job_state st;
	struct worker_ctx w;
	char state_path[PATH_MAX], result_path[PATH_MAX], dir[PATH_MAX];
	char result_json[8192];
	int i;
	const char *job_id = NULL;
	int rest_start = 0;

	/* u3ctl __worker --job ID [--] <capture args...> */
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--job") && i + 1 < argc) {
			job_id = argv[++i];
			rest_start = i + 1;
		} else if (!strcmp(argv[i], "--")) {
			rest_start = i + 1;
			break;
		}
	}
	if (!job_id || rest_start >= argc)
		die_json("__worker: --job ID and capture args required");

	parse_capture_args(argc - rest_start, argv + rest_start, &o);

	state_dir_of(o.out_path, dir, sizeof(dir));
	g_mkdir_with_parents(dir, 0755);
	job_paths(dir, job_id, state_path, sizeof(state_path),
			result_path, sizeof(result_path), NULL, 0);

	memset(&st, 0, sizeof(st));
	snprintf(st.id, sizeof(st.id), "%s", job_id);
	snprintf(st.status, sizeof(st.status), "arming");
	st.pid = getpid();
	st.started_ms = now_ms();
	st.updated_ms = st.started_ms;
	st.will_auto_stop = o.limit_samples > 0 || o.limit_time_samples > 0;
	if (!o.out_path || !o.out_path[0]) {
		time_t now = time(NULL);
		char auto_name[128];
		strftime(auto_name, sizeof(auto_name),
				"captures/cap-%Y%m%d-%H%M%S.sr", localtime(&now));
		o.out_path = g_strdup(auto_name);
	}
	snprintf(st.out, sizeof(st.out), "%s", o.out_path);
	state_write(state_path, &st);

	signal(SIGTERM, worker_sig_handler);
	signal(SIGINT, worker_sig_handler);

	memset(&w, 0, sizeof(w));
	w.st = &st;
	snprintf(w.state_path, sizeof(w.state_path), "%s", state_path);
	w.last_write_ms = now_ms();

	run_capture_session(&o, &res, worker_progress, &w, &worker_stop_flag);

	/* Final result goes to its own file; capture-stop prints it. */
	{
		GString *j = capture_result_json(&o, res.driver, &res);
		g_string_append_printf(j, ",\"job\":\"%s\",\"state\":\"done\"}",
				job_id);
		snprintf(result_json, sizeof(result_json), "%s", j->str);
		g_string_free(j, TRUE);
	}
	{
		FILE *f = fopen(result_path, "wb");
		if (f) {
			fwrite(result_json, 1, strlen(result_json), f);
			fclose(f);
		}
	}

	snprintf(st.status, sizeof(st.status), "done");
	st.samples = res.samples;
	st.samplerate = res.rate;
	st.trigger_seen = res.trigger_seen;
	st.trigger_pos = res.trigger_pos;
	st.updated_ms = now_ms();
	state_write(state_path, &st);

	free_cap_opts(&o);
	g_free(res.conn);
	g_free(res.driver);
	return 0;
}

/*---- capture-start ---------------------------------------------------------- */

static void self_exe(char *buf, size_t n)
{
	uint32_t size = (uint32_t)n;

	if (_NSGetExecutablePath(buf, &size) != 0)
		die_json("cannot resolve own executable path");
}

int cmd_capture_start(int argc, char **argv)
{
	struct cap_opts o;
	struct job_state st;
	char dir[PATH_MAX], state_path[PATH_MAX], log_path[PATH_MAX];
	char job_id[80], exe[PATH_MAX];
	char **wargv;
	int wn = 0, i, ret;
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	pid_t pid = -1;
	char now_s[32];
	int64_t t0;

	parse_capture_args(argc, argv, &o);
	/* Streaming default timeout is the safety line for a long watch,
	 * not the length of the capture. */
	if (!o.timeout_explicit)
		o.timeout_s = 600;
	if (!o.out_path || !o.out_path[0]) {
		time_t now = time(NULL);
		char auto_name[128];
		strftime(auto_name, sizeof(auto_name),
				"captures/cap-%Y%m%d-%H%M%S.sr", localtime(&now));
		o.out_path = g_strdup(auto_name);
	}
	if (!o.overwrite && g_file_test(o.out_path, G_FILE_TEST_EXISTS))
		die_json("output file '%s' exists, use --overwrite", o.out_path);

	state_dir_of(o.out_path, dir, sizeof(dir));
	g_mkdir_with_parents(dir, 0755);

	/* One active job at a time: the device is exclusive. */
	if (find_job(dir, NULL, &st, state_path, sizeof(state_path))
			&& !status_is_terminal(st.status) && pid_alive(st.pid))
		die_json("device busy: job '%s' is still %s (stop it first)",
				st.id, st.status);

	/* Job id; a same-second restart gets a suffix. */
	{
		time_t now = time(NULL);
		strftime(now_s, sizeof(now_s), "%Y%m%d-%H%M%S", localtime(&now));
		for (int seq = 0; ; seq++) {
			if (seq)
				snprintf(job_id, sizeof(job_id), "job-%s-%d", now_s, seq + 1);
			else
				snprintf(job_id, sizeof(job_id), "job-%s", now_s);
			char probe[PATH_MAX + 64];
			job_paths(dir, job_id, probe, sizeof(probe), NULL, 0, NULL, 0);
			if (!g_file_test(probe, G_FILE_TEST_EXISTS))
				break;
		}
	}
	job_paths(dir, job_id, state_path, sizeof(state_path), NULL, 0,
			log_path, sizeof(log_path));

	self_exe(exe, sizeof(exe));

	/* argv: self __worker --job ID -- <original args> --out resolved
	 *        --overwrite  (trailing --out wins in the parser) */
	/* 5 fixed head entries + argc forwarded + 3 trailing = argc + 8 */
	wargv = g_new0(char *, argc + 9);
	wargv[wn++] = exe;
	wargv[wn++] = "__worker";
	wargv[wn++] = "--job";
	wargv[wn++] = job_id;
	wargv[wn++] = "--";
	for (i = 0; i < argc; i++)
		wargv[wn++] = argv[i];
	wargv[wn++] = "--out";
	wargv[wn++] = o.out_path;
	wargv[wn++] = "--overwrite";

	int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (logfd < 0)
		die_json("cannot open job log '%s'", log_path);

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, logfd, 1);
	posix_spawn_file_actions_adddup2(&fa, logfd, 2);
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	ret = posix_spawn(&pid, exe, &fa, &attr, wargv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	close(logfd);
	g_free(wargv);
	if (ret != 0)
		die_json("failed to spawn worker: %s", strerror(ret));

	/* Wait for the worker's first state write so the caller can poll
	 * immediately. */
	t0 = now_ms();
	while (now_ms() - t0 < 1500) {
		struct job_state probe;
		if (state_read(state_path, &probe, NULL) && probe.pid == pid)
			break;
		usleep(50000);
	}
	if (!g_file_test(state_path, G_FILE_TEST_EXISTS))
		die_json("worker did not start (see %s)", log_path);

	{
		GString *j = g_string_new(NULL);
		g_string_append(j, "{");
		json_kv_str(j, "job", job_id);
		g_string_append_c(j, ',');
		json_kv_i64(j, "pid", pid);
		g_string_append_c(j, ',');
		json_kv_str(j, "out", o.out_path);
		g_string_append_c(j, ',');
		json_kv_bool(j, "will_auto_stop",
				o.limit_samples > 0 || o.limit_time_samples > 0);
		g_string_append_c(j, ',');
		json_kv_i64(j, "timeout_s", o.timeout_s);
		g_string_append_c(j, ',');
		json_kv_str(j, "log", log_path);
		g_string_append(j, "}");
		printf("%s\n", j->str);
		g_string_free(j, TRUE);
	}

	free_cap_opts(&o);
	return 0;
}

/*---- capture-status ---------------------------------------------------------- */

int cmd_capture_status(int argc, char **argv)
{
	const char *want = NULL;
	char dir[PATH_MAX], state_path[PATH_MAX];
	struct job_state st;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--job") && i + 1 < argc)
			want = argv[++i];
	}
	snprintf(dir, sizeof(dir), "%s", U3CTL_JOB_DIR);
	if (!find_job(dir, want, &st, state_path, sizeof(state_path)))
		die_json("no job found%s%s", want ? " with id " : "",
				want ? want : "");

	gboolean alive = pid_alive(st.pid);
	/* Orphan detection: a non-terminal state whose worker is gone is
	 * reported honestly, never as a clean done. */
	if (!status_is_terminal(st.status) && !alive) {
		snprintf(st.status, sizeof(st.status), "%s",
				!strcmp(st.status, "arming") ? "failed" : "interrupted");
		snprintf(st.note, sizeof(st.note),
				"worker died (pid %lld)", (long long)st.pid);
		st.updated_ms = now_ms();
		state_write(state_path, &st);
	}

	{
		GString *j = g_string_new(NULL);
		g_string_append(j, "{");
		json_kv_str(j, "job", st.id);
		g_string_append_c(j, ',');
		json_kv_str(j, "status", st.status);
		g_string_append_c(j, ',');
		json_kv_bool(j, "pid_alive", pid_alive(st.pid));
		g_string_append_c(j, ',');
		json_kv_i64(j, "pid", st.pid);
		g_string_append_c(j, ',');
		json_kv_str(j, "out", st.out);
		g_string_append_c(j, ',');
		json_kv_u64(j, "samplerate", st.samplerate);
		g_string_append_c(j, ',');
		json_kv_u64(j, "samples", st.samples);
		g_string_append_c(j, ',');
		json_kv_bool(j, "trigger_seen", st.trigger_seen);
		if (st.trigger_seen) {
			g_string_append_c(j, ',');
			json_kv_u64(j, "trigger_pos", st.trigger_pos);
		}
		g_string_append_c(j, ',');
		json_kv_bool(j, "will_auto_stop", st.will_auto_stop);
		g_string_append_c(j, ',');
		json_kv_i64(j, "started_ms", st.started_ms);
		g_string_append_c(j, ',');
		json_kv_i64(j, "updated_ms", st.updated_ms);
		if (st.note[0]) {
			g_string_append_c(j, ',');
			json_kv_str(j, "note", st.note);
		}
		g_string_append(j, "}");
		printf("%s\n", j->str);
		g_string_free(j, TRUE);
	}
	(void)alive;
	return 0;
}

/*---- capture-stop ------------------------------------------------------------- */

int cmd_capture_stop(int argc, char **argv)
{
	const char *want = NULL;
	gboolean do_open = FALSE;
	char dir[PATH_MAX], state_path[PATH_MAX], result_path[PATH_MAX];
	struct job_state st;
	int64_t t0;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--job") && i + 1 < argc)
			want = argv[++i];
		else if (!strcmp(argv[i], "--open"))
			do_open = TRUE;
	}
	snprintf(dir, sizeof(dir), "%s", U3CTL_JOB_DIR);
	if (!find_job(dir, want, &st, state_path, sizeof(state_path)))
		die_json("no job found%s%s", want ? " with id " : "",
				want ? want : "");
	job_paths(dir, st.id, state_path, sizeof(state_path),
			result_path, sizeof(result_path), NULL, 0);

	if (status_is_terminal(st.status)) {
		/* Per spec C.5: stopping a finished job is an explicit error —
		 * the result is already on disk, not produced by this stop. */
		die_json("job '%s' is already %s (result: %s)",
				st.id, st.status,
				g_file_test(result_path, G_FILE_TEST_EXISTS)
					? result_path : "none");
	}

	/* Graceful stop: SIGTERM, the worker ends the session and finishes
	 * the .sr itself. */
	snprintf(st.status, sizeof(st.status), "stopping");
	state_write(state_path, &st);
	kill((pid_t)st.pid, SIGTERM);

	t0 = now_ms();
	while (now_ms() - t0 < 5000) {
		struct job_state probe;
		if (state_read(state_path, &probe, NULL)
				&& status_is_terminal(probe.status)) {
			st = probe;
			break;
		}
		usleep(50000);
	}

	if (!status_is_terminal(st.status)) {
		/* It refused to die politely: kill it and say what happened. */
		kill((pid_t)st.pid, SIGKILL);
		waitpid((pid_t)st.pid, NULL, WNOHANG);
		snprintf(st.status, sizeof(st.status), "interrupted");
		snprintf(st.note, sizeof(st.note),
				"did not stop after SIGTERM; worker killed");
		st.updated_ms = now_ms();
		state_write(state_path, &st);
	}

	if (!strcmp(st.status, "done")
			&& g_file_test(result_path, G_FILE_TEST_EXISTS)) {
		gchar *body;
		gsize len;
		if (g_file_get_contents(result_path, &body, &len, NULL)) {
			printf("%s\n", body);
			g_free(body);
			if (do_open && st.out[0]) {
				char abs[PATH_MAX];
				const char *p = st.out;
				if (realpath(st.out, abs))
					p = abs;
				open_in_gui(p);
			}
			return 0;
		}
	}

	/* interrupted / failed: honest report, distinct exit code */
	{
		GString *j = g_string_new(NULL);
		g_string_append(j, "{");
		json_kv_str(j, "job", st.id);
		g_string_append_c(j, ',');
		json_kv_str(j, "status", st.status);
		g_string_append_c(j, ',');
		json_kv_str(j, "file", st.out);
		g_string_append_c(j, ',');
		json_kv_u64(j, "samples", st.samples);
		if (st.note[0]) {
			g_string_append_c(j, ',');
			json_kv_str(j, "note", st.note);
		}
		g_string_append(j, "}");
		printf("%s\n", j->str);
		g_string_free(j, TRUE);
	}
	return 1;
}

/*---- capture-list ---------------------------------------------------------------- */

static int cmp_started(gconstpointer a, gconstpointer b)
{
	const struct job_state *x = a, *y = b;

	return x->started_ms < y->started_ms ? -1 : x->started_ms > y->started_ms;
}

int cmd_capture_list(int argc, char **argv)
{
	char dir[PATH_MAX];
	GSList *jobs, *l;
	GString *j;

	(void)argc;
	(void)argv;
	snprintf(dir, sizeof(dir), "%s", U3CTL_JOB_DIR);
	jobs = g_slist_sort(scan_jobs(dir), cmp_started);

	j = g_string_new(NULL);
	g_string_append(j, "{\"jobs\":[");
	gboolean first = TRUE;
	for (l = jobs; l; l = l->next) {
		struct job_state *st = l->data;
		g_string_append(j, first ? "" : ",");
		g_string_append_printf(j,
			"{\"job\":\"%s\",\"status\":\"%s\",\"pid_alive\":%s,"
			"\"samples\":%llu,\"out\":\"%s\",\"started_ms\":%lld}",
			st->id, st->status, pid_alive(st->pid) ? "true" : "false",
			(unsigned long long)st->samples, st->out,
			(long long)st->started_ms);
		first = FALSE;
	}
	g_string_append(j, "]}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
	g_slist_free_full(jobs, g_free);
	return 0;
}
