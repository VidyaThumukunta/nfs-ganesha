// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright IBM Corporation, 2012
 *  Contributor: Frank Filz <ffilzlnx@mindspring.com>
 *
 *
 * This software is a server that implements the NFS protocol.
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *
 */

#include "multilock.h"
#include "assert.h"

struct command_def commands[NUM_COMMANDS + 1] = {
	{ "OPEN", 4 },	  { "CLOSE", 5 },   { "LOCKW", 5 }, { "LOCK", 4 },
	{ "UNLOCK", 6 },  { "TEST", 4 },    { "LIST", 4 },  { "HOP", 3 },
	{ "UNHOP", 5 },	  { "SEEK", 4 },    { "READ", 4 },  { "WRITE", 5 },
	{ "COMMENT", 7 }, { "ALARM", 5 },   { "HELLO", 5 }, { "FORK", 4 },
	{ "QUIT", 4 },	  { "UNKNOWN", 0 },
};

char errdetail[MAXSTR * 2 + 1];
char badtoken[MAXSTR + 1];
struct client *client_list;
FILE *input;
FILE *output;
bool script;
bool quiet;
bool duperrors;
bool strict;
bool error_is_fatal;
long global_tag;
long saved_tags[26];
bool syntax;
long lno;

long get_global_tag(bool increment)
{
	if (script && increment)
		global_tag = lno;
	else if (increment)
		global_tag++;

	return global_tag;
}

struct token on_off[] = { { "on", 2, true },
			  { "off", 3, false },
			  { "", 0, true } };

struct token lock_types[] = { { "read", 4, F_RDLCK },
			      { "write", 5, F_WRLCK },
			      { "shared", 6, F_RDLCK },
			      { "exclusive", 9, F_WRLCK },
			      { "F_RDLCK", 7, F_RDLCK },
			      { "F_WRLCK", 7, F_WRLCK },
			      { "unlock", 6, F_UNLCK },
			      { "F_UNLCK", 7, F_UNLCK },
			      { "*", 1, -1 },
			      { "", 0, 0 } };

struct token read_write_flags[] = { { "rw", 2, O_RDWR },
				    { "ro", 2, O_RDONLY },
				    { "wo", 2, O_WRONLY },
				    { "O_RDWR", 6, O_RDWR },
				    { "O_RDONLY", 8, O_RDONLY },
				    { "O_WRONLY", 8, O_WRONLY },
				    { "", 0, 0 } };

const char *str_read_write_flags(int flags)
{
	switch (flags & O_ACCMODE) {
	case O_RDWR:
		return "rw";
	case O_RDONLY:
		return "ro";
	case O_WRONLY:
		return "wo";
	}

	return "unknown";
}

struct token open_flags[] = {
	{ "create", 6, O_CREAT },   { "creat", 5, O_CREAT },
	{ "O_CREAT", 7, O_CREAT },  { "exclusive", 9, O_EXCL },
	{ "excl", 4, O_EXCL },	    { "O_EXCL", 6, O_EXCL },
	{ "truncate", 8, O_TRUNC }, { "trunc", 5, O_TRUNC },
	{ "O_TRUNC", 7, O_TRUNC },  { "", 0, 0 }
};

void sprintf_open_flags(char **rest, int *left, int flags)
{
	int i;
	int ex_flags = 0;

	for (i = 0; open_flags[i].t_len != 0; i++) {
		if ((ex_flags & open_flags[i].t_value) == 0 &&
		    (flags & open_flags[i].t_value) == open_flags[i].t_value) {
			/* Append open flag to line */
			sprint_left(*rest, *left, " %s", open_flags[i].t_name);
		}

		ex_flags |= open_flags[i].t_value;
	}
}

struct token lock_modes[] = { { "POSIX", 5, LOCK_MODE_POSIX },
			      { "OFD", 3, LOCK_MODE_OFD },
			      { "", 0, 0 } };

void sprintf_lock_modes(char **rest, int *left, int mode)
{
	int i;

	for (i = 0; lock_modes[i].t_len != 0; i++) {
		if (mode == lock_modes[i].t_value) {
			/* Append lock mode to line */
			sprint_left(*rest, *left, " %s", lock_modes[i].t_name);
			break;
		}
	}
}

const char *str_lock_mode(int lock_mode)
{
	switch ((enum lock_mode)lock_mode) {
	case LOCK_MODE_POSIX:
		return "POSIX";
	case LOCK_MODE_OFD:
		return "OFD";
	}

	return "unknown";
}

int readln(FILE *in, char *buf, int buflen)
{
	int len;

	if (fgets(buf, buflen, in) != NULL) {
		len = strlen(buf);
		if (buf[len - 1] == '\n')
			buf[--len] = '\0';
		return len;
	} else {
		return -1;
	}
}

char *SkipWhite(char *line, enum requires_more requires_more, const char *who)
{
	char *c = line;

	/* Skip white space */
	while (*c == ' ' || *c == '\t')
		c++;

	switch (requires_more) {
	case REQUIRES_MORE:
		if (*c == '\0' || *c == '#') {
			array_sprintf(errdetail,
				      "Expected more characters on command %s",
				      who);

			if (*c == '\0')
				array_strcpy(badtoken, "<NULL>");
			else
				array_strcpy(badtoken, c);

			errno = EINVAL;
			return NULL;
		}
		break;

	case REQUIRES_NO_MORE:
		if (*c != '\0' && *c != '#') {
			array_sprintf(errdetail,
				      "Extra characters on command %s", who);
			array_strcpy(badtoken, c);
			errno = EINVAL;
			return NULL;
		}
		break;

	case REQUIRES_EITHER:
		break;
	}

	/*
  if(*c == '#')
    c += strlen(c);
*/
	return c;
}

