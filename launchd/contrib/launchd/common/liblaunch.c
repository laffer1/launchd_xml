/*
 * Copyright 2006 Infoweapons Corporation
 */

/*
 * $FreeBSD$
 *
 * Copyright (c) 2005 R. Tyler Ballance <tyler@tamu.edu> All rights reserved.
 *
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
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

/* for XML configuration file parser */
#include <bsdxml.h> /* equivalent to expat.h */

#include "launch.h"
#include "launch_priv.h"

/* stack utility used by XML parser */
#include "launch_pliststack.h"

/*! 
 * _launch_data is typedef'd to launch_data_t which is the versitile data
 * structure behind the entire application, allowing for a linked list, a
 * string, array, etc
 */
struct _launch_data {
	launch_data_type_t type;
	union {
		struct {
			launch_data_t *_array;
			size_t _array_cnt;
		};
		struct {
			char *string;
			size_t string_len;
		};
		struct {
			void *opaque;
			size_t opaque_size;
		};
		int fd;
		int err;
		long long number;
		bool boolean;
		double float_num;
	};
};

struct _launch {
	void	*sendbuf;
	int	*sendfds;   
	void	*recvbuf;
	int	*recvfds;   
	size_t	sendlen;                
	size_t	sendfdcnt;                    
	size_t	recvlen;                                
	size_t	recvfdcnt;                                    
	int	fd;                                                     
};

static void make_msg_and_cmsg(launch_data_t, void **, size_t *, int **, 
			      size_t *);
static launch_data_t make_data(launch_t, size_t *, size_t *);
static launch_data_t launch_data_array_pop_first(launch_data_t where);

static pthread_once_t _lc_once = PTHREAD_ONCE_INIT;

static struct _launch_client {
	pthread_mutex_t mtx;
	launch_t	l;
	launch_data_t	async_resp;
} *_lc = NULL;

/*
 * Initialization routine of launchd client(launchctl).
 */
static void launch_client_init(void)
{
	struct sockaddr_un sun;
	char *where = getenv(LAUNCHD_SOCKET_ENV);
	char *_launchd_fd = getenv(LAUNCHD_TRUSTED_FD_ENV);
	int dfd, lfd = -1;
	
	_lc = calloc(1, sizeof(struct _launch_client));
	
	if (!_lc)
		return;

	pthread_mutex_init(&_lc->mtx, NULL);

	if (_launchd_fd) {
		lfd = strtol(_launchd_fd, NULL, 10);
		if ((dfd = dup(lfd)) >= 0) {
			close(dfd);
			_fd(lfd);
		} else {
			lfd = -1;
		}
		unsetenv(LAUNCHD_TRUSTED_FD_ENV);
	}
	if (lfd == -1) {
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		
		if (where) {
			strncpy(sun.sun_path, where, 
				sizeof(sun.sun_path));
		} else if (getuid() == 0) {
			strncpy(sun.sun_path, 
				LAUNCHD_SOCK_PREFIX "/sock", 
				sizeof(sun.sun_path));
		} else {
			goto out_bad;
		}

		if ((lfd = _fd(socket(AF_UNIX, SOCK_STREAM, 0))) == -1)
			goto out_bad;
		if (-1 == connect(lfd, (struct sockaddr *)&sun, 
				  sizeof(sun)))
			goto out_bad;
	}
	if (!(_lc->l = launchd_fdopen(lfd)))
		goto out_bad;
	if (!(_lc->async_resp = launch_data_alloc(LAUNCH_DATA_ARRAY)))
		goto out_bad;

	return;
out_bad:
	fprintf(stderr, 
		"Entered out_bad withing launch_client_init()\n");

	if (_lc->l)
		launchd_close(_lc->l);
	else if (lfd != -1)
		close(lfd);
	if (_lc)
		free(_lc);
	_lc = NULL;
}

/*
 * Allocates memory for a specified launch data type.
 */
launch_data_t launch_data_alloc(launch_data_type_t t) {
	launch_data_t d = calloc(1, sizeof(struct _launch));

	if (d) {
		d->type = t;
		switch (t) {
		case LAUNCH_DATA_PROPERTY:
		case LAUNCH_DATA_DICTIONARY:
		case LAUNCH_DATA_ARRAY:
			d->_array = malloc(0);
			/* jmp - need to set count */
			d->_array_cnt = 0;
			break;
		default:
			break;
		}
	}

	return d;
}

