/**
 * @file   textfilecontent54.c
 * @brief  textfilecontent54 probe
 * @author "Tomas Heinrich" <theinric@redhat.com>
 *
 * 2010/06/13 dkopecek@redhat.com
 *  This probe is able to process a textfilecontent54_object as defined in OVAL 5.4 and 5.5.
 *
 */

/*
 * Copyright 2009 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *      "Tomas Heinrich" <theinric@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#if defined USE_REGEX_PCRE
#include <pcre.h>
#elif defined USE_REGEX_POSIX
#include <regex.h>
#endif

#include <seap.h>
#include <probe-api.h>
#include <probe/entcmp.h>
#include <probe/probe.h>
#include <probe/option.h>
#include <oval_fts.h>
#include <alloc.h>
#include "common/assume.h"
#include "common/debug_priv.h"

#define FILE_SEPARATOR '/'

oval_schema_version_t over;

#if defined USE_REGEX_PCRE
static int get_substrings(char *str, int *ofs, pcre *re, int want_substrs, char ***substrings) {
	int i, ret, rc;
	int ovector[60], ovector_len = sizeof (ovector) / sizeof (ovector[0]);
	char **substrs;

	// todo: max match count check

	for (i = 0; i < ovector_len; ++i)
		ovector[i] = -1;

#if defined(__SVR4) && defined(__sun)
	rc = pcre_exec(re, NULL, str, strlen(str), *ofs, PCRE_NO_UTF8_CHECK, ovector, ovector_len);
#else
	rc = pcre_exec(re, NULL, str, strlen(str), *ofs, 0, ovector, ovector_len);
#endif

	if (rc < -1) {
		dE("Function pcre_exec() failed to match a regular expression with return code %d on string '%s'.", rc, str);
		return rc;
	} else if (rc == -1) {
		/* no match */
		return 0;
	}

	*ofs = (*ofs == ovector[1]) ? ovector[1] + 1 : ovector[1];

	if (!want_substrs) {
		/* just report successful match */
		return 1;
	}

	ret = 0;
	if (rc == 0) {
		/* vector too small */
		// todo: report partial results
		rc = ovector_len / 3;
	}

	substrs = malloc(rc * sizeof (char *));
	for (i = 0; i < rc; ++i) {
		int len;
		char *buf;

		if (ovector[2 * i] == -1)
			continue;
		len = ovector[2 * i + 1] - ovector[2 * i];
		buf = malloc(len + 1);
		memcpy(buf, str + ovector[2 * i], len);
		buf[len] = '\0';
		substrs[ret] = buf;
		++ret;
	}

	*substrings = substrs;

	return ret;
}
#elif defined USE_REGEX_POSIX
static int get_substrings(char *str, int *ofs, regex_t *re, int want_substrs, char ***substrings) {
	int i, ret, rc;
	regmatch_t pmatch[40];
	int pmatch_len = sizeof (pmatch) / sizeof (pmatch[0]);
	char **substrs;

	rc = regexec(re, str + *ofs, pmatch_len, pmatch, 0);
	if (rc == REG_NOMATCH) {
		/* no match */
		return 0;
	}

	*ofs += (0 == pmatch[0].rm_eo) ? 1 : pmatch[0].rm_eo;

	if (!want_substrs) {
		/* just report successful match */
		return 1;
	}

	ret = 0;
	substrs = malloc(pmatch_len * sizeof (char *));
	for (i = 0; i < pmatch_len; ++i) {
		int len;
		char *buf;

		if (pmatch[i].rm_so == -1)
			continue;
		len = pmatch[i].rm_eo - pmatch[i].rm_so;
		buf = malloc(len + 1);
		memcpy(buf, str + pmatch[i].rm_so, len);
		buf[len] = '\0';
		substrs[ret] = buf;
		++ret;
	}

	*substrings = substrs;

	return ret;
}
#endif