char *get_token(char *line, char **token, int *len, bool optional,
		enum requires_more requires_more, const char *invalid)
{
	char *c = line;

	if (optional)
		c = SkipWhite(c, REQUIRES_EITHER, invalid);
	else
		c = SkipWhite(c, REQUIRES_MORE, invalid);

	if (c == NULL)
		return c;

	if (optional && (*c == '\0' || *c == '#')) {
		*token = NULL;
		return c;
	}

	/* Skip token */
	*token = c;
	while (*c != ' ' && *c != '\0' && *c != '#')
		c++;

	*len = c - *token;

	return c;
}

char *get_token_value(char *line, int *value, struct token *tokens,
		      bool optional, enum requires_more requires_more,
		      const char *invalid)
{
	char *t;
	int len;
	char *c = get_token(line, &t, &len, optional, requires_more, invalid);
	struct token *tok;

	if (c == NULL)
		return c;

	if (optional && t == NULL) {
		tok = tokens;

		while (tok->t_len != 0)
			tok++;

		*value = tok->t_value;
		return c;
	}

	for (tok = tokens; tok->t_len != 0; tok++) {
		if (tok->t_len != len)
			continue;

		if (strncasecmp(t, tok->t_name, len) == 0) {
			*value = tok->t_value;
			return SkipWhite(c, requires_more, invalid);
		}
	}

	if (optional) {
		/* optional token not found, rewind to before token and
		 * use the default value
		 */
		*value = tok->t_value;
		return t;
	}

	array_strcpy(errdetail, invalid);
	array_strncpy(badtoken, t, len);
	errno = EINVAL;
	return NULL;
}

char *get_optional_token(char *line, bool *found, const char *token,
			 enum requires_more requires_more, const char *invalid)
{
	char *t;
	int len;
	char *c = get_token(line, &t, &len, true, requires_more, invalid);

	*found = false;

	if (c == NULL || t == NULL)
		return c;

	if (strncasecmp(t, token, strlen(token)) == 0) {
		*found = true;
		return SkipWhite(c, requires_more, invalid);
	}

	/* optional token not found, rewind to before token */
	return t;
}

char *get_client(char *line, struct client **pclient, bool create,
		 enum requires_more requires_more)
{
	char *c = line;
	char *t;
	struct client *client;
	int len;

	c = get_token(line, &t, &len, false, requires_more, "Invalid client");

	if (c == NULL)
		return c;

	for (client = client_list; client != NULL; client = client->c_next) {
		if (strlen(client->c_name) != len)
			continue;

		if (strncmp(t, client->c_name, len) == 0)
			break;
	}

	*pclient = client;

	if (client == NULL) {
		if (create) {
			client = calloc(1, sizeof(*client));

			if (client == NULL) {
				array_strcpy(errdetail,
					     "Could not create client");
				errno = ENOMEM;
				array_strncpy(badtoken, t, len);
				return NULL;
			}

			array_strncpy(client->c_name, t, len);

			*pclient = client;

			c = SkipWhite(c, requires_more, "get_client");

			if (c == NULL)
				free(client);
			else if (!quiet && !syntax)
				fprintf(output, "Created temp client %s\n",
					client->c_name);

			return c;
		} else {
			array_strcpy(errdetail, "Could not find client");
			errno = ENOENT;
			array_strncpy(badtoken, t, len);
			return NULL;
		}
	}

	return SkipWhite(c, requires_more, "get_client");
}

char *get_command(char *line, enum commands *cmd)
{
	enum commands i;
	char *t;
	char *c;
	int len;

	*cmd = NUM_COMMANDS;

	c = get_token(line, &t, &len, false, REQUIRES_EITHER,
		      "Invalid command 1");

	if (c == NULL)
		return c;

	for (i = CMD_OPEN; i < NUM_COMMANDS; i++) {
		if (len == commands[i].cmd_len &&
		    strncasecmp(t, commands[i].cmd_name, commands[i].cmd_len) ==
			    0) {
			*cmd = i;

			if (i == CMD_QUIT)
				return SkipWhite(c, REQUIRES_EITHER, "");
			else
				return SkipWhite(c, REQUIRES_MORE,
						 "Invalid command 2");
		}
	}

	array_strcpy(errdetail, "Invalid command 3");
	array_strcpy(badtoken, line);
	return NULL;
}

char *get_long(char *line, long *value, enum requires_more requires_more,
	       const char *invalid)
{
	char *c;
	char *t;
	char *e;
	int len;

	c = get_token(line, &t, &len, false, requires_more, invalid);
	if (c == NULL)
		return c;

	/* Extract value */
	if (*t == '*' && len == 1)
		*value = -1;
	else {
		*value = strtol(t, &e, 0);
		if (e != NULL && e != c) {
			array_strcpy(errdetail, invalid);
			array_strncpy(badtoken, t, len);
			errno = EINVAL;
			return NULL;
		}
	}

	return SkipWhite(c, requires_more, invalid);
}

char *get_unsignedlonglong(char *line, long long *value,
			   enum requires_more requires_more,
			   const char *invalid)
{
	char *c;
	char *t;
	char *e;
	int len;

	c = get_token(line, &t, &len, false, requires_more, invalid);
	if (c == NULL)
		return c;

	/* Extract value */
	if (*t == '*' && len == 1)
		*value = -1;
	else {
		*value = strtoull(t, &e, 0);

		if (e != NULL && e != c) {
			array_strcpy(errdetail, invalid);
			array_strncpy(badtoken, t, len);
			errno = EINVAL;
			return NULL;
		}
	}

	return SkipWhite(c, requires_more, invalid);
}