/*
 * Returns the launch data type.
 */
launch_data_type_t launch_data_get_type(launch_data_t d)
{ 
	return d->type; 
}

/*
 * Frees launch data memory.
 */
void launch_data_free(launch_data_t d)
{
	size_t i;

	switch (d->type) {
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < d->_array_cnt; i++)
			launch_data_free(d->_array[i]);
		free(d->_array);
		break;
	case LAUNCH_DATA_STRING:
		if (d->string)
			free(d->string);
		break;
	case LAUNCH_DATA_OPAQUE:
		if (d->opaque)
			free(d->opaque);
		break;
	default:
		break;
	}
	free(d);
}

/*
 * Gets count of dictionary entries.
 */
size_t launch_data_dict_get_count(launch_data_t dict) 
{ 
	return (dict->_array_cnt / 2); 
}

/*
 * Inserts new dictionary entry.
 */
bool launch_data_dict_insert(launch_data_t dict, launch_data_t what, 
			     const char *key)
{
	size_t i;
	launch_data_t thekey = launch_data_alloc(LAUNCH_DATA_STRING);

	launch_data_set_string(thekey, key);

	for (i = 0; i < dict->_array_cnt; i += 2) {
		if (!strcasecmp(key, dict->_array[i]->string)) {
			launch_data_array_set_index(dict, thekey, i);
			launch_data_array_set_index(dict, what, i + 1);
			return true;
		}
	}
	launch_data_array_set_index(dict, thekey, i);
	launch_data_array_set_index(dict, what, i + 1);
	return (true);
}

/*
 * Looks up value from dictionary with specified key.
 */
launch_data_t launch_data_dict_lookup(launch_data_t dict, 
				      const char *key)
{
	size_t i;

	if (LAUNCH_DATA_DICTIONARY != dict->type)
		return NULL;

	for (i = 0; i < dict->_array_cnt; i += 2) {
		if (!strcasecmp(key, dict->_array[i]->string))
			return dict->_array[i + 1];
	}

	return (NULL);
}

/*
 * Removes dictionary entry with specified key.
 */
bool launch_data_dict_remove(launch_data_t dict, const char *key)
{
	size_t i;

	for (i = 0; i < dict->_array_cnt; i += 2) {
		if (!strcasecmp(key, dict->_array[i]->string))
			break;
	}
	if (i == dict->_array_cnt)
		return false;
	launch_data_free(dict->_array[i]);
	launch_data_free(dict->_array[i + 1]);
	memmove(dict->_array + i, dict->_array + i + 2, 
		(dict->_array_cnt - (i + 2)) * sizeof(launch_data_t));
	dict->_array_cnt -= 2;
	return (true);
}

/*
 * Iterator function of dictionary with supplied routine.
 */
void launch_data_dict_iterate(launch_data_t dict, 
			      void (*cb)(launch_data_t, const char *, 
					 void *), void *context)
{
	size_t i;

	if (LAUNCH_DATA_DICTIONARY != dict->type)
		return;

	for (i = 0; i < dict->_array_cnt; i += 2)
		cb(dict->_array[i + 1], dict->_array[i]->string, 
		   context);
	return;
}

/*
 * Dumps dictionary data.
 */
void launch_data_dict_dump(launch_data_t dict)
{
	size_t i;

	if (LAUNCH_DATA_DICTIONARY != dict->type)
		return;

	for (i = 0; i < dict->_array_cnt; i += 2) {
		fprintf(stdout, "\n%s: ", dict->_array[i]->string);
		launch_data_dump(dict->_array[i+1]);
	}
	fprintf(stdout, "\n");
	return;
}

/*
 * Sets index of an array entry with specified size.
 */
bool launch_data_array_set_index(launch_data_t where, launch_data_t what,
				 size_t ind)
{
	if ((ind + 1) >= where->_array_cnt) {
		where->_array = realloc(where->_array, 
					(ind + 1) 
					* sizeof(launch_data_t));
		memset(where->_array + where->_array_cnt, 0, 
		       (ind + 1 - where->_array_cnt) 
		       * sizeof(launch_data_t));
		where->_array_cnt = ind + 1;
	}

	if (where->_array[ind])
		launch_data_free(where->_array[ind]);
	where->_array[ind] = what;
	return true;
}

/*
 * Gets index of an array entry with specified size.
 */
