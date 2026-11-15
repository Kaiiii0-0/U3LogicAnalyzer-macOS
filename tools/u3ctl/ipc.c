/*
 * This file is part of the WeActStudio.U3LogicAnalyzer project.
 *
 * u3ctl — headless CLI controller for the U3 logic analyzer.
 * IPC client: hand a capture path to the running GUI instance so it opens
 * there instead of spawning a new window.
 *
 * The GUI listens on a QLocalServer whose socket file is the same path
 * this client connects to (see pulseview/pv/application.cpp
 * ipc_socket_name()). Protocol: one line "<absolute path>\n" in,
 * "OK\n" back. Windows named pipes are not wired yet — the GUI has the
 * server side, the CLI catches up when Windows builds matter.
 */

#include "u3ctl.h"

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>

#define IPC_OPEN_OK    0
#define IPC_OPEN_NOGUI 1

static void ipc_socket_path(char *buf, size_t len)
{
	snprintf(buf, len, "/tmp/u3la-open-%d.sock", (int)getuid());
}

/*
 *  0 = delivered to the running GUI instance
 *  1 = no instance is listening (caller decides what that means)
 * <0 = error
 */
int open_in_gui(const char *path)
{
	char sockpath[256];
	struct sockaddr_un addr;
	char abspath[PATH_MAX];
	char req[PATH_MAX + 2];
	char ack[16];
	int fd, ret;
	ssize_t n;

	if (!path || !*path)
		return -1;
	if (!realpath(path, abspath))
		return -1;

	ipc_socket_path(sockpath, sizeof(sockpath));

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(sockpath) >= sizeof(addr.sun_path)) {
		close(fd);
		return -1;
	}
	strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		/* ECONNREFUSED / ENOENT: no GUI running. */
		close(fd);
		return IPC_OPEN_NOGUI;
	}

	n = snprintf(req, sizeof(req), "%s\n", abspath);
	if (write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}

	/* Wait for the instance's ack, bounded — a hung GUI must not hang
	 * the agent's capture command. */
	struct timeval tv = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	n = read(fd, ack, sizeof(ack) - 1);
	close(fd);

	if (n <= 0)
		return -1;
	ack[n] = '\0';
	ret = strncmp(ack, "OK", 2) == 0 ? IPC_OPEN_OK : -1;
	return ret;
}

#else /* _WIN32 */

int open_in_gui(const char *path)
{
	(void)path;
	/* Named-pipe client pending; report as unavailable. */
	return 1;
}

#endif

void cmd_open_json(const char *path, int ret)
{
	GString *j = g_string_new(NULL);
	g_string_append(j, "{");
	json_kv_str(j, "file", path);
	g_string_append_c(j, ',');
	switch (ret) {
	case IPC_OPEN_OK:
		json_kv_str(j, "gui", "existing-instance");
		break;
	case IPC_OPEN_NOGUI:
		json_kv_bool(j, "gui_opened", FALSE);
		g_string_append_c(j, ',');
		json_kv_str(j, "reason", "no running GUI instance");
		break;
	default:
		json_kv_bool(j, "gui_opened", FALSE);
		g_string_append_c(j, ',');
		json_kv_str(j, "reason", "ipc error");
	}
	g_string_append(j, "}");
	printf("%s\n", j->str);
	g_string_free(j, TRUE);
}

int cmd_open(int argc, char **argv)
{
	const char *path = NULL;

	for (int i = 0; i < argc; i++) {
		if (g_str_has_prefix(argv[i], "--")) {
			i++;
			continue;
		}
		path = argv[i];
	}
	if (!path)
		die_json("usage: u3ctl open FILE.sr");
	if (!g_file_test(path, G_FILE_TEST_EXISTS))
		die_json("no such file '%s'", path);

	int ret = open_in_gui(path);
	cmd_open_json(path, ret);
	return ret == IPC_OPEN_OK ? 0 : (ret == IPC_OPEN_NOGUI ? 1 : 2);
}