char *get_lock_type(char *line, int *type)
{
	return get_token_value(line, type, lock_types, false, REQUIRES_MORE,
			       "Invalid lock type");
}

char *get_on_off(char *line, bool *value)
{
	int tvalue;
	char *rest;

	rest = get_token_value(line, &tvalue, on_off, true, REQUIRES_NO_MORE,
			       "Invalid on/off");
	*value = (bool)tvalue;
	return rest;
}

char *get_fpos(char *line, long *fpos, enum requires_more requires_more)
{
	char *c = line;

	/* Get fpos */
	c = get_long(c, fpos, requires_more, "Invalid fpos");
	if (c == NULL)
		return c;

	if (*fpos < 0 || *fpos > MAXFPOS) {
		array_strcpy(errdetail, "Invalid fpos");
		array_sprintf(badtoken, "%ld", *fpos);
		errno = EINVAL;
		return NULL;
	}

	return c;
}

char *get_rdata(char *line, struct response *resp, int max,
		enum requires_more requires_more)
{
	char *c = line;
	char *t;
	int quoted;
	int len;

	assert(max < sizeof(resp->r_data));

	c = SkipWhite(c, REQUIRES_MORE, "get rdata 1");

	if (c == NULL)
		return c;

	if (*c != '"') {
		if (requires_more != REQUIRES_NO_MORE) {
			errno = EINVAL;
			array_strcpy(errdetail, "Expected string");
			array_strcpy(badtoken, c);
			return NULL;
		}
		quoted = false;
	} else {
		c++;
		quoted = true;
	}

	t = c;

	if (quoted)
		while (*c != '"' && *c != '\0')
			c++;
	else
		while (*c != '\0')
			c++;

	if (quoted && *c == '\0') {
		errno = EINVAL;
		array_strcpy(errdetail, "Unterminated string");
		array_strcpy(badtoken, t - 1);
		return NULL;
	}

	len = c - t;

	if (len > max) {
		errno = EINVAL;
		array_sprintf(errdetail, "String length %d longer than %d", len,
			      max);
		array_strcpy(badtoken, t - 1);
		return NULL;
	}

	resp->r_length = len;
	array_strncpy(resp->r_data, t, len);
	c++;

	return SkipWhite(c, requires_more, "get rdata 2");
}

char *get_open_opts(char *line, long *fpos, int *flags, int *mode,
		    int *lock_mode)
{
	char *c;
	int flag2 = -1;
	bool has_mode = false;

	/* Set default mode */
	*mode = S_IRUSR | S_IWUSR;

	/* Get fpos */
	c = get_fpos(line, fpos, REQUIRES_MORE);
	if (c == NULL)
		return c;

	c = get_token_value(c, flags, read_write_flags, false, REQUIRES_MORE,
			    "Invalid open flags");
	if (c == NULL)
		return c;

	*flags |= O_SYNC;

	/* Check optional open flags */
	while (flag2 != 0) {
		c = get_token_value(c, &flag2, open_flags, true, REQUIRES_MORE,
				    "Invalid optional open flag");

		if (c == NULL)
			return c;

		*flags |= flag2;
	}

	c = get_optional_token(c, &has_mode, "mode", REQUIRES_MORE,
			       "Invalid optional open flag");

	if (c == NULL)
		return c;

	if (has_mode) {
		/* We got a mode, now get the mode value */
		long modev;

		c = get_long(c, &modev, REQUIRES_MORE, "Invalid mode");

		if (c == NULL)
			return c;

		*mode = modev;
	}

	/* Check optional lock mode, default to POSIX */
	*lock_mode = LOCK_MODE_POSIX;

	c = get_token_value(c, lock_mode, lock_modes, true, REQUIRES_MORE,
			    "Invalid optional lock mode");
	if (c == NULL)
		return c;

	return SkipWhite(c, REQUIRES_MORE, "get_open_opts");
}

const char *str_status(enum status status)
{
	switch (status) {
	case STATUS_OK:
		return "OK";
	case STATUS_AVAILABLE:
		return "AVAILABLE";
	case STATUS_GRANTED:
		return "GRANTED";
	case STATUS_DENIED:
		return "DENIED";
	case STATUS_DEADLOCK:
		return "DEADLOCK";
	case STATUS_CONFLICT:
		return "CONFLICT";
	case STATUS_CANCELED:
		return "CANCELED";
	case STATUS_COMPLETED:
		return "COMPLETED";
	case STATUS_ERRNO:
		return "ERRNO";
	case STATUS_PARSE_ERROR:
		return "PARSE_ERROR";
	case STATUS_ERROR:
		return "ERROR";
	}

	return "unknown";
}

char *get_status(char *line, struct response *resp)
{
	enum status stat;
	char *t;
	int len;
	char *c = get_token(line, &t, &len, false, REQUIRES_EITHER,
			    "Invalid status");

	if (c == NULL)
		return c;

	for (stat = STATUS_OK; stat <= STATUS_ERROR; stat++) {
		const char *cmp_status = str_status(stat);

		if (strlen(cmp_status) != len)
			continue;

		if (strncasecmp(cmp_status, t, len) == 0) {
			enum requires_more requires_more = REQUIRES_MORE;

			resp->r_status = stat;

			if (stat == STATUS_COMPLETED ||
			    (resp->r_cmd == CMD_QUIT && stat == STATUS_OK))
				requires_more = REQUIRES_NO_MORE;

			return SkipWhite(c, requires_more, "get_status");
		}
	}

	array_strcpy(errdetail, "Invalid status");
	array_strncpy(badtoken, t, len);
	errno = EINVAL;
	return NULL;
}