launch_data_t launch_data_array_get_index(launch_data_t where, 
					  size_t ind)
{
	if (LAUNCH_DATA_ARRAY != where->type)
		return NULL;
	if (ind < where->_array_cnt)
		return where->_array[ind];
	return NULL;
}

/*
 * Pops first array entry from 'where'.
 */
launch_data_t launch_data_array_pop_first(launch_data_t where)
{
	launch_data_t r = NULL;
       
	if (where->_array_cnt > 0) {
		r = where->_array[0];
		memmove(where->_array, where->_array + 1,
			(where->_array_cnt - 1) * sizeof(launch_data_t));
		where->_array_cnt--;
	}
	return r;
}

/*
 * Gets count of entries in array 'where'.
 */
size_t launch_data_array_get_count(launch_data_t where)
{
	if ((LAUNCH_DATA_ARRAY != where->type) 
	    && (where->type != LAUNCH_DATA_DICTIONARY))
		return 0;
	return where->_array_cnt;
}

/*
 * Dumps array data.
 */
void launch_data_array_dump(launch_data_t array)
{
	size_t i;

	if (LAUNCH_DATA_ARRAY != array->type)
		return;

	for (i = 0; i < array->_array_cnt; i++) {
		fprintf(stdout, "\ndata[%d]: ", (int)i);
		launch_data_dump(array->_array[i]);
	}
	fprintf(stdout, "\n");
}

/*
 * Dumps mixed types in launch data.
 */
void launch_data_dump(launch_data_t data)
{
	switch (data->type) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_dump(data);
		break;
	case LAUNCH_DATA_ARRAY:
		launch_data_array_dump(data);
		break;
	case LAUNCH_DATA_FD:
		fprintf(stdout, "%d", data->fd);
		break;
	case LAUNCH_DATA_INTEGER:
		fprintf(stdout, "%ld", (long)data->number);
		break;
	case LAUNCH_DATA_REAL:
		fprintf(stdout, "%f", data->float_num);
		break;
	case LAUNCH_DATA_BOOL:
		fprintf(stdout, "%d", data->boolean);
		break;
	case LAUNCH_DATA_STRING:
		fprintf(stdout, "%s", data->string);
		break;
	case LAUNCH_DATA_ERRNO:
		//fprintf(stdout, "%d", data->errno);
		break;
	case LAUNCH_DATA_PROPERTY:
	case LAUNCH_DATA_OPAQUE:
	default:
		fprintf(stdout, "unknown type");
		break;
	}

	return;
}

/*
 * Sets the value of the field 'err'.
 */
bool launch_data_set_errno(launch_data_t d, int e)
{
	d->err = e;
	return true;
}

/* 
 * Sets the value of the field 'fd'.
 */
bool launch_data_set_fd(launch_data_t d, int fd)
{
	d->fd = fd;
	return true;
}

/*
 * Sets the value of the field 'number'.
 */
bool launch_data_set_integer(launch_data_t d, long long n)
{
	d->number = n;
	return true;
}

/*
 * Sets the value of the field 'boolean'.
 */
bool launch_data_set_bool(launch_data_t d, bool b)
{
	d->boolean = b;
	return true;
}

/*
 * Sets the value of the field 'float_num'.
 */
bool launch_data_set_real(launch_data_t d, double n)
{
	d->float_num = n;
	return true;
}

/*
 * Sets the value of the field 'string'.
 */
bool launch_data_set_string(launch_data_t d, const char *s)
{
	if (d->string)
		free(d->string);
	d->string = strdup(s);
	if (d->string) {
		d->string_len = strlen(d->string);
		return true;
	}
	return false;
}

/*
 * Sets the value of the field 'opaque'.
 */
bool launch_data_set_opaque(launch_data_t d, const void *o, size_t os)
{
	d->opaque_size = os;
	if (d->opaque)
		free(d->opaque);
	d->opaque = malloc(os);
	if (d->opaque) {
		memcpy(d->opaque, o, os);
		return true;
	}
	return false;
}

/*
 * Gets the value of the field 'err'.
 */
int launch_data_get_errno(launch_data_t d)
{
	return d->err;
}

/*
 * Gets the value of the field 'fd'.
 */
int launch_data_get_fd(launch_data_t d)
{
	return d->fd;
}

/*
 * Gets the value of the field 'number'.
 */
long long launch_data_get_integer(launch_data_t d)
{
	return d->number;
}

/*
 * Gets the value of the field 'boolean'.
 */
