/*
 * Copyright 2006 Infoweapons Corporation
 */

/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/fcntl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <paths.h>
#include <string.h>

#include "launch.h"
#include "launch_priv.h"
#include "launchd.h"
#include "launchd_core_logic.h"
#include "launchd_unix_ipc.h"

extern char **environ;

static SLIST_HEAD(, conncb) connections = { NULL };

static launch_data_t adjust_rlimits(launch_data_t in);

static void ipc_listen_callback(void *obj __attribute__((unused)),
				struct kevent *kev);

static kq_callback kqipc_listen_callback = ipc_listen_callback;

static pid_t ipc_self = 0;

static char *sockpath = NULL;
static char *sockdir = NULL;

static bool ipc_inited = false;

void
ipc_clean_up(void)
{
	if (ipc_self != getpid())
		return;

	if (-1 == unlink(sockpath))
		syslog(LOG_WARNING, "unlink(\"%s\"): %m", sockpath);
	else if (-1 == rmdir(sockdir))
		syslog(LOG_WARNING, "rmdir(\"%s\"): %m", sockdir);
}

void
ipc_server_init(void)
{
	struct sockaddr_un sun;
	mode_t oldmask;
	int r, fd = -1;
	char ourdir[1024];

	if (ipc_inited)
		return;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;

	if (getpid() == 1) {
		strcpy(ourdir, LAUNCHD_SOCK_PREFIX);
		strncpy(sun.sun_path, LAUNCHD_SOCK_PREFIX "/sock",
			sizeof(sun.sun_path));

		unlink(ourdir);
		if (mkdir(ourdir, S_IRWXU) == -1) {
			if (errno == EROFS) {
				goto out_bad;
			} else if (errno == EEXIST) {
				struct stat sb;
				stat(ourdir, &sb);
				if (!S_ISDIR(sb.st_mode)) {
					errno = EEXIST;
					syslog(LOG_ERR, 
					       "mkdir(\"%s\"): %m",
					       LAUNCHD_SOCK_PREFIX);
					goto out_bad;
				}
			} else {
				syslog(LOG_ERR, "mkdir(\"%s\"): %m",
				       ourdir);
				goto out_bad;
			}
		}
	} else {
		snprintf(ourdir, sizeof(ourdir),
			 "/tmp/launchd-%u.XXXXXX",
			 getpid());
		mkdtemp(ourdir);
		snprintf(sun.sun_path, sizeof(sun.sun_path),
			 "%s/sock", ourdir);
		setenv(LAUNCHD_SOCKET_ENV, sun.sun_path, 1);
	}

	if (unlink(sun.sun_path) == -1 && errno != ENOENT) {
		if (errno != EROFS)
			syslog(LOG_ERR, "unlink(\"thesocket\"): %m");
		goto out_bad;
	}
	if ((fd = _fd(socket(AF_UNIX, SOCK_STREAM, 0))) == -1) {
		syslog(LOG_ERR, "socket(\"thesocket\"): %m");
		goto out_bad;
	}

	oldmask = umask(S_IRWXG|S_IRWXO);
	r = bind(fd, (struct sockaddr *)&sun, sizeof(sun));
	umask(oldmask);

	if (r == -1) {
		if (errno != EROFS)
			syslog(LOG_ERR, "bind(\"thesocket\"): %m");
		goto out_bad;
	}

	if (listen(fd, SOMAXCONN) == -1) {
		syslog(LOG_ERR, "listen(\"thesocket\"): %m");
		goto out_bad;
	}

	if (kevent_mod(fd, EVFILT_READ, EV_ADD, 0, 0, 
		       &kqipc_listen_callback) == -1) {
		syslog(LOG_ERR, 
		       "kevent_mod(\"thesocket\", EVFILT_READ): %m");
		goto out_bad;
	}

	ipc_inited = true;

	sockdir = strdup(ourdir);
	sockpath = strdup(sun.sun_path);

	ipc_self = getpid();
	atexit(ipc_clean_up);

out_bad:
	if (!ipc_inited && fd != -1)
		launchd_assumes(close(fd) == 0);
}