void free_client(struct client *client)
{
	if (client == NULL)
		return;

	if (client->c_prev != NULL)
		client->c_prev->c_next = client->c_next;

	if (client->c_next != NULL)
		client->c_next->c_prev = client->c_prev;

	if (client_list == client)
		client_list = client->c_next;

	free(client);
}

void free_response(struct response *resp, struct response **list)
{
	if (resp == NULL)
		return;

	if (list != NULL && *list == resp)
		*list = resp->r_next;

	if (resp->r_prev != NULL)
		resp->r_prev->r_next = resp->r_next;

	if (resp->r_next != NULL)
		resp->r_next->r_prev = resp->r_prev;

	if (resp->r_client != NULL && --resp->r_client->c_refcount == 0) {
		free_client(resp->r_client);
		resp->r_client = NULL;
	}

	free(resp);
}

const char *str_lock_type(int type)
{
	switch (type) {
	case F_RDLCK:
		return "read";
	case F_WRLCK:
		return "write";
	case F_UNLCK:
		return "unlock";
	}

	return "unknown";
}

char *get_tag(char *line, struct response *resp, int required,
	      enum requires_more requires_more)
{
	if (*line == '$') {
		char *c = line + 1;

		if (tolower(*c) >= 'a' || tolower(*c) <= 'z')
			resp->r_tag = saved_tags[tolower(*c++) - 'a'];
		else
			resp->r_tag = get_global_tag(false);

		return SkipWhite(c, requires_more, "get_tag");
	}

	if (required || (*line != '\0' && *line != '#'))
		return get_long(line, &resp->r_tag, requires_more,
				"Invalid tag");

	resp->r_tag = -1;
	return line;
}

char *get_rq_tag(char *line, struct response *req, int required,
		 enum requires_more requires_more)
{
	if (*line == '$') {
		char *c = line + 1;

		req->r_tag = get_global_tag(true);

		if (tolower(*c) >= 'a' || tolower(*c) <= 'z')
			saved_tags[tolower(*c++) - 'a'] = get_global_tag(false);

		return SkipWhite(c, requires_more, "get_rq_tag");
	}

	if (required || (*line != '\0' && *line != '#'))
		return get_long(line, &req->r_tag, requires_more,
				"Invalid tag");

	req->r_tag = -1;
	return line;
}

void sprintf_resp(char *line, int size, const char *lead, struct response *resp)
{
	char *rest = line;
	int left = size - 1; /* Leave room for terminating NUL */

	if (lead != NULL) {
		const char *name = "<NULL>";

		if (resp->r_client != NULL)
			name = resp->r_client->c_name;

		sprint_left(rest, left, "%s %s ", lead, name);
	}

	sprint_left(rest, left, "%ld %s %s", resp->r_tag,
		    commands[resp->r_cmd].cmd_name, str_status(resp->r_status));

	switch (resp->r_status) {
	case STATUS_OK:
		switch (resp->r_cmd) {
		case CMD_COMMENT:
		case CMD_HELLO:
		case CMD_FORK:
			sprint_left(rest, left, " \"%s\"\n", resp->r_data);
			break;

		case CMD_LOCKW:
		case CMD_LOCK:
		case CMD_UNLOCK:
		case CMD_TEST:
		case CMD_LIST:
		case CMD_HOP:
		case CMD_UNHOP:
		case NUM_COMMANDS:
			sprint_left(rest, left, " Unexpected Status\n");
			break;

		case CMD_ALARM:
			sprint_left(rest, left, " %ld\n", resp->r_secs);
			break;

		case CMD_QUIT:
			sprint_left(rest, left, "\n");
			break;

		case CMD_OPEN:
			sprint_left(rest, left, " %ld %ld\n", resp->r_fpos,
				    resp->r_fno);
			break;

		case CMD_CLOSE:
		case CMD_SEEK:
			sprint_left(rest, left, " %ld\n", resp->r_fpos);
			break;

		case CMD_WRITE:
			sprint_left(rest, left, " %ld %llu\n", resp->r_fpos,
				    resp->r_length);
			break;

		case CMD_READ:
			sprint_left(rest, left, " %ld %llu \"%s\"\n",
				    resp->r_fpos, resp->r_length, resp->r_data);
			break;
		}
		break;

	case STATUS_AVAILABLE:
	case STATUS_GRANTED:
	case STATUS_DENIED:
	case STATUS_DEADLOCK:
		if (resp->r_cmd == CMD_LIST) {
			sprint_left(rest, left, " %ld %llu %llu\n",
				    resp->r_fpos, resp->r_start,
				    resp->r_length);
		} else {
			sprint_left(rest, left, " %ld %s %llu %llu\n",
				    resp->r_fpos,
				    str_lock_type(resp->r_lock_type),
				    resp->r_start, resp->r_length);
		}
		break;

	case STATUS_CONFLICT:
		sprint_left(rest, left, " %ld %ld %s %llu %llu\n", resp->r_fpos,
			    resp->r_pid, str_lock_type(resp->r_lock_type),
			    resp->r_start, resp->r_length);
		break;

	case STATUS_CANCELED:
		if (resp->r_cmd == CMD_LOCKW) {
			sprint_left(rest, left, " %ld %s %llu %llu\n",
				    resp->r_fpos,
				    str_lock_type(resp->r_lock_type),
				    resp->r_start, resp->r_length);
		} else if (resp->r_cmd == CMD_ALARM) {
			sprint_left(rest, left, " %ld\n", resp->r_secs);
		}
		break;

	case STATUS_COMPLETED:
		sprint_left(rest, left, "\n");
		break;

	case STATUS_ERRNO:
		if (errno == 0) {
			sprint_left(rest, left, " %ld \"%s\"\n", resp->r_errno,
				    errdetail);
		} else {
			sprint_left(rest, left,
				    " %ld \"%s\" \"%s\" bad token \"%s\"\n",
				    resp->r_errno, strerror(resp->r_errno),
				    errdetail, badtoken);
		}
		break;

	case STATUS_PARSE_ERROR:
		break;

	case STATUS_ERROR:
		break;
	}

	/* Make sure we are NUL terminated even if we used the last byte. */
	*rest = '\0';
}