static SEXP_t *create_item(const char *path, const char *filename, char *pattern,
			   int instance, char **substrs, int substr_cnt)
{
	int i;
	SEXP_t *item;
	SEXP_t *r0;
	SEXP_t *se_instance, *se_filepath;
	char *text;

        if (strlen(path) + strlen(filename) + 1 > PATH_MAX) {
                dE("path+filename too long");
                return (NULL);
        }

	if (oval_schema_version_cmp(over, OVAL_SCHEMA_VERSION(5.4)) < 0) {
		pattern = text = NULL;
		se_instance = NULL;
	} else {
		text = substrs[0];
		se_instance = SEXP_number_newu_64((int64_t) instance);
	}
	if (oval_schema_version_cmp(over, OVAL_SCHEMA_VERSION(5.6)) < 0) {
		se_filepath = NULL;
	} else {
		const size_t path_len = strlen(path);
		/* Avoid 2 slashes */
		if (path_len >= 1 && path[path_len - 1] == FILE_SEPARATOR) {
			se_filepath = SEXP_string_newf("%s%s", path, filename);
		} else {
			se_filepath = SEXP_string_newf("%s%c%s", path, FILE_SEPARATOR, filename);
		}
	}

        item = probe_item_create(OVAL_INDEPENDENT_TEXT_FILE_CONTENT, NULL,
                                 "filepath", OVAL_DATATYPE_SEXP, se_filepath,
                                 "path",     OVAL_DATATYPE_STRING, path,
                                 "filename", OVAL_DATATYPE_STRING, filename,
                                 "pattern",  OVAL_DATATYPE_STRING, pattern,
                                 "instance", OVAL_DATATYPE_SEXP, se_instance,
                                 "line",     OVAL_DATATYPE_STRING, pattern,
                                 "text",     OVAL_DATATYPE_STRING, substrs[0],
                                 NULL);

	for (i = 1; i < substr_cnt; ++i) {
                probe_item_ent_add (item, "subexpression", NULL, r0 = SEXP_string_new (substrs[i], strlen (substrs[i])));
                SEXP_free (r0);
	}

	SEXP_free(se_filepath);
	SEXP_free(se_instance);
	return item;
}

struct pfdata {
	char *pattern;
	int re_opts;
	SEXP_t *instance_ent;
        probe_ctx *ctx;
#if defined USE_REGEX_PCRE
	pcre *compiled_regex;
#elif defined USE_REGEX_POSIX
	regex_t *compiled_regex;
#endif
};