void
ipc_open(int fd, struct jobcb *j)
{
	struct conncb *c = calloc(1, sizeof(struct conncb));

	fcntl(fd, F_SETFL, O_NONBLOCK);

	c->kqconn_callback = ipc_callback;
	c->conn = launchd_fdopen(fd);
	c->j = j;
	SLIST_INSERT_HEAD(&connections, c, sle);
	kevent_mod(fd, EVFILT_READ, EV_ADD, 0, 0, &c->kqconn_callback);
}

void
ipc_listen_callback(void *obj __attribute__((unused)),
		    struct kevent *kev)
{
	struct sockaddr_un sun;
	socklen_t sl = sizeof(sun);
	int cfd;

	if ((cfd = _fd(accept(kev->ident, (struct sockaddr *)&sun, 
			      &sl))) == -1) {
		return;
	}

	ipc_open(cfd, NULL);
}

void
ipc_callback(void *obj, struct kevent *kev)
{
	struct conncb *c = obj;
	int r;
	
	if (kev->filter == EVFILT_READ) {
		if (launchd_msg_recv(c->conn, ipc_readmsg, c) == -1
		    && errno != EAGAIN) {
			if (errno != ECONNRESET)
				syslog(LOG_DEBUG, "%s(): recv: %m", 
				       __func__);
			ipc_close(c);
		}
	} else if (kev->filter == EVFILT_WRITE) {
		r = launchd_msg_send(c->conn, NULL);
		if (r == -1) {
			if (errno != EAGAIN) {
				syslog(LOG_DEBUG, "%s(): send: %m",
				       __func__);
				ipc_close(c);
			}
		} else if (r == 0) {
			kevent_mod(launchd_getfd(c->conn), EVFILT_WRITE,
				   EV_DELETE, 0, 0, NULL);
		}
	} else {
		syslog(LOG_DEBUG, "%s(): unknown filter type!",
		       __func__);
		ipc_close(c);
	}
}

static void set_user_env(launch_data_t obj, const char *key,
			 void *context __attribute__((unused)))
{
	setenv(key, launch_data_get_string(obj), 1);
}

void
ipc_close_fds(launch_data_t o)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o,
					 (void (*)(launch_data_t,
						   const char *,
						   void *))ipc_close_fds,
					 NULL);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			ipc_close_fds(launch_data_array_get_index(o, i));
		break;
	case LAUNCH_DATA_FD:
		if (launch_data_get_fd(o) != -1)
			launchd_assumes(close(launch_data_get_fd(o)) == 0);
		break;
	default:
		break;
	}
}

void
ipc_revoke_fds(launch_data_t o)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o,
					 (void (*)(launch_data_t,
						  const char *,
						  void *))ipc_revoke_fds,
					 NULL);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			ipc_revoke_fds(launch_data_array_get_index(o,i));
		break;
	case LAUNCH_DATA_FD:
		launch_data_set_fd(o, -1);
		break;
	default:
		break;
	}
}

struct readmsg_context {
	struct conncb *c;
	launch_data_t resp;
};

void
ipc_readmsg(launch_data_t msg, void *context)
{
	struct readmsg_context rmc = { context, NULL };

	if (LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) {
		launch_data_dict_iterate(msg, ipc_readmsg2, &rmc);
	} else if (LAUNCH_DATA_STRING == launch_data_get_type(msg)) {
		ipc_readmsg2(NULL, launch_data_get_string(msg), &rmc);
	} else {
		rmc.resp = launch_data_new_errno(EINVAL);
	}

	if (NULL == rmc.resp)
		rmc.resp = launch_data_new_errno(ENOSYS);

	ipc_close_fds(msg);

	if (launchd_msg_send(rmc.c->conn, rmc.resp) == -1) {
		if (errno == EAGAIN) {
			kevent_mod(launchd_getfd(rmc.c->conn),
				   EVFILT_WRITE, EV_ADD, 0, 0,
				   &rmc.c->kqconn_callback);
		} else {
			syslog(LOG_DEBUG,"launchd_msg_send() == -1: %m");
			ipc_close(rmc.c);
		}
	}
	launch_data_free(rmc.resp);
}