void respond(struct response *resp)
{
	char line[MAXXFER];

	sprintf_resp(line, sizeof(line), NULL, resp);

	if (output != stdout) {
		fputs(line, output);
		fflush(output);
	}

	if (resp->r_status >= STATUS_ERRNO)
		fprintf_stderr("%s", line);
	else if (!quiet)
		fprintf(stdout, "%s", line);
}

char *parse_response(char *line, struct response *resp)
{
	char *rest;
	long long verify_len;

	if (resp->r_original[0] == '\0')
		array_strcpy(resp->r_original, line);

	resp->r_cmd = NUM_COMMANDS;
	resp->r_tag = -1;

	rest = get_tag(line, resp, true, REQUIRES_MORE);

	if (rest == NULL)
		goto fail;

	rest = get_command(rest, &resp->r_cmd);

	if (rest == NULL)
		goto fail;

	rest = get_status(rest, resp);

	if (rest == NULL)
		goto fail;

	switch (resp->r_status) {
	case STATUS_OK:
		switch (resp->r_cmd) {
		case CMD_COMMENT:
		case CMD_HELLO:
		case CMD_FORK:
			rest = get_rdata(rest, resp, MAXSTR, REQUIRES_NO_MORE);
			break;

		case CMD_LOCKW:
		case CMD_LOCK:
		case CMD_UNLOCK:
		case CMD_TEST:
		case CMD_LIST:
		case CMD_HOP:
		case CMD_UNHOP:
		case NUM_COMMANDS:
			array_strcpy(errdetail, "Unexpected Status");
			errno = EINVAL;
			array_sprintf(badtoken, "%s",
				      str_status(resp->r_status));
			goto fail;

		case CMD_ALARM:
			return get_long(rest, &resp->r_secs, REQUIRES_NO_MORE,
					"Invalid alarm time");

		case CMD_QUIT:
			return rest;

		case CMD_OPEN:
			rest = get_fpos(rest, &resp->r_fpos, REQUIRES_MORE);

			if (rest == NULL)
				goto fail;

			rest = get_long(rest, &resp->r_fno, REQUIRES_NO_MORE,
					"Invalid file number");
			break;

		case CMD_CLOSE:
		case CMD_SEEK:
			rest = get_fpos(rest, &resp->r_fpos, REQUIRES_NO_MORE);
			break;

		case CMD_WRITE:
			rest = get_fpos(rest, &resp->r_fpos, REQUIRES_MORE);

			if (rest == NULL)
				goto fail;

			rest = get_unsignedlonglong(rest, &resp->r_length,
						    REQUIRES_NO_MORE,
						    "Invalid length");
			break;

		case CMD_READ:
			rest = get_fpos(rest, &resp->r_fpos, REQUIRES_MORE);

			if (rest == NULL)
				goto fail;

			rest = get_unsignedlonglong(rest, &resp->r_length,
						    REQUIRES_MORE,
						    "Invalid length");

			if (rest == NULL)
				goto fail;

			verify_len = resp->r_length;

			rest = get_rdata(rest, resp, MAXSTR, REQUIRES_NO_MORE);

			if (verify_len != resp->r_length) {
				array_strcpy(errdetail,
					     "Read length doesn't match");
				errno = EINVAL;
				array_sprintf(badtoken, "%llu != %llu",
					      verify_len, resp->r_length);
				goto fail;
			}
			break;
		}
		break;

	case STATUS_AVAILABLE:
	case STATUS_GRANTED:
	case STATUS_DENIED:
	case STATUS_DEADLOCK:
		rest = get_fpos(rest, &resp->r_fpos, REQUIRES_MORE);

		if (rest == NULL)
			goto fail;

		if (resp->r_cmd != CMD_LIST) {
			rest = get_lock_type(rest, &resp->r_lock_type);
			if (rest == NULL)
				goto fail;
		}

		rest = get_unsignedlonglong(rest, &resp->r_start, REQUIRES_MORE,
					    "Invalid lock start");
		if (rest == NULL)
			goto fail;

		rest = get_unsignedlonglong(rest, &resp->r_length,
					    REQUIRES_NO_MORE,
					    "Invalid lock length");
		break;

	case STATUS_CONFLICT:
		rest = get_fpos(rest, &resp->r_fpos, REQUIRES_MORE);

		if (rest == NULL)
			goto fail;

		rest = get_long(rest, &resp->r_pid, REQUIRES_MORE,
				"Invalid conflict pid");

		if (rest == NULL)
			goto fail;

		rest = get_lock_type(rest, &resp->r_lock_type);

		if (rest == NULL)
			goto fail;

		rest = get_unsignedlonglong(rest, &resp->r_start, REQUIRES_MORE,
					    "Invalid lock start");

		if (rest == NULL)
			goto fail;

		rest = get_unsignedlonglong(rest, &resp->r_length,
					    REQUIRES_NO_MORE,
					    "Invalid lock length");
		break;

	case STATUS_CANCELED:
		if (resp->r_cmd == CMD_LOCKW) {
			rest = get_fpos(rest, &resp->r_fpos, REQUIRES_MORE);

			if (rest == NULL)
				goto fail;

			rest = get_lock_type(rest, &resp->r_lock_type);

			if (rest == NULL)
				goto fail;

			rest = get_unsignedlonglong(rest, &resp->r_start,
						    REQUIRES_MORE,
						    "Invalid lock start");

			if (rest == NULL)
				goto fail;

			rest = get_unsignedlonglong(rest, &resp->r_length,
						    REQUIRES_NO_MORE,
						    "Invalid lock length");
		} else if (resp->r_cmd == CMD_ALARM) {
			rest = get_long(rest, &resp->r_secs, REQUIRES_NO_MORE,
					"Invalid alarm time");
		}
		break;

	case STATUS_COMPLETED:
		break;

	case STATUS_ERRNO:
		rest = get_long(rest, &resp->r_errno, REQUIRES_MORE,
				"Invalid errno");

		if (rest == NULL)
			goto fail;

		array_strcpy(resp->r_data, rest);
		rest += strlen(rest);
		break;

	case STATUS_PARSE_ERROR:
		break;

	case STATUS_ERROR:
		break;
	}

	if (rest != NULL)
		return rest;

fail:
	resp->r_status = STATUS_PARSE_ERROR;
	array_sprintf(resp->r_data,
		      "%s %ld ERRNO %d \"%s\" \"%s\" bad token \"%s\"",
		      commands[resp->r_cmd].cmd_name, resp->r_tag, errno,
		      strerror(errno), errdetail, badtoken);
	resp->r_cmd = NUM_COMMANDS;
	return NULL;
}