bool launch_data_get_bool(launch_data_t d)
{
	return d->boolean;
}

/*
 * Gets the value of the field 'float_num'.
 */
double launch_data_get_real(launch_data_t d)
{
	return d->float_num;
}

/*
 * Gets the value of the field 'string'.
 */
const char *launch_data_get_string(launch_data_t d)
{
	if (LAUNCH_DATA_STRING != d->type)
		return NULL;
	return d->string;
}

/*
 * Gets the value of the field 'opaque'.
 */
void *launch_data_get_opaque(launch_data_t d)
{
	if (LAUNCH_DATA_OPAQUE != d->type)
		return NULL;
	return d->opaque;
}

/*
 * Gets the value of the field 'opaque_size'.
 */
size_t launch_data_get_opaque_size(launch_data_t d)
{
	return d->opaque_size;
}

/*
 * Gets the value of the field 'fd'.
 */
int launchd_getfd(launch_t l)
{
	return l->fd;
}

/*
 * Allocates launch_t data structure for an open file descriptor.
 */
launch_t launchd_fdopen(int fd)
{
        launch_t c;

        c = calloc(1, sizeof(struct _launch));
	if (!c)
		return NULL;

        c->fd = fd;

	fcntl(fd, F_SETFL, O_NONBLOCK);

        if ((c->sendbuf = malloc(0)) == NULL)
		goto out_bad;
        if ((c->sendfds = malloc(0)) == NULL)
		goto out_bad;
        if ((c->recvbuf = malloc(0)) == NULL)
		goto out_bad;
        if ((c->recvfds = malloc(0)) == NULL)
		goto out_bad;

	return c;

out_bad:
	if (c->sendbuf)
		free(c->sendbuf);
	if (c->sendfds)
		free(c->sendfds);
	if (c->recvbuf)
		free(c->recvbuf);
	if (c->recvfds)
		free(c->recvfds);
	free(c);
	return NULL;
}

/*
 * Frees memory used for launch_t information for a file descriptor.
 */
void launchd_close(launch_t lh)
{
	if (lh->sendbuf)
		free(lh->sendbuf);
	if (lh->sendfds)
		free(lh->sendfds);
	if (lh->recvbuf)
		free(lh->recvbuf);
	if (lh->recvfds)
		free(lh->recvfds);
	close(lh->fd);
	free(lh);
}

/*
 * Creates message and control message.
 */
static void make_msg_and_cmsg(launch_data_t d, void **where, size_t *len,
			      int **fd_where, size_t *fdcnt)
{
	size_t i;

	*where = realloc(*where, *len + sizeof(struct _launch_data));
	memcpy(*where + *len, d, sizeof(struct _launch_data));
	*len += sizeof(struct _launch_data);

	switch (d->type) {
	case LAUNCH_DATA_FD:
		if (d->fd != -1) {
			*fd_where = realloc(*fd_where, (*fdcnt + 1) 
					    * sizeof(int));
			(*fd_where)[*fdcnt] = d->fd;
			(*fdcnt)++;
		}
		break;
	case LAUNCH_DATA_STRING:
		*where = realloc(*where, *len + strlen(d->string) + 1);
		memcpy(*where + *len, d->string, strlen(d->string) + 1);
		*len += strlen(d->string) + 1;
		break;
	case LAUNCH_DATA_OPAQUE:
		*where = realloc(*where, *len + d->opaque_size);
		memcpy(*where + *len, d->opaque, d->opaque_size);
		*len += d->opaque_size;
		break;
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		*where = realloc(*where, *len 
				 + (d->_array_cnt 
				    * sizeof(launch_data_t)));
		memcpy(*where + *len, d->_array, 
		       d->_array_cnt * sizeof(launch_data_t));
		*len += d->_array_cnt * sizeof(launch_data_t);

		for (i = 0; i < d->_array_cnt; i++)
			make_msg_and_cmsg(d->_array[i], where, len, 
					  fd_where, fdcnt);
		break;
	default:
		break;
	}
}

/*
 * Creates data structure from received message.
 */