void
ipc_readmsg2(launch_data_t data, const char *cmd, void *context)
{
	struct readmsg_context *rmc = context;
	launch_data_t resp = NULL;
	struct jobcb *j;

	if (rmc->resp)
		return;

	if (!strcmp(cmd, LAUNCH_KEY_STARTJOB)) {
		if ((j = job_find(launch_data_get_string(data))) != NULL) {
			job_start(j);
			errno = 0;
		}
		resp = launch_data_new_errno(errno);
	} else if (!strcmp(cmd, LAUNCH_KEY_STOPJOB)) {
		if ((j = job_find(launch_data_get_string(data))) != NULL) {
			job_stop(j);
			errno = 0;
		}
		resp = launch_data_new_errno(errno);
	} else if (!strcmp(cmd, LAUNCH_KEY_REMOVEJOB)) {
		if ((j = job_find(launch_data_get_string(data))) != NULL) {
			job_remove(j);
			errno = 0;
		}
		resp = launch_data_new_errno(errno);
	} else if (!strcmp(cmd, LAUNCH_KEY_SUBMITJOB)) {
		if (launch_data_get_type(data) == LAUNCH_DATA_ARRAY) {
			size_t i;

			resp = launch_data_alloc(LAUNCH_DATA_ARRAY);
			for (i = 0; i < launch_data_array_get_count(data); i++) {
				if (job_import(launch_data_array_get_index(data, i)))
					errno = 0;
				launch_data_array_set_index(resp, launch_data_new_errno(errno), i);
			}
		} else {
			if (job_import(data))
				errno = 0;
			resp = launch_data_new_errno(errno);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_UNSETUSERENVIRONMENT)) {
		unsetenv(launch_data_get_string(data));
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETUSERENVIRONMENT)) {
		char **tmpenviron = environ;
		resp = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		for (; *tmpenviron; tmpenviron++) {
			char envkey[1024];
			launch_data_t s = launch_data_alloc(LAUNCH_DATA_STRING);
			launch_data_set_string(s, strchr(*tmpenviron, '=') + 1);
			strncpy(envkey, *tmpenviron, sizeof(envkey));
			*(strchr(envkey, '=')) = '\0';
			launch_data_dict_insert(resp, s, envkey);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_SETUSERENVIRONMENT)) {
		launch_data_dict_iterate(data, set_user_env, NULL);
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_CHECKIN)) {
		if (rmc->c->j) {
			resp = job_export(rmc->c->j);
			job_checkin(rmc->c->j);
		} else {
			resp = launch_data_new_errno(EACCES);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_RELOADTTYS)) {
		//clean_ttys();
		update_ttys();
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_SHUTDOWN)) {
		launchd_shutdown();
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_SINGLEUSER)) {
		launchd_single_user();
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETJOBS)) {
		resp = job_export_all();
		ipc_revoke_fds(resp);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETRESOURCELIMITS)) {
		resp = adjust_rlimits(NULL);
	} else if (!strcmp(cmd, LAUNCH_KEY_SETRESOURCELIMITS)) {
		resp = adjust_rlimits(data);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETJOB)) {
		if ((j = job_find(launch_data_get_string(data))) == NULL) {
			resp = launch_data_new_errno(errno);
		} else {
			resp = job_export(j);
			ipc_revoke_fds(resp);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_GETJOBWITHHANDLES)) {
		if ((j = job_find(launch_data_get_string(data))) == NULL) {
			resp = launch_data_new_errno(errno);
		} else {
			resp = job_export(j);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_SETLOGMASK)) {
		resp = launch_data_new_integer(setlogmask(launch_data_get_integer(data)));
	} else if (!strcmp(cmd, LAUNCH_KEY_GETLOGMASK)) {
		int oldmask = setlogmask(LOG_UPTO(LOG_DEBUG));
		resp = launch_data_new_integer(oldmask);
		setlogmask(oldmask);
	} else if (!strcmp(cmd, LAUNCH_KEY_SETUMASK)) {
		resp = launch_data_new_integer(umask(launch_data_get_integer(data)));
	} else if (!strcmp(cmd, LAUNCH_KEY_GETUMASK)) {
		mode_t oldmask = umask(0);
		resp = launch_data_new_integer(oldmask);
		umask(oldmask);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETRUSAGESELF)) {
		struct rusage rusage;
		getrusage(RUSAGE_SELF, &rusage);
		resp = launch_data_new_opaque(&rusage, sizeof(rusage));
	} else if (!strcmp(cmd, LAUNCH_KEY_GETRUSAGECHILDREN)) {
		struct rusage rusage;
		getrusage(RUSAGE_CHILDREN, &rusage);
		resp = launch_data_new_opaque(&rusage, sizeof(rusage));
	} else if (!strcmp(cmd, LAUNCH_KEY_SETSTDOUT)) {
		resp = launchd_setstdio(STDOUT_FILENO, data);
	} else if (!strcmp(cmd, LAUNCH_KEY_SETSTDERR)) {
		resp = launchd_setstdio(STDERR_FILENO, data);
	} else if (!strcmp(cmd, LAUNCH_KEY_BATCHCONTROL)) {
		batch_job_enable(launch_data_get_bool(data), rmc->c);
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_BATCHQUERY)) {
		resp = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(resp, batch_disabler_count == 0);
	}

	rmc->resp = resp;
}