#define return_if_ne_lock_type(expected, received)                          \
	do {                                                                \
		if (expected != -1 && expected != received) {               \
			array_sprintf(errdetail, "Unexpected lock type %s", \
				      lock_types[received].t_name);         \
			return false;                                       \
		}                                                           \
	} while (0)

#define return_if_ne_long(expected, received, fmt)                      \
	do {                                                            \
		if (expected != -1 && expected != received) {           \
			array_sprintf(errdetail, fmt " %ld", received); \
			return false;                                   \
		}                                                       \
	} while (0)

#define return_if_ne_unsignedlonglong(expected, received, fmt)           \
	do {                                                             \
		if (expected != -1 && expected != received) {            \
			array_sprintf(errdetail, fmt " %llu", received); \
			return false;                                    \
		}                                                        \
	} while (0)

#define return_if_ne_string(expected, received, fmt)                   \
	do {                                                           \
		if (strcmp(expected, "*") != 0 &&                      \
		    strcmp(expected, received) != 0) {                 \
			array_sprintf(errdetail, fmt " %s", received); \
			return false;                                  \
		}                                                      \
	} while (0)

int compare_responses(struct response *expected, struct response *received)
{
	errno = 0;

	if (received == NULL) {
		array_strcpy(errdetail, "Unexpected NULL response");
		return false;
	}

	if (expected->r_client != received->r_client &&
	    strcmp(expected->r_client->c_name, received->r_client->c_name) !=
		    0) {
		array_sprintf(errdetail, "Unexpected response from %s",
			      received->r_client->c_name);
		return false;
	}

	if (expected->r_cmd != received->r_cmd) {
		array_sprintf(errdetail, "Unexpected command %s",
			      commands[received->r_cmd].cmd_name);
		return false;
	}

	return_if_ne_long(expected->r_tag, received->r_tag, "Unexpected tag");

	if (expected->r_status != received->r_status) {
		array_sprintf(errdetail, "Unexpected status %s",
			      str_status(received->r_status));
		return false;
	}

	switch (expected->r_status) {
	case STATUS_OK:
		switch (expected->r_cmd) {
		case CMD_COMMENT:
		case CMD_HELLO:
		case CMD_FORK:
			/* could check string, but not worth it - HELLO has
			 * already set client name and that has been checked
			 */
			break;

		case CMD_LOCKW:
		case CMD_LOCK:
		case CMD_UNLOCK:
		case CMD_TEST:
		case CMD_LIST:
		case CMD_HOP:
		case CMD_UNHOP:
		case NUM_COMMANDS:
			array_sprintf(errdetail, "Unexpected Status %s for %s",
				      str_status(received->r_status),
				      commands[received->r_cmd].cmd_name);
			return false;

		case CMD_ALARM:
			return_if_ne_long(expected->r_secs, received->r_secs,
					  "Unexpected secs");
			break;

		case CMD_QUIT:
			break;

		case CMD_OPEN:
			return_if_ne_long(expected->r_fpos, received->r_fpos,
					  "Unexpected fpos");
			return_if_ne_long(expected->r_fno, received->r_fno,
					  "Unexpected file number");
			break;

		case CMD_CLOSE:
		case CMD_SEEK:
			return_if_ne_long(expected->r_fpos, received->r_fpos,
					  "Unexpected fpos");
			break;

		case CMD_WRITE:
			return_if_ne_long(expected->r_fpos, received->r_fpos,
					  "Unexpected fpos");
			return_if_ne_unsignedlonglong(expected->r_length,
						      received->r_length,
						      "Unexpected length");
			break;

		case CMD_READ:
			return_if_ne_long(expected->r_fpos, received->r_fpos,
					  "Unexpected fpos");
			return_if_ne_unsignedlonglong(expected->r_length,
						      received->r_length,
						      "Unexpected length");
			return_if_ne_string(expected->r_data, received->r_data,
					    "Unexpected data");
			break;
		}
		break;

	case STATUS_AVAILABLE:
	case STATUS_GRANTED:
	case STATUS_DENIED:
	case STATUS_DEADLOCK:
		return_if_ne_long(expected->r_fpos, received->r_fpos,
				  "Unexpected fpos");

		if (expected->r_cmd != CMD_LIST)
			return_if_ne_lock_type(expected->r_lock_type,
					       received->r_lock_type);

		return_if_ne_unsignedlonglong(expected->r_start,
					      received->r_start,
					      "Unexpected start");
		return_if_ne_unsignedlonglong(expected->r_length,
					      received->r_length,
					      "Unexpected length");
		break;

	case STATUS_CONFLICT:
		return_if_ne_long(expected->r_fpos, received->r_fpos,
				  "Unexpected fpos");
		return_if_ne_long(expected->r_pid, received->r_pid,
				  "Unexpected pid");
		return_if_ne_lock_type(expected->r_lock_type,
				       received->r_lock_type);
		return_if_ne_unsignedlonglong(expected->r_start,
					      received->r_start,
					      "Unexpected start");
		return_if_ne_unsignedlonglong(expected->r_length,
					      received->r_length,
					      "Unexpected length");
		break;

	case STATUS_CANCELED:
		if (expected->r_cmd == CMD_LOCKW) {
			return_if_ne_long(expected->r_fpos, received->r_fpos,
					  "Unexpected fpos");
			return_if_ne_lock_type(expected->r_lock_type,
					       received->r_lock_type);
			return_if_ne_unsignedlonglong(expected->r_start,
						      received->r_start,
						      "Unexpected start");
			return_if_ne_unsignedlonglong(expected->r_length,
						      received->r_length,
						      "Unexpected length");
		} else if (expected->r_cmd == CMD_ALARM) {
			return_if_ne_long(expected->r_secs, received->r_secs,
					  "Unexpected secs");
		}
		break;

	case STATUS_COMPLETED:
		break;

	case STATUS_ERRNO:
		break;

	case STATUS_PARSE_ERROR:
		break;

	case STATUS_ERROR:
		break;
	}

	return true;
}