static launch_data_t make_data(launch_t conn, size_t *data_offset, 
			       size_t *fdoffset)
{
	launch_data_t r = conn->recvbuf + *data_offset;
	size_t i;

	if ((conn->recvlen - *data_offset) < sizeof(struct _launch_data))
		return NULL;
	*data_offset += sizeof(struct _launch_data);

	switch (r->type) {
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		if ((conn->recvlen - *data_offset) 
		    < (r->_array_cnt * sizeof(launch_data_t))) {
			errno = EAGAIN;
			return NULL;
		}
		r->_array = conn->recvbuf + *data_offset;
		*data_offset += r->_array_cnt * sizeof(launch_data_t);
		for (i = 0; i < r->_array_cnt; i++) {
			r->_array[i] = make_data(conn, data_offset, 
						 fdoffset);
			if (r->_array[i] == NULL)
				return NULL;
		}
		break;
	case LAUNCH_DATA_STRING:
		if ((conn->recvlen - *data_offset) 
		    < (r->string_len + 1)) {
			errno = EAGAIN;
			return NULL;
		}
		r->string = conn->recvbuf + *data_offset;
		*data_offset += r->string_len + 1;
		break;
	case LAUNCH_DATA_OPAQUE:
		if ((conn->recvlen - *data_offset) < r->opaque_size) {
			errno = EAGAIN;
			return NULL;
		}
		r->opaque = conn->recvbuf + *data_offset;
		*data_offset += r->opaque_size;
		break;
	case LAUNCH_DATA_FD:
		if (r->fd != -1) {
			r->fd = _fd(conn->recvfds[*fdoffset]);
			*fdoffset += 1;
		}
		break;
	case LAUNCH_DATA_INTEGER:
	case LAUNCH_DATA_REAL:
	case LAUNCH_DATA_BOOL:
	case LAUNCH_DATA_ERRNO:
		break;
	default:
		errno = EINVAL;
		return NULL;
		break;
	}

	return r;
}

/*
 * Sends a message data. 
 */
int launchd_msg_send(launch_t lh, launch_data_t d)
{
	struct cmsghdr *cm = NULL;
	struct msghdr mh;
	struct iovec iov;
	int r;
	size_t sentctrllen = 0;

	memset(&mh, 0, sizeof(mh));

	mh.msg_iov = &iov;
        mh.msg_iovlen = 1;

	if (d)
		make_msg_and_cmsg(d, &lh->sendbuf, &lh->sendlen, 
				  &lh->sendfds, &lh->sendfdcnt);

	if (lh->sendfdcnt > 0) {
		sentctrllen = mh.msg_controllen = \
			CMSG_SPACE(lh->sendfdcnt * sizeof(int));
		cm = alloca(mh.msg_controllen);
		mh.msg_control = cm;

		memset(cm, 0, mh.msg_controllen);

		cm->cmsg_len = CMSG_LEN(lh->sendfdcnt * sizeof(int));
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;

		memcpy(CMSG_DATA(cm), lh->sendfds,
		       lh->sendfdcnt * sizeof(int));
	}

	iov.iov_base = lh->sendbuf;
	iov.iov_len = lh->sendlen;

	if ((r = sendmsg(lh->fd, &mh, 0)) == -1) {
		return -1;
	} else if (r == 0) {
		errno = ECONNRESET;
		return -1;
	} else if (sentctrllen != mh.msg_controllen) {
		errno = ECONNRESET;
		return -1;
	}

	lh->sendlen -= r;
	if (lh->sendlen > 0) {
		memmove(lh->sendbuf, lh->sendbuf + r, lh->sendlen);
	} else {
		free(lh->sendbuf);
		lh->sendbuf = malloc(0);
	}

	lh->sendfdcnt = 0;
	free(lh->sendfds);
	lh->sendfds = malloc(0);

	if (lh->sendlen > 0) {
		errno = EAGAIN;
		return -1;
	}

	return 0;
}

//! This function is not called yet by anything
/*!
 * According to zarzycki@:
 * 
 * Eventually the launch_msg() interface will be used for asynchronous 
 * callbacks, thus the need to find out when messages are pending, and 
 * that brings us back to launch_get_fd(). 
 *
 * launch_get_fd() and launch_msg() are the only two interfaces into the 
 * library, and we use pthread_once() from both to properly setup the 
 * library, no matter which is called first.
 */
int launch_get_fd(void) {
	pthread_once(&_lc_once, launch_client_init);

	if (_lc == NULL) {
		errno = ENOTCONN;
		return -1;
	}

	return _lc->l->fd;
}

/*
 * Processes received message.
 */
