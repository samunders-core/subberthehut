/*
 * Copyright 2015 Marius Thesing
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <inttypes.h> // uint64_t / PRIx64

#include <xmlrpc-c/base.h>
#include <xmlrpc-c/client.h>
#include <glib.h> // g_base64_decode_step
#include <zlib.h>

#define STH_XMLRPC_URL         "https://api.opensubtitles.org/xml-rpc"
#define LOGIN_LANGCODE         "en"
#define LOGIN_USER_AGENT       "subberthehut v" VERSION

#define ZLIB_CHUNK             (64 * 1024)

#define STH_XMLRPC_SIZE_LIMIT  (10 * 1024 * 1024)

#define HEADER_ID              '#'
#define HEADER_MATCHED_BY_HASH 'H'
#define HEADER_LANG            "Lng"
#define HEADER_RELEASE_NAME    "Release / File Name"

#define SEP_VERTICAL           "\342\224\202"
#define SEP_HORIZONTAL         "\342\224\200"
#define SEP_CROSS              "\342\224\274"
#define SEP_UP_RIGHT           "\342\224\224"

/* __attribute__(cleanup) */
#define _cleanup_free_    __attribute__((cleanup(cleanup_free)))
#define _cleanup_fclose_  __attribute__((cleanup(cleanup_fclose)))
#define _cleanup_xmlrpc_  __attribute__((cleanup(cleanup_xmlrpc_DECREF)))

static void cleanup_free(void *p) {
	free(*(void**)p);
}

static void cleanup_fclose(FILE **p) {
	if(*p)
		fclose(*p);
}

static void cleanup_xmlrpc_DECREF(xmlrpc_value **p) {
	if(*p)
		xmlrpc_DECREF(*p);
}
/* end __attribute__(cleanup) */

static xmlrpc_env env;
static xmlrpc_client *client;

// options default values
static const char *lang = "eng";
static bool list_languages = false;
static bool force_overwrite = false;
static bool always_ask = false;
static bool never_ask = false;
static bool hash_search_only = false;
static bool name_search_only = false;
static bool same_name = false;
static int limit = 10;
static bool exit_on_fail = true;
static unsigned int quiet = 0;

struct sub_info {
	int id;
	bool matched_by_hash;
	const char *lang;
	const char *release_name;
	const char *filename;
};

static void log_err(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	putc('\n', stderr);
}

static void log_info(const char *format, ...) {
	if (quiet >= 2)
		return;

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	putchar('\n');
}

static int log_oom() {
	log_err("Out of Memory.");
	return ENOMEM;
}

/*
 * creates the 64-bit hash used for the search query.
 * copied and modified from:
 * http://trac.opensubtitles.org/projects/opensubtitles/wiki/HashSourceCodes
 */
static void get_hash_and_filesize(FILE *handle, uint64_t *hash, uint64_t *filesize) {
	fseek(handle, 0, SEEK_END);
	*filesize = ftell(handle);
	fseek(handle, 0, SEEK_SET);

	*hash = *filesize;

	for (uint64_t tmp = 0, i = 0; i < 65536 / sizeof(tmp) && fread((char *) &tmp, sizeof(tmp), 1, handle); *hash += tmp, i++);
	fseek(handle, (*filesize - 65536) > 0 ? (*filesize - 65536) : 0, SEEK_SET);
	for (uint64_t tmp = 0, i = 0; i < 65536 / sizeof(tmp) && fread((char *) &tmp, sizeof(tmp), 1, handle); *hash += tmp, i++);
}

/*
 * convenience function the get a string value from a xmlrpc struct.
 */
static const char *struct_get_string(xmlrpc_value *s, const char *key) {
	_cleanup_xmlrpc_ xmlrpc_value *xmlval = NULL;
	const char *str;

	xmlrpc_struct_find_value(&env, s, key, &xmlval);
	xmlrpc_read_string(&env, xmlval, &str);

	return str;
}