void add_response(struct response *resp, struct response **list)
{
	resp->r_next = *list;

	if (*list != NULL)
		(*list)->r_prev = resp;

	*list = resp;
}

struct response *check_expected_responses(struct response *expected_responses,
					  struct response *client_resp)
{
	struct response *expected_resp = expected_responses;

	while (expected_resp != NULL &&
	       !compare_responses(expected_resp, client_resp))
		expected_resp = expected_resp->r_next;

	return expected_resp;
}

char *parse_alarm(char *line, struct response *req)
{
	return get_long(line, &req->r_secs, REQUIRES_NO_MORE, "Invalid secs");
}

char *parse_open(char *line, struct response *req)
{
	char *more = get_open_opts(line, &req->r_fpos, &req->r_flags,
				   &req->r_mode, &req->r_lock_type);

	if (more == NULL)
		return more;

	return get_rdata(more, req, PATH_MAX - 1, REQUIRES_NO_MORE);
}

char *parse_write(char *line, struct response *req)
{
	char *more;

	more = get_fpos(line, &req->r_fpos, REQUIRES_MORE);

	if (more == NULL)
		return more;

	return get_rdata(more, req, MAXSTR, REQUIRES_NO_MORE);
}

char *parse_read(char *line, struct response *req)
{
	char *more;

	req->r_data[0] = '\0';

	more = get_fpos(line, &req->r_fpos, REQUIRES_MORE);

	if (more == NULL)
		return more;

	if (*more == '"')
		return get_rdata(more, req, MAXSTR, REQUIRES_NO_MORE);
	else
		return get_unsignedlonglong(more, &req->r_length,
					    REQUIRES_NO_MORE, "Invalid len");
}

char *parse_seek(char *line, struct response *req)
{
	char *more;

	more = get_fpos(line, &req->r_fpos, REQUIRES_MORE);

	if (more == NULL)
		return more;

	return get_unsignedlonglong(more, &req->r_start, REQUIRES_NO_MORE,
				    "Invalid pos");
}

char *parse_lock(char *line, struct response *req)
{
	char *more;

	more = get_fpos(line, &req->r_fpos, REQUIRES_MORE);

	if (more == NULL)
		return more;

	more = get_lock_type(more, &req->r_lock_type);

	if (more == NULL)
		return more;

	if (req->r_lock_type != F_RDLCK && req->r_lock_type != F_WRLCK) {
		errno = EINVAL;
		array_strcpy(errdetail, "Invalid lock type");
		array_sprintf(badtoken, "%s", str_lock_type(req->r_lock_type));
	}

	more = get_unsignedlonglong(more, &req->r_start, REQUIRES_MORE,
				    "Invalid lock start");

	if (more == NULL)
		return more;

	return get_unsignedlonglong(more, &req->r_length, REQUIRES_NO_MORE,
				    "Invalid lock len");
}