static void launch_msg_getmsgs(launch_data_t m, void *context)
{
	launch_data_t async_resp, *sync_resp = context;
	
	if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(m)) 
	    && (async_resp = \
		launch_data_dict_lookup(m, LAUNCHD_ASYNC_MSG_KEY))) {
		launch_data_array_set_index(_lc->async_resp, 
		    launch_data_copy(async_resp),
		    launch_data_array_get_count(_lc->async_resp));
	} else {
		*sync_resp = launch_data_copy(m);
	}
}

/*
 * Sends message.
 */
launch_data_t launch_msg(launch_data_t d)
{
	launch_data_t resp = NULL;

	pthread_once(&_lc_once, launch_client_init);
	
	if (_lc == NULL) {
		errno = ENOTCONN;
		return NULL;
	}

	pthread_mutex_lock(&_lc->mtx);

	if (d && launchd_msg_send(_lc->l, d) == -1) {
		do {
			if (errno != EAGAIN)
				goto out;
		} while (launchd_msg_send(_lc->l, NULL) == -1);
	}
       
	while (resp == NULL) {
		if ((d == NULL) 
		    && (launch_data_array_get_count(_lc->async_resp) 
			> 0)) {
			resp = launch_data_array_pop_first(_lc->async_resp);
			goto out;
		}
		if (launchd_msg_recv(_lc->l, launch_msg_getmsgs, 
				     &resp) == -1) {
			if (errno != EAGAIN) {
				goto out;
			} else if (d == NULL) {
				errno = 0;
				goto out;
			} else {
				fd_set rfds;

				FD_ZERO(&rfds);
				FD_SET(_lc->l->fd, &rfds);
			
				select(_lc->l->fd + 1, &rfds, NULL, 
				       NULL, NULL);
			}
		}
	}

out:
	pthread_mutex_unlock(&_lc->mtx);

	return resp;
}

/*
 * Processes received message.
 */
int launchd_msg_recv(launch_t lh, void (*cb)(launch_data_t, void *), 
		     void *context)
{
	struct cmsghdr *cm = alloca(4096); 
	launch_data_t rmsg;
	size_t data_offset, fd_offset;
        struct msghdr mh;
        struct iovec iov;
	int r;

        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;

	lh->recvbuf = realloc(lh->recvbuf, lh->recvlen + 8*1024);

	iov.iov_base = lh->recvbuf + lh->recvlen;
	iov.iov_len = 8*1024;
	mh.msg_control = cm;
	mh.msg_controllen = 4096;

	if ((r = recvmsg(lh->fd, &mh, 0)) == -1)
		return -1;
	if (r == 0) {
		errno = ECONNRESET;
		return -1;
	}
	if (mh.msg_flags & MSG_CTRUNC) {
		errno = ECONNABORTED;
		return -1;
	}
	lh->recvlen += r;
	if (mh.msg_controllen > 0) {
		lh->recvfds = realloc(lh->recvfds, lh->recvfdcnt 
				      * sizeof(int) 
				      + mh.msg_controllen 
				      - sizeof(struct cmsghdr));
		memcpy(lh->recvfds + lh->recvfdcnt, CMSG_DATA(cm), 
		       mh.msg_controllen - sizeof(struct cmsghdr));
		lh->recvfdcnt += (mh.msg_controllen 
				  - sizeof(struct cmsghdr))/ sizeof(int);
	}

parse_more:
	data_offset = 0;
	fd_offset = 0;

	rmsg = make_data(lh, &data_offset, &fd_offset);

	if (rmsg) {
		cb(rmsg, context);

		lh->recvlen -= data_offset;
		if (lh->recvlen > 0) {
			memmove(lh->recvbuf, lh->recvbuf + data_offset, 
				lh->recvlen);
		} else {
			free(lh->recvbuf);
			lh->recvbuf = malloc(0);
		}

		lh->recvfdcnt -= fd_offset;
		if (lh->recvfdcnt > 0) {
			memmove(lh->recvfds, lh->recvfds + fd_offset, 
				lh->recvfdcnt * sizeof(int));
		} else {
			free(lh->recvfds);
			lh->recvfds = malloc(0);
		}

		if (lh->recvlen > 0)
			goto parse_more;
		else
			r = 0;
	} else {
		errno = EAGAIN;
		r = -1;
	}

	return r;
}

/*
 * Copies launch_data_t structure.
 */