static int process_file(const char *prefix, const char *path, const char *file, void *arg)
{
	struct pfdata *pfd = (struct pfdata *) arg;
	int ret = 0, path_len, file_len, cur_inst = 0, fd = -1, substr_cnt,
		buf_size = 0, buf_used = 0, ofs = 0, buf_inc = 4096;
	char **substrs = NULL;
	char *whole_path = NULL, *whole_path_with_prefix = NULL, *buf = NULL;
	SEXP_t *next_inst = NULL;
	struct stat st;

	if (file == NULL)
		goto cleanup;

	path_len   = strlen(path);
	file_len   = strlen(file);
	whole_path = malloc(path_len + file_len + 2);

	memcpy(whole_path, path, path_len);

	if (whole_path[path_len - 1] != FILE_SEPARATOR) {
		whole_path[path_len] = FILE_SEPARATOR;
		++path_len;
	}

	memcpy(whole_path + path_len, file, file_len + 1);

	/*
	 * If stat() fails, don't report an error and just skip the file.
	 * This is an expected situation, because the fts_*() functions
	 * are called with the 'FTS_PHYSICAL' option. Normally, stumbling
	 * upon a symlink without a target would cause fts_read() to return
	 * the 'FTS_SLNONE' flag, but the 'FTS_PHYSICAL' option causes it
	 * to return 'FTS_SL' and the presence of a valid target has to
	 * be determined with stat().
	 */
	whole_path_with_prefix = oscap_path_join(prefix, whole_path);
	if (stat(whole_path_with_prefix, &st) == -1)
		goto cleanup;
	if (!S_ISREG(st.st_mode))
		goto cleanup;

	fd = open(whole_path_with_prefix, O_RDONLY);
	if (fd == -1) {
		SEXP_t *msg;

		msg = probe_msg_creatf(OVAL_MESSAGE_LEVEL_ERROR, "open(): '%s' %s.", whole_path, strerror(errno));
		probe_cobj_add_msg(probe_ctx_getresult(pfd->ctx), msg);
		SEXP_free(msg);
		probe_cobj_set_flag(probe_ctx_getresult(pfd->ctx), SYSCHAR_FLAG_ERROR);
		ret = -1;
		goto cleanup;
	}

	do {
		buf_size += buf_inc;
		buf = realloc(buf, buf_size);
		ret = read(fd, buf + buf_used, buf_inc);
		if (ret == -1) {
			SEXP_t *msg;

			msg = probe_msg_creatf(OVAL_MESSAGE_LEVEL_ERROR, "read(): '%s' %s.", whole_path, strerror(errno));
			probe_cobj_add_msg(probe_ctx_getresult(pfd->ctx), msg);
			SEXP_free(msg);
			probe_cobj_set_flag(probe_ctx_getresult(pfd->ctx), SYSCHAR_FLAG_ERROR);
			ret = -2;
			goto cleanup;
		}
		buf_used += ret;
	} while (ret == buf_inc);

	if (buf_used == buf_size)
		buf = realloc(buf, ++buf_size);
	buf[buf_used++] = '\0';

	do {
		int want_instance;

		next_inst = SEXP_number_newi_32(cur_inst + 1);

		if (probe_entobj_cmp(pfd->instance_ent, next_inst) == OVAL_RESULT_TRUE)
			want_instance = 1;
		else
			want_instance = 0;

		SEXP_free(next_inst);
		substr_cnt = get_substrings(buf, &ofs, pfd->compiled_regex, want_instance, &substrs);

		if (substr_cnt < 0) {
			SEXP_t *msg;
			msg = probe_msg_creatf(OVAL_MESSAGE_LEVEL_ERROR,
				"Regular expression pattern match failed in file %s with error %d.",
				whole_path, substr_cnt);
			probe_cobj_add_msg(probe_ctx_getresult(pfd->ctx), msg);
			SEXP_free(msg);
			probe_cobj_set_flag(probe_ctx_getresult(pfd->ctx), SYSCHAR_FLAG_ERROR);
			ret = -3;
			goto cleanup;
		}

		if (substr_cnt > 0) {
			++cur_inst;

			if (want_instance) {
				int k;
				SEXP_t *item;

				item = create_item(path, file, pfd->pattern,
						   cur_inst, substrs, substr_cnt);

                                probe_item_collect(pfd->ctx, item);

				for (k = 0; k < substr_cnt; ++k)
					free(substrs[k]);
				free(substrs);
			}
		}
	} while (substr_cnt > 0 && ofs < buf_used);

 cleanup:
	if (fd != -1)
		close(fd);
	free(buf);
	if (whole_path != NULL)
		free(whole_path);
	free(whole_path_with_prefix);

	/* coverity[leaked_storage] - substrs is not leaked */
	return ret;
}

int probe_offline_mode_supported()
{
	return PROBE_OFFLINE_OWN;
}