static int login(const char **token) {
	_cleanup_xmlrpc_ xmlrpc_value *result = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *token_xmlval = NULL;
	_cleanup_free_ const char *status = NULL;

	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "LogIn", &result, "(ssss)", "", "", LOGIN_LANGCODE, LOGIN_USER_AGENT);
	if (env.fault_occurred) {
		log_err("login failed: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	status = struct_get_string(result, "status");
	if (strcmp(status, "200 OK")) {
		log_err("login failed: %s", status);
		return 1;
	}

	xmlrpc_struct_find_value(&env, result, "token", &token_xmlval);
	xmlrpc_read_string(&env, token_xmlval, token);

	return 0;
}

static int search_get_results(const char *token, uint64_t hash, uint64_t filesize,
                              const char *filename, xmlrpc_value **data) {
	_cleanup_xmlrpc_ xmlrpc_value *hash_query = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *sublanguageid_xmlval = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *hash_xmlval = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *filesize_xmlval = NULL;
	_cleanup_free_ char *hash_str = NULL;
	_cleanup_free_ char *filesize_str = NULL;

	_cleanup_xmlrpc_ xmlrpc_value *name_query = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *filename_xmlval = NULL;

	_cleanup_xmlrpc_ xmlrpc_value *query_array = NULL;

	_cleanup_xmlrpc_ xmlrpc_value *limit_xmlval = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *param_struct = NULL;

	_cleanup_xmlrpc_ xmlrpc_value *result = NULL;

	query_array = xmlrpc_array_new(&env);

	// create hash-based query
	if (!name_search_only) {
		hash_query = xmlrpc_struct_new(&env);
		sublanguageid_xmlval = xmlrpc_string_new(&env, lang);
		xmlrpc_struct_set_value(&env, hash_query, "sublanguageid", sublanguageid_xmlval);
		int r = asprintf(&hash_str, "%016" PRIx64, hash);
		if (r == -1)
			return log_oom();

		hash_xmlval = xmlrpc_string_new(&env, hash_str);
		xmlrpc_struct_set_value(&env, hash_query, "moviehash", hash_xmlval);

		r = asprintf(&filesize_str, "%" PRIu64, filesize);
		if (r == -1)
			return log_oom();

		filesize_xmlval = xmlrpc_string_new(&env, filesize_str);
		xmlrpc_struct_set_value(&env, hash_query, "moviebytesize", filesize_xmlval);

		xmlrpc_array_append_item(&env, query_array, hash_query);
	}

	// create full-text query
	if (!hash_search_only) {
		name_query = xmlrpc_struct_new(&env);

		sublanguageid_xmlval = xmlrpc_string_new(&env, lang);
		xmlrpc_struct_set_value(&env, name_query, "sublanguageid", sublanguageid_xmlval);

		filename_xmlval = xmlrpc_string_new(&env, filename);
		xmlrpc_struct_set_value(&env, name_query, "query", filename_xmlval);

		xmlrpc_array_append_item(&env, query_array, name_query);
	}

	// create parameter structure (currently only for "limit")
	param_struct = xmlrpc_struct_new(&env);
	limit_xmlval = xmlrpc_int_new(&env, limit);
	xmlrpc_struct_set_value(&env, param_struct, "limit", limit_xmlval);

	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "SearchSubtitles", &result, "(sAS)", token, query_array, param_struct);
	if (env.fault_occurred) {
		log_err("query failed: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	xmlrpc_struct_read_value(&env, result, "data", data);
	if (env.fault_occurred) {
		log_err("failed to get data: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	return 0;
}

static void print_separator(int c, int digit_count) {
	for (int i = 0; i < c; i++) {
		if (i == digit_count + 1 ||
		    i == digit_count + 1 + 4 ||
		    i == digit_count + 1 + 4 + 6) {
			fputs(SEP_CROSS, stdout);
		}
		else {
			fputs(SEP_HORIZONTAL, stdout);
		}
	}
	putchar('\n');
}

static void print_table(struct sub_info *sub_infos, int n, int align_release_name) {
	// count number of digits
	int digit_count = 0;
	int n_tmp = n;
	while (n_tmp) {
		n_tmp /= 10;
		digit_count++;
	}

	// header
	putchar('\n');
	int c = printf("%-*c " SEP_VERTICAL " %c " SEP_VERTICAL " %s " SEP_VERTICAL " %-*s\n",
	               digit_count,
	               HEADER_ID,
	               HEADER_MATCHED_BY_HASH,
	               HEADER_LANG,
	               align_release_name,
	               HEADER_RELEASE_NAME);

	c -= 5;

	// separator
	print_separator(c, digit_count);

	// list
	for (int i = 0; i < n; i++) {
		printf("%-*i " SEP_VERTICAL " %c " SEP_VERTICAL " %s " SEP_VERTICAL " %-*s\n",
		       digit_count,
		       i + 1,
		       sub_infos[i].matched_by_hash ? '*' : ' ',
		       sub_infos[i].lang,
		       align_release_name,
		       sub_infos[i].release_name);

		printf("%-*s " SEP_VERTICAL "   " SEP_VERTICAL "     " SEP_VERTICAL " " SEP_UP_RIGHT "%s\n",
		       digit_count,
		       "",
		       sub_infos[i].filename);

		if (i != n - 1)
			print_separator(c, digit_count);
	}
	putchar('\n');
}

static const char *get_sub_path(const char *filepath, const char *sub_filename) {
	char *sub_filepath;

	if (same_name) {
		const char *sub_ext = strrchr(sub_filename, '.');
		if (!sub_ext) {
			log_err("warning: subtitle filename from the OpenSubtitles.org "
			        "database has no file extension, assuming .srt.");
			sub_ext = ".srt";
		}
		const char *lastdot = strrchr(filepath, '.');
		int index;
		if (!lastdot)
			index = strlen(filepath) - 1;
		else
			index = (lastdot - filepath);

		sub_filepath = malloc(index + 1 + strlen(sub_ext) + 1);
		if (!sub_filepath)
			return NULL;

		strncpy(sub_filepath, filepath, index);
		sub_filepath[index] = '\0';
		strcat(sub_filepath, sub_ext);
	} else {
		const char *lastslash = strrchr(filepath, '/');
		if (!lastslash) {
			sub_filepath = strdup(sub_filename);
			if (!sub_filepath)
				return NULL;
		} else {
			int index = (lastslash - filepath);
			sub_filepath = malloc(index + 1 + strlen(sub_filename) + 1);
			if (!sub_filepath)
				return NULL;

			strncpy(sub_filepath, filepath, index + 1);
			sub_filepath[index + 1] = '\0';
			strcat(sub_filepath, sub_filename);
		}
	}
	return sub_filepath;
}

static int select_1_out_of(int n) {
	_cleanup_free_ char *line = NULL;
	size_t len = 0;
	char *endptr = NULL;
	int sel = 0;
	do {
		printf("Choose subtitle [1..%i], q/Q to quit: ", n);
		fflush(stdout);
		if (getline(&line, &len, stdin) == -1) {
			return -EIO;
		} else if (line && ('q' == line[0] || 'Q' == line[0])) {
			return -1;
		}
		sel = strtol(line, &endptr, 10);
	} while (*endptr != '\n' || sel < 1 || sel > n);
	return sel;
}

static int download_chosen_results(const char *filepath, const char *token, xmlrpc_value *results, int n) {
	int r = 0;
	struct sub_info sub_infos[n];

	int sel = 0; // selected list item

	/* Make the values in the "Release / File Name" column
	 * at least as long as the header title itself. */
	int align_release_name = strlen(HEADER_RELEASE_NAME);

	for (int i = 0; i < n; i++) {
		_cleanup_xmlrpc_ xmlrpc_value *oneresult = NULL;
		xmlrpc_array_read_item(&env, results, i, &oneresult);

		// dear OpenSubtitles.org, why are these IDs provided as strings?
		_cleanup_free_ const char *sub_id_str = struct_get_string(oneresult, "IDSubtitleFile");
		_cleanup_free_ const char *matched_by_str = struct_get_string(oneresult, "MatchedBy");

		sub_infos[i].id = strtol(sub_id_str, NULL, 10);
		sub_infos[i].matched_by_hash = strcmp(matched_by_str, "moviehash") == 0;
		sub_infos[i].lang = struct_get_string(oneresult, "SubLanguageID");
		sub_infos[i].release_name = struct_get_string(oneresult, "MovieReleaseName");
		sub_infos[i].filename = struct_get_string(oneresult, "SubFileName");

		// select first hash match if one exists
		if (sub_infos[i].matched_by_hash && sel == 0)
			sel = i + 1;

		int s = strlen(sub_infos[i].release_name);
		if (s > align_release_name)
			align_release_name = s;

		s = strlen(sub_infos[i].filename);
		if (s > align_release_name)
			align_release_name = s;
	}

	if (never_ask && sel == 0)
		sel = 1;

	while (sel == 0 || always_ask) {
		print_table(sub_infos, n, align_release_name);
		// let user choose the subtitle to download
		sel = select_1_out_of(n);
		if (sel <= 0) {
			r = -sel;
			goto finish;
		}
		_cleanup_free_ const char *sub_filepath = get_sub_path(filepath, sub_infos[sel - 1].filename);
		if (!sub_filepath) {
			r = log_oom();
			goto finish;
		}
		log_info("downloading to %s ...", sub_filepath);
		r = sub_download(token, sub_infos[sel - 1].id, sub_filepath);
		if (r != 0 || n == 1)
			goto finish;
		sel = 0;
	}

	if (!quiet)
		print_table(sub_infos, n, align_release_name);

	_cleanup_free_ const char *sub_filepath = get_sub_path(filepath, sub_infos[sel - 1].filename);
	if (!sub_filepath) {
		r = log_oom();
		goto finish;
	}
	log_info("downloading to %s ...", sub_filepath);
	r = sub_download(token, sub_infos[sel - 1].id, sub_filepath);

finish:
	// __attribute__(cleanup) can't be used in structs, let alone arrays
	for (int i = 0; i < n; i++) {
		free((void *)sub_infos[i].lang);
		free((void *)sub_infos[i].release_name);
		free((void *)sub_infos[i].filename);
	}

	return r;
}

static int sub_download(const char *token, int sub_id, const char *file_path) {
	_cleanup_xmlrpc_ xmlrpc_value *sub_id_xmlval = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *query_array = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *result = NULL;;

	_cleanup_xmlrpc_ xmlrpc_value *data = NULL;       // result -> data
	_cleanup_xmlrpc_ xmlrpc_value *data_0 = NULL;     // result -> data[0]
	_cleanup_xmlrpc_ xmlrpc_value *data_0_sub = NULL; // result -> data[0][data]

	_cleanup_free_ const char *sub_base64 = NULL;	  // the subtitle, gzipped and base64 encoded

	// zlib stuff, see also http://zlib.net/zlib_how.html
	int z_ret;
	z_stream z_strm;
	unsigned char z_out[ZLIB_CHUNK];
	unsigned char z_in[ZLIB_CHUNK];
	z_strm.zalloc = Z_NULL;
	z_strm.zfree = Z_NULL;
	z_strm.opaque = Z_NULL;
	z_strm.avail_in = 0;
	z_strm.next_in = Z_NULL;

	_cleanup_fclose_ FILE *f = NULL;
	int r = 0;

	// check if file already exists
	if (access(file_path, F_OK) == 0) {
		if (force_overwrite) {
			log_info("file already exists, overwriting.");
		} else {
			log_err("file already exists, aborting. Use -f to force an overwrite.");
			return EEXIST;
		}
	}

	// download
	sub_id_xmlval = xmlrpc_int_new(&env, sub_id);

	query_array = xmlrpc_array_new(&env);
	xmlrpc_array_append_item(&env, query_array, sub_id_xmlval);

	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "DownloadSubtitles", &result, "(sA)", token, query_array);
	if (env.fault_occurred) {
		log_err("query failed: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	// get base64 encoded data
	xmlrpc_struct_find_value(&env, result, "data", &data);
	xmlrpc_array_read_item(&env, data, 0, &data_0);
	xmlrpc_struct_find_value(&env, data_0, "data", &data_0_sub);
	xmlrpc_read_string(&env, data_0_sub, &sub_base64);

	// decode and decompress to file
	f = fopen(file_path, "w+");
	if (!f) {
		perror("failed to open output file");
		return errno;
	}

	// 16+MAX_WBITS is needed for gzip support
	z_ret = inflateInit2(&z_strm, 16 + MAX_WBITS);
	if (z_ret != Z_OK) {
		log_err("failed to init zlib (%i)", z_ret);
		return z_ret;
	}

	int b64_state = 0;
	unsigned int b64_save = 0;
	unsigned int b64_offset = 0;
	do {
		// write decoded data to z_in
		z_strm.avail_in = g_base64_decode_step(&sub_base64[b64_offset], ZLIB_CHUNK, z_in, &b64_state, &b64_save);
		b64_offset += z_strm.avail_in * 4 / 3; //  base64 encodes 3 bytes in 4 chars
		if (z_strm.avail_in == 0)
			break;

		z_strm.next_in = z_in;

		// decompress decoded data from z_in to z_out
		do {
			z_strm.avail_out = ZLIB_CHUNK;
			z_strm.next_out = z_out;
			z_ret = inflate(&z_strm, Z_NO_FLUSH);

			switch (z_ret) {
			case Z_NEED_DICT:
				z_ret = Z_DATA_ERROR;
				// fallthrough
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				r = z_ret;
				log_err("zlib error: %s (%d)", z_strm.msg, z_ret);
				goto finish;
			}
			// write decompressed data from z_out to file
			unsigned int have = ZLIB_CHUNK - z_strm.avail_out;

			size_t written = fwrite(z_out, 1, have, f);
			if (written != have) {
				log_err("failed to write file: %m");
				r = errno;
				goto finish;
			}
		} while (z_strm.avail_out == 0);
	} while (z_ret != Z_STREAM_END);

finish:
	inflateEnd(&z_strm);

	return r;
}

static void show_usage() {
	puts("Usage: subberthehut [options] <file>...\n\n"

	     "OpenSubtitles.org downloader.\n\n"

	     "subberthehut can do a hash-based and a name-based search.\n"
	     "On a hash-based search, subberthehut will generate a hash from the specified\n"
	     "video file and use this to search for appropriate subtitles.\n"
	     "Any results from this hash-based search should be compatible\n"
	     "with the video file. Therefore subberthehut will, by default, automatically\n"
	     "download the first subtitle from these search results.\n"
	     "In case the hash-based search returns no results, subberthehut will also\n"
	     "do a name-based search, meaning the OpenSubtitles.org database\n"
	     "will be searched with the filename of the specified file. The results\n"
	     "from this search are not guaranteed to be compatible with the video\n"
	     "file. Therefore subberthehut will, by default, ask the user which subtitle to\n"
	     "download.\n"
	     "Results from the hash-based search are marked with an asterisk (*)\n"
	     "in the 'H' column.\n\n"

	     "Options:\n"
	     " -h, --help              Show this help and exit.\n"
	     "\n"
	     " -v, --version           Show version information and exit.\n"
	     "\n"
	     " -l, --lang <languages>  Comma-separated list of languages to search for,\n"
	     "                         e.g. 'eng,ger'. Use 'all' to search for all\n"
	     "                         languages. Default is 'eng'. Use --list-languages\n"
	     "                         to list all available languages.\n"
	     "\n"
	     " -L, --list-languages    List all available languages and exit.\n"
	     "\n"
	     " -a, --always-ask        Always ask which subtitle to download, even\n"
	     "                         when there are hash-based results.\n"
	     "\n"
	     " -n, --never-ask         Never ask which subtitle to download, even\n"
	     "                         when there are only name-based results.\n"
	     "                         When this option is specified, the first\n"
	     "                         search result will be downloaded.\n"
	     "\n"
	     " -f, --force             Overwrite output file if it already exists.\n"
	     "\n"
	     " -o, --hash-search-only  Only do a hash-based search.\n"
	     "\n"
	     " -O, --name-search-only  Only do a name-based search. This is useful in\n"
	     "                         case of false positives from the hash-based search.\n"
	     "\n"
	     " -s, --same-name         Download the subtitle to the same filename as the\n"
	     "                         original file, only replacing the file extension.\n"
	     "\n"
	     " -t, --limit <number>    Limits the number of returned results. The default is 10.\n"
	     "\n"
	     " -e, --no-exit-on-fail   By default, subberthehut will exit immediately if\n"
	     "                         multiple files are passed and it fails to download\n"
	     "                         a subtitle for one them. When this option is passed,\n"
	     "                         subberthehut will process the next file(s) regardless.\n"
	     "\n"
	     " -q, --quiet             Don't print the table if the user doesn't have to be\n"
	     "                         asked which subtitle to download. Pass this option twice\n"
	     "                         to suppress anything but warnings and error messages.\n");
}

static void show_version() {
	puts("subberthehut " VERSION "\n"
	     "https://github.com/samunders-core/subberthehut/");
}

static int process_file(const char *filepath, const char *token) {
	_cleanup_fclose_ FILE *f = NULL;
	uint64_t hash = 0, filesize = 0;

	_cleanup_xmlrpc_ xmlrpc_value *results = NULL;

	_cleanup_free_ const char *sub_filename = NULL;
	_cleanup_free_ const char *sub_filepath = NULL;

	int r = 0;

	// get hash/filesize
	if (!name_search_only) {
		f = fopen(filepath, "r");
		if (!f) {
			log_err("failed to open %s: %m", filepath);
			return errno;
		}

		get_hash_and_filesize(f, &hash, &filesize);
	}

	const char *filename = strrchr(filepath, '/');
	if (filename)
		filename++; // skip '/'
	else
		filename = filepath;

	log_info("searching for %s...", filename);

	r = search_get_results(token, hash, filesize, filename, &results);
	if (r != 0)
		return r;

	int results_length = xmlrpc_array_size(&env, results);
	if (env.fault_occurred) {
		log_err("failed to get array size: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	if (results_length == 0) {
		log_err("no results.");
		return 1;
	}

	return download_chosen_results(filepath, token, results, results_length);
}

static int list_sub_languages() {
	_cleanup_xmlrpc_ xmlrpc_value *result = NULL;
	_cleanup_xmlrpc_ xmlrpc_value *languages = NULL;

	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "GetSubLanguages", &result, "()");
	if (env.fault_occurred) {
		log_err("failed to download languages: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	xmlrpc_struct_read_value(&env, result, "data", &languages);
	if (env.fault_occurred) {
		log_err("failed to get data: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	int n = xmlrpc_array_size(&env, languages);
	if (env.fault_occurred) {
		log_err("failed to get array size: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	for (int i = 0; i < n; i++) {
		_cleanup_xmlrpc_ xmlrpc_value *language = NULL;
		xmlrpc_array_read_item(&env, languages, i, &language);

		_cleanup_free_ const char *lang_id = struct_get_string(language, "SubLanguageID");
		_cleanup_free_ const char *lang_name = struct_get_string(language, "LanguageName");

		printf("%s - %s\n", lang_id, lang_name);
	}

	return 0;
}

int main(int argc, char *argv[]) {
	_cleanup_free_ const char *token = NULL;

	int r = EXIT_SUCCESS;

	// parse options
	const struct option opts[] = {
		{"help", no_argument, NULL, 'h'},
		{"lang", required_argument, NULL, 'l'},
		{"list-languages", no_argument, NULL, 'L'},
		{"always-ask", no_argument, NULL, 'a'},
		{"never-ask", no_argument, NULL, 'n'},
		{"force", no_argument, NULL, 'f'},
		{"hash-search-only", no_argument, NULL, 'o'},
		{"name-search-only", no_argument, NULL, 'O'},
		{"same-name", no_argument, NULL, 's'},
		{"limit", required_argument, NULL, 't'},
		{"no-exit-on-fail", no_argument, NULL, 'e'},
		{"quiet", no_argument, NULL, 'q'},
		{"version", no_argument, NULL, 'v'},
		{0, 0, 0, 0}
	};

	int c;
	while ((c = getopt_long(argc, argv, "hl:LanfoOst:eqv", opts, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_usage();
			return EXIT_SUCCESS;

		case 'l':
			lang = optarg;
			break;

		case 'L':
			list_languages = true;
			break;

		case 'a':
			always_ask = true;
			break;

		case 'n':
			never_ask = true;
			break;

		case 'f':
			force_overwrite = true;
			break;

		case 'o':
			hash_search_only = true;
			name_search_only = false;
			break;

		case 'O':
			name_search_only = true;
			hash_search_only = false;
			break;

		case 's':
			same_name = true;
			break;

		case 't':
		{
			char *endptr = NULL;
			limit = strtol(optarg, &endptr, 10);

			if (*endptr != '\0' || limit < 1) {
				log_err("invalid limit: %s", optarg);
				return EXIT_FAILURE;
			}
			break;
		}

		case 'e':
			exit_on_fail = false;
			break;

		case 'q':
			quiet++;
			break;

		case 'v':
			show_version();
			return EXIT_SUCCESS;

		default:
			return EXIT_FAILURE;
		}
	}

	// check if user has specified at least one file (except for listing languages)
	if (argc - optind < 1 && !list_languages) {
		show_usage();
		return EXIT_FAILURE;
	}

	// xmlrpc init
	xmlrpc_env_init(&env);
	xmlrpc_client_setup_global_const(&env);
	// make sure the library doesn't complain about too much data
	xmlrpc_limit_set(XMLRPC_XML_SIZE_LIMIT_ID, STH_XMLRPC_SIZE_LIMIT);

	xmlrpc_client_create(&env, XMLRPC_CLIENT_NO_FLAGS, "subberthehut", VERSION, NULL, 0, &client);
	if (env.fault_occurred) {
		log_err("failed to init xmlrpc client: %s (%d)", env.fault_string, env.fault_code);
		r = env.fault_code;
		goto finish;
	}

	// login
	r = login(&token);
	if (r != 0)
		goto finish;

	// only list the languages and exit
	if (list_languages) {
		r = list_sub_languages();
		goto finish;
	}

	// process files
	for (int i = optind; i < argc; i++) {
		char *filepath = argv[i];
		r = process_file(filepath, token);
		if (r != 0 && exit_on_fail)
			goto finish;
	}

finish:
	xmlrpc_env_clean(&env);
	xmlrpc_client_destroy(client);
	xmlrpc_client_teardown_global_const();

	return r;
}
