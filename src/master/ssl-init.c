/* Copyright (C) 2002-2006 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "env-util.h"
#include "file-copy.h"
#include "log.h"
#include "ssl-init.h"

#ifdef HAVE_SSL

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static struct timeout *to;
static char *generating_path = NULL;

#define SSL_PARAMETERS_PERM_PATH PKG_STATEDIR"/"SSL_PARAMETERS_FILENAME

static void start_generate_process(const char *fname)
{
	const char *binpath = PKG_LIBEXECDIR"/ssl-build-param";
	struct log_io *log;
	pid_t pid;
	int log_fd;

	log_fd = log_create_pipe(&log, 10);
	if (log_fd == -1)
		pid = -1;
	else {
		pid = fork();
		if (pid < 0)
			i_error("fork() failed: %m");
	}
	if (pid == -1) {
		(void)close(log_fd);
		return;
	}

	log_set_prefix(log, "ssl-build-param: ");
	if (pid != 0) {
		/* parent */
		i_assert(generating_path == NULL);
		generating_path = i_strdup(fname);
		PID_ADD_PROCESS_TYPE(pid, PROCESS_TYPE_SSL_PARAM);
		(void)close(log_fd);
		return;
	}

	/* child. */
	if (dup2(log_fd, 2) < 0)
		i_fatal("dup2(stderr) failed: %m");

	child_process_init_env();
	client_process_exec(t_strconcat(binpath, " "SSL_PARAMETERS_PERM_PATH,
					NULL), "");
	i_fatal_status(FATAL_EXEC, "execv(%s) failed: %m", binpath);
}

void ssl_parameter_process_destroyed(pid_t pid __attr_unused__)
{
	if (file_copy(SSL_PARAMETERS_PERM_PATH, generating_path, TRUE) <= 0) {
		i_error("file_copy(%s, %s) failed: %m",
			SSL_PARAMETERS_PERM_PATH, generating_path);
	}
	i_free_and_null(generating_path);
}

static bool check_parameters_file_set(struct settings *set)
{
	const char *path;
	struct stat st;
	time_t regen_time;

	if (set->ssl_disable)
		return TRUE;

	path = t_strconcat(set->login_dir, "/"SSL_PARAMETERS_FILENAME, NULL);
	if (lstat(path, &st) < 0) {
		if (errno != ENOENT) {
			i_error("lstat() failed for SSL parameters file %s: %m",
				path);
			return TRUE;
		}

		/* try to copy the permanent parameters file here if possible */
		if (file_copy(SSL_PARAMETERS_PERM_PATH, path, TRUE) > 0) {
			if (stat(path, &st) < 0) {
				i_error("stat(%s) failed: %m", path);
				st.st_mtime = 0;
			}
		} else {
			st.st_mtime = 0;
		}
	} else if (st.st_size == 0) {
		/* broken, delete it (mostly for backwards compatibility) */
		st.st_mtime = 0;
		(void)unlink(path);
	}

	/* make sure it's new enough, it's not 0 sized, and the permissions
	   are correct */
	regen_time = set->ssl_parameters_regenerate == 0 ? ioloop_time :
		(st.st_mtime + (time_t)(set->ssl_parameters_regenerate*3600));
	if (regen_time < ioloop_time || st.st_size == 0 ||
	    st.st_uid != master_uid) {
		if (st.st_mtime == 0) {
			i_info("Generating Diffie-Hellman parameters "
			       "for the first time. This may take "
			       "a while..");
		}
		start_generate_process(path);
		return FALSE;
	}

	return TRUE;
}

void ssl_check_parameters_file(void)
{
	struct server_settings *server;

	if (generating_path != NULL)
		return;

	for (server = settings_root; server != NULL; server = server->next) {
		if (server->defaults != NULL &&
		    !check_parameters_file_set(server->defaults))
			break;
	}
}

static void check_parameters_file_timeout(void *context __attr_unused__)
{
	ssl_check_parameters_file();
}

void ssl_init(void)
{
	generating_path = NULL;

	/* check every 10 mins */
	to = timeout_add(600 * 1000, check_parameters_file_timeout, NULL);

        ssl_check_parameters_file();
}

void ssl_deinit(void)
{
	timeout_remove(&to);
}

#else

void ssl_parameter_process_destroyed(pid_t pid __attr_unused__) {}
void ssl_check_parameters_file(void) {}
void ssl_init(void) {}
void ssl_deinit(void) {}

#endif