char *parse_unlock(char *line, struct response *req)
{
	char *more;

	req->r_lock_type = F_UNLCK;

	more = get_fpos(line, &req->r_fpos, REQUIRES_MORE);

	if (more == NULL)
		return more;

	more = get_unsignedlonglong(more, &req->r_start, REQUIRES_MORE,
				    "Invalid lock start");

	if (more == NULL)
		return more;

	return get_unsignedlonglong(more, &req->r_length, REQUIRES_NO_MORE,
				    "Invalid lock len");
}

char *parse_close(char *line, struct response *req)
{
	return get_fpos(line, &req->r_fpos, REQUIRES_NO_MORE);
}

char *parse_list(char *line, struct response *req)
{
	char *more;

	req->r_lock_type = F_WRLCK;

	more = get_fpos(line, &req->r_fpos, REQUIRES_MORE);

	if (more == NULL)
		return more;

	more = get_unsignedlonglong(more, &req->r_start, REQUIRES_MORE,
				    "Invalid lock start");

	if (more == NULL)
		return more;

	return get_unsignedlonglong(more, &req->r_length, REQUIRES_NO_MORE,
				    "Invalid lock len");
}

char *parse_string(char *line, struct response *req)
{
	return get_rdata(line, req, MAXSTR, REQUIRES_NO_MORE);
}

char *parse_empty(char *line, struct response *req)
{
	return line;
}

typedef char *(*parse_function_t)(char *line, struct response *req);

parse_function_t parse_functions[NUM_COMMANDS] = {
	parse_open,   parse_close,  parse_lock, /* lock */
	parse_lock, /* lockw */
	parse_unlock, parse_lock, /* test */
	parse_list,   parse_lock, /* hop */
	parse_unlock, /* unhop */
	parse_seek,   parse_read,   parse_write, parse_string, /* comment */
	parse_alarm,  parse_string, /* hello */
	parse_string, /* fork */
	parse_empty, /* quit */
};

char *parse_request(char *line, struct response *req, int no_tag)
{
	char *rest = line;

	req->r_cmd = NUM_COMMANDS;
	req->r_tag = -1;

	if (no_tag)
		req->r_tag = get_global_tag(true);
	else
		rest = get_rq_tag(rest, req, true, REQUIRES_MORE);

	if (rest == NULL)
		return rest;

	rest = get_command(rest, &req->r_cmd);

	if (rest == NULL)
		return rest;

	switch (req->r_cmd) {
	case CMD_OPEN:
	case CMD_CLOSE:
	case CMD_LOCKW:
	case CMD_LOCK:
	case CMD_UNLOCK:
	case CMD_TEST:
	case CMD_LIST:
	case CMD_HOP:
	case CMD_UNHOP:
	case CMD_SEEK:
	case CMD_READ:
	case CMD_WRITE:
	case CMD_HELLO:
	case CMD_FORK:
	case CMD_COMMENT:
	case CMD_ALARM:
	case CMD_QUIT:
		rest = parse_functions[req->r_cmd](rest, req);
		break;

	case NUM_COMMANDS:
		break;
	}

	return rest;
}

void send_cmd(struct response *req)
{
	char line[MAXXFER];

	sprintf_req(line, sizeof(line), NULL, req);

	fputs(line, req->r_client->c_output);
	fflush(req->r_client->c_output);
}

void sprintf_req(char *line, int size, const char *lead, struct response *req)
{
	char *rest = line;
	int left = size - 1; /* Leave room for terminating NUL */

	if (lead != NULL) {
		const char *name = "<NULL>";

		if (req->r_client != NULL)
			name = req->r_client->c_name;

		sprint_left(rest, left, "%s %s ", lead, name);
	}

	sprint_left(rest, left, "%ld %s", req->r_tag,
		    commands[req->r_cmd].cmd_name);

	switch (req->r_cmd) {
	case CMD_COMMENT:
	case CMD_HELLO:
	case CMD_FORK:
		sprint_left(rest, left, " \"%s\"\n", req->r_data);
		break;

	case CMD_LOCKW:
	case CMD_LOCK:
	case CMD_TEST:
	case CMD_HOP:
		sprint_left(rest, left, " %ld %s %llu %llu\n", req->r_fpos,
			    str_lock_type(req->r_lock_type), req->r_start,
			    req->r_length);
		break;

	case CMD_UNLOCK:
	case CMD_LIST:
	case CMD_UNHOP:
		sprint_left(rest, left, " %ld %llu %llu\n", req->r_fpos,
			    req->r_start, req->r_length);
		break;

	case NUM_COMMANDS:
		sprint_left(rest, left, " Unexpected Command\n");
		break;

	case CMD_ALARM:
		sprint_left(rest, left, " %ld\n", req->r_secs);
		break;

	case CMD_QUIT:
		sprint_left(rest, left, "\n");
		break;

	case CMD_OPEN:
		sprint_left(rest, left, " %ld %s", req->r_fpos,
			    str_read_write_flags(req->r_flags));
		sprintf_open_flags(&rest, &left, req->r_flags);
		sprintf_lock_modes(&rest, &left, req->r_lock_type);
		sprint_left(rest, left, " \"%s\"\n", req->r_data);
		break;

	case CMD_CLOSE:
		sprint_left(rest, left, " %ld\n", req->r_fpos);
		break;

	case CMD_SEEK:
		sprint_left(rest, left, " %ld %llu\n", req->r_fpos,
			    req->r_start);
		break;

	case CMD_WRITE:
		sprint_left(rest, left, " %ld %s\n", req->r_fpos, req->r_data);
		break;

	case CMD_READ:
		sprint_left(rest, left, " %ld %llu\n", req->r_fpos,
			    req->r_length);
		break;
	}

	/* Make sure we are NUL terminated even if we used the last byte. */
	*rest = '\0';
}