int probe_main(probe_ctx *ctx, void *arg)
{
	SEXP_t *path_ent, *file_ent, *inst_ent, *bh_ent, *patt_ent, *filepath_ent, *probe_in;
        SEXP_t *r0;
	bool val;
	struct pfdata pfd;
	int ret = 0;
#if defined USE_REGEX_PCRE
	int errorffset = -1;
	const char *error;
#elif defined USE_REGEX_POSIX
	regex_t _re;
	pfd.compiled_regex = &_re;
	int err;
#endif
	OVAL_FTS    *ofts;
	OVAL_FTSENT *ofts_ent;

        (void)arg;

	memset(&pfd, 0, sizeof(pfd));

        probe_in = probe_ctx_getobject(ctx);

	over = probe_obj_get_platform_schema_version(probe_in);
        path_ent = probe_obj_getent(probe_in, "path",     1);
        file_ent = probe_obj_getent(probe_in, "filename", 1);
        inst_ent = probe_obj_getent(probe_in, "instance", 1);
        patt_ent = probe_obj_getent(probe_in, "pattern",  1);
        filepath_ent = probe_obj_getent(probe_in, "filepath",  1);
	bh_ent = probe_obj_getent(probe_in, "behaviors", 1);

        /* we want (path+filename or filepath) + instance + pattern*/
        if ( ((path_ent == NULL || file_ent == NULL) && filepath_ent==NULL) || 
             inst_ent==NULL || 
             patt_ent==NULL) {
                SEXP_free (patt_ent);
		ret = PROBE_ENOELM;
		goto cleanup;
        }

	/* get pattern from SEXP */
        SEXP_t *ent_val;
        ent_val = probe_ent_getval(patt_ent);
	pfd.pattern = SEXP_string_cstr(ent_val);
        SEXP_free(patt_ent);
        SEXP_free(ent_val);
	assume_d(pfd.pattern != NULL, -1);

	probe_tfc54behaviors_canonicalize(&bh_ent);

	pfd.instance_ent = inst_ent;
        pfd.ctx          = ctx;
#if defined USE_REGEX_PCRE
	pfd.re_opts = PCRE_UTF8;
	r0 = probe_ent_getattrval(bh_ent, "ignore_case");
	if (r0) {
		val = SEXP_string_getb(r0);
		SEXP_free(r0);
		if (val)
			pfd.re_opts |= PCRE_CASELESS;
	}
	r0 = probe_ent_getattrval(bh_ent, "multiline");
	if (r0) {
		val = SEXP_string_getb(r0);
		SEXP_free(r0);
		if (val)
			pfd.re_opts |= PCRE_MULTILINE;
	}
	r0 = probe_ent_getattrval(bh_ent, "singleline");
	if (r0) {
		val = SEXP_string_getb(r0);
		SEXP_free(r0);
		if (val)
			pfd.re_opts |= PCRE_DOTALL;
	}

	pfd.compiled_regex = pcre_compile(pfd.pattern, pfd.re_opts, &error,
					  &errorffset, NULL);
	if (pfd.compiled_regex == NULL) {
		SEXP_t *msg;

		msg = probe_msg_creatf(OVAL_MESSAGE_LEVEL_ERROR, "pcre_compile() '%s' %s.", pfd.pattern, error);
		probe_cobj_add_msg(probe_ctx_getresult(pfd.ctx), msg);
		SEXP_free(msg);
		probe_cobj_set_flag(probe_ctx_getresult(pfd.ctx), SYSCHAR_FLAG_ERROR);
		goto cleanup;
	}
#elif defined USE_REGEX_POSIX
	pfd.re_opts = REG_EXTENDED | REG_NEWLINE;
	r0 = probe_ent_getattrval(bh_ent, "ignore_case");
	if (r0) {
		val = SEXP_string_getb(r0);
		SEXP_free(r0);
		if (val)
			pfd.re_opts |= REG_ICASE;
	}

	if ((err = regcomp(pfd.compiled_regex, pfd.pattern, pfd.re_opts)) != 0) {
		SEXP_t *msg;

		msg = probe_msg_creatf(OVAL_MESSAGE_LEVEL_ERROR, "regcomp() '%s' returned %d.", pfd.pattern, err);
		probe_cobj_add_msg(probe_ctx_getresult(pfd.ctx), msg);
		SEXP_free(msg);
		probe_cobj_set_flag(probe_ctx_getresult(pfd.ctx), SYSCHAR_FLAG_ERROR);
		goto cleanup;
	}
#endif
	const char *prefix = getenv("OSCAP_PROBE_ROOT");

	if ((ofts = oval_fts_open_prefixed(prefix, path_ent, file_ent, filepath_ent, bh_ent, probe_ctx_getresult(ctx))) != NULL) {
		while ((ofts_ent = oval_fts_read(ofts)) != NULL) {
			if (ofts_ent->fts_info == FTS_F
			    || ofts_ent->fts_info == FTS_SL) {
				// todo: handle return code
				process_file(prefix, ofts_ent->path, ofts_ent->file, &pfd);
			}
			oval_ftsent_free(ofts_ent);
		}

		oval_fts_close(ofts);
	}

 cleanup:
        SEXP_free(file_ent);
        SEXP_free(path_ent);
        SEXP_free(inst_ent);
        SEXP_free(bh_ent);
        SEXP_free(filepath_ent);
	if (pfd.pattern != NULL)
		free(pfd.pattern);
#if defined USE_REGEX_PCRE
	if (pfd.compiled_regex != NULL)
		pcre_free(pfd.compiled_regex);
#elif defined USE_REGEX_POSIX
	regfree(&_re);
#endif
	return ret;
}