void
ipc_close(struct conncb *c)
{
	batch_job_enable(true, c);

	SLIST_REMOVE(&connections, c, conncb, sle);
	launchd_close(c->conn);
	free(c);
}

launch_data_t
adjust_rlimits(launch_data_t in)
{
	static struct rlimit *l = NULL;
	static size_t lsz = sizeof(struct rlimit) * RLIM_NLIMITS;
	struct rlimit *ltmp;
	size_t i,ltmpsz;

	if (l == NULL) {
		l = malloc(lsz);
		for (i = 0; i < RLIM_NLIMITS; i++)
			launchd_assumes(getrlimit(i, l + i) != -1);
	}

	if (in) {
		ltmp = launch_data_get_opaque(in);
		ltmpsz = launch_data_get_opaque_size(in);

		if (ltmpsz > lsz) {
			syslog(LOG_WARNING, "Too much rlimit data sent!");
			ltmpsz = lsz;
		}
		
		for (i = 0; i < (ltmpsz / sizeof(struct rlimit)); i++) {
			if (ltmp[i].rlim_cur == l[i].rlim_cur 
			    && ltmp[i].rlim_max == l[i].rlim_max)
				continue;

			if (/* XXX readcfg_pid && */ getpid() == 1) {
				int gmib[] = { CTL_KERN, KERN_MAXPROC };
				int pmib[] = { CTL_KERN,
					       KERN_MAXPROCPERUID };
				const char *gstr = "kern.maxproc";
				const char *pstr = "kern.maxprocperuid";
				int gval = ltmp[i].rlim_max;
				int pval = ltmp[i].rlim_cur;
				switch (i) {
				case RLIMIT_NOFILE:
					gmib[1] = KERN_MAXFILES;
					pmib[1] = KERN_MAXFILESPERPROC;
					gstr = "kern.maxfiles";
					pstr = "kern.maxfilesperproc";
					break;
				case RLIMIT_NPROC:
					/* kernel will not clamp to this value, we must */
					if (gval > (2048 + 20))
						gval = 2048 + 20;
					break;
				default:
					break;
				}

				if (gval > 0) {
					launchd_assumes(
						sysctl(gmib, 2, NULL,
						       NULL, &gval,
						       sizeof(gval))
						!= -1);
				} else {
					syslog(LOG_WARNING, "sysctl(\"%s\"): can't be zero", gstr);
				}
				if (pval > 0) {
					launchd_assumes(
						sysctl(pmib, 2, NULL,
						       NULL, &pval,
						       sizeof(pval))
						!= -1);
				} else {
					syslog(LOG_WARNING, "sysctl(\"%s\"): can't be zero", pstr);
				}
			}
			launchd_assumes(setrlimit(i, ltmp + i) != -1);
			/* the kernel may have clamped the values we gave it */
			launchd_assumes(getrlimit(i, l + i) != -1);
		}
	}

	return launch_data_new_opaque(l, sizeof(struct rlimit) * RLIM_NLIMITS);
}