launch_data_t launch_data_copy(launch_data_t o)
{
	launch_data_t r = launch_data_alloc(o->type);
	size_t i;

	free(r->_array);
	memcpy(r, o, sizeof(struct _launch_data));

	switch (o->type) {
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		r->_array = calloc(1, o->_array_cnt * sizeof(launch_data_t));
		for (i = 0; i < o->_array_cnt; i++) {
			if (o->_array[i])
				r->_array[i] = launch_data_copy(o->_array[i]);
		}
		break;
	case LAUNCH_DATA_STRING:
		r->string = strdup(o->string);
		break;
	case LAUNCH_DATA_OPAQUE:
		r->opaque = malloc(o->opaque_size);
		memcpy(r->opaque, o->opaque, o->opaque_size);
		break;
	default:
		break;
	}

	return r;
}

/*
 * Sends a "batch enable" message to launchd daemon.
 */
void launchd_batch_enable(bool val)
{
	launch_data_t resp, tmp, msg;

	tmp = launch_data_alloc(LAUNCH_DATA_BOOL);
	launch_data_set_bool(tmp, val);

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_BATCHCONTROL);

	resp = launch_msg(msg);

	launch_data_free(msg);

	if (resp)
		launch_data_free(resp);
}

/*
 * Sends a "batch query" message to launchd daemon.
 */
bool launchd_batch_query(void)
{
	launch_data_t resp, msg = launch_data_alloc(LAUNCH_DATA_STRING);
	bool rval = true;

	launch_data_set_string(msg, LAUNCH_KEY_BATCHQUERY);

	resp = launch_msg(msg);

	launch_data_free(msg);

	if (resp) {
		if (launch_data_get_type(resp) == LAUNCH_DATA_BOOL)
			rval = launch_data_get_bool(resp);
		launch_data_free(resp);
	}
	return rval;
}

/*
 * Set close-on-exec flag of the speficied file descriptor.
 *  - on exec(), file will be closed
 */
int _fd(int fd)
{
	if (fd >= 0)
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		//fcntl(fd, F_SETFD, 1);
	return fd;
}

/*
 * Allocates new instance of launch_data_t of type errno.
 */
launch_data_t launch_data_new_errno(int e)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_ERRNO);

	if (r)
	       launch_data_set_errno(r, e);

	return r;
}

/*
 * Allocates new instance of launch_data_t of type fd(file descriptor).
 */
launch_data_t launch_data_new_fd(int fd)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_FD);

	if (r)
	       launch_data_set_fd(r, fd);

	return r;
}

/*
 * Allocates new instance of launch_data_t of type integer.
 */
launch_data_t launch_data_new_integer(long long n)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_INTEGER);

	if (r)
		launch_data_set_integer(r, n);

	return r;
}

/*
 * Allocates new instance of launch_data_t of type boolean.
 */
launch_data_t launch_data_new_bool(bool b)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_BOOL);

	if (r)
		launch_data_set_bool(r, b);

	return r;
}

/*
 * Allocates new instance of launch_data_t of type real.
 */
launch_data_t launch_data_new_real(double d)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_REAL);

	if (r)
		launch_data_set_real(r, d);

	return r;
}

/*
 * Allocates new instance of launch_data_t of type string.
 */
launch_data_t launch_data_new_string(const char *s)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_STRING);

	if (r == NULL)
		return NULL;

	if (!launch_data_set_string(r, s)) {
		launch_data_free(r);
		return NULL;
	}

	return r;
}

/*
 * Allocates new instance of launch_data_t of type opaque.
 */
launch_data_t launch_data_new_opaque(const void *o, size_t os)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_OPAQUE);

	if (r == NULL)
		return NULL;

	if (!launch_data_set_opaque(r, o, os)) {
		launch_data_free(r);
		return NULL;
	}

	return r;
}

#ifdef _XML_CONF_

#define BUFF_SIZE 1024

/*
 * Main routine for conversion of plist to launchd_data_t.
 */
launch_data_t plist_to_launchdata(int fd)
{
	STACK pliststack;
	XML_Parser parser;
	int bytes_read;
	void *buff;
	launch_data_t launchdata = NULL;

	if (fd < 0) {
		fprintf(stderr, "File pointer is invalid!");
		return (NULL);
	}

	pliststack = create_stack();
	parser = XML_ParserCreate (NULL);

	if ((pliststack == NULL) || (parser == NULL)) {
		fprintf(stderr, "Memory Error: Unable to allocate!");
		return NULL;
	}

	XML_SetElementHandler(parser, plist_start, plist_end);
	XML_SetCharacterDataHandler(parser, plist_text);
	XML_SetUserData(parser, (void *)pliststack);

	for (;;) {
		buff = XML_GetBuffer(parser, BUFF_SIZE);

		if (buff == NULL) {
			fprintf(stderr, "Memory Error: Unable to allocate!");
			return (NULL);
		}
		
		bytes_read = read(fd, buff, BUFF_SIZE);
		if (bytes_read < 0) {
			fprintf(stderr, "Read Error!");
			return (NULL);
		}

		if (! XML_ParseBuffer(parser, bytes_read, bytes_read == 0)) {
			fprintf(stderr, "Parsing Error!");
			return (NULL);
		}
	
		if (bytes_read == 0)
			break;
	}

	launchdata = top(pliststack);
	launch_data_dict_dump(launchdata);

	pop(pliststack);

	if (!is_empty(pliststack))
		fprintf(stderr, "Warning: Stack not empty!");

	dispose_stack(pliststack);
	XML_ParserFree(parser);

	return (launchdata);
}

/*
 * Handler function for opening XML tag.
 */
void plist_start(void *userdata, const char *name, const char **atts)
{
	STACK stack = (STACK)userdata;
	launch_data_t tmpdata = NULL;
	/* check plist version */

	DEBUG_PRINT(name);

	if (!strcmp(name, "dict")) {
		tmpdata = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		push(tmpdata, stack);
	} else if (!strcmp(name, "array")) {
		tmpdata = launch_data_alloc(LAUNCH_DATA_ARRAY);
		push(tmpdata, stack);
	} else if (!strcmp(name, "key") || !strcmp(name, "string")) {
		tmpdata = launch_data_alloc(LAUNCH_DATA_STRING);
		push(tmpdata, stack);
	} else if (!strcmp(name, "true") || !strcmp(name, "false")) {
		tmpdata = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(tmpdata, !strcmp(name, "true"));
		push(tmpdata, stack);
	} else if (!strcmp(name, "integer")) {
		tmpdata = launch_data_alloc(LAUNCH_DATA_INTEGER);
		push(tmpdata, stack);
	} else if (!strcmp(name, "plist")) {
		/* check the version of plist here */
	} else {
		fprintf(stderr, "Unknown tag type!");
	}

	return;
}

/* 
 * Handler function for closing XML tag.
 */
void plist_end(void *userdata, const char *el)
{
	STACK stack = (STACK)userdata;
	launch_data_t tmpdata = NULL;
	launch_data_t upperdata = NULL;
	int count = 0;

	if (is_empty(stack)) {
		fprintf(stderr, "Stack empty");
		return;
	}

	tmpdata = top(stack);
	if (tmpdata == NULL) {
		fprintf(stderr, "Unable to get top entry!");
		return;
	}

	/* remove the top element */
	pop(stack);

	upperdata = (launch_data_t)top(stack);
	if (upperdata == NULL) {
		DEBUG_PRINT("found uppermost tag");
		/* last element will be re-inserted */
		push(tmpdata, stack);
	} else {
		/* to-do: needs to check for key-value pair for dictionary */
		count = launch_data_array_get_count(upperdata);
		launch_data_array_set_index(upperdata, tmpdata, count);
	}

	return;
}

/*
 * Handler function for text portion.
 */
void plist_text(void *userdata, const XML_Char *str, int len)
{
	STACK stack = (STACK)userdata;
	launch_data_t tmpdata = NULL;
	char *tmpstr;

        /* need to ignore whitespace */
	if (isspace(str[0]))
		return;

	/* for low processing overhead, minimal check */
	if (!is_empty(stack))
		tmpdata = top(stack);
	if (tmpdata == NULL)
		return;

	tmpstr = (char *)malloc(len+1);
	/* null-terminate the data */
	strncpy(tmpstr, str, len);
	tmpstr[len] = '\0';

	DEBUG_PRINT(tmpstr);

	if (tmpdata->type == LAUNCH_DATA_STRING) {
		launch_data_set_string(tmpdata, tmpstr);
	} else if (tmpdata->type == LAUNCH_DATA_INTEGER) {
		launch_data_set_integer(tmpdata, 
					strtol(tmpstr, (char **)NULL, 
					       10));
	} else {
		fprintf(stderr, "text is not expected here!");
	}

	return;
}
#endif /* _XML_CONF_ */
