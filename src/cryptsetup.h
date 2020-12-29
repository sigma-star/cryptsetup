/*
 * cryptsetup - setup cryptographic volumes for dm-crypt
 *
 * Copyright (C) 2004 Jana Saout <jana@saout.de>
 * Copyright (C) 2004-2007 Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2009-2020 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2009-2020 Milan Broz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef CRYPTSETUP_H
#define CRYPTSETUP_H

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <popt.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "lib/nls.h"
#include "lib/utils_crypt.h"
#include "lib/utils_loop.h"
#include "lib/utils_fips.h"
#include "lib/utils_io.h"
#include "lib/utils_blkid.h"

#include "libcryptsetup.h"
#include "libcryptsetup_cli.h"
#include "lib/cli/cli_internal.h"
#include "plugin.h"

#define CONST_CAST(x) (x)(uintptr_t)
#define DEFAULT_CIPHER(type)	(DEFAULT_##type##_CIPHER "-" DEFAULT_##type##_MODE)
#define SECTOR_SIZE 512
#define MAX_SECTOR_SIZE 4096
#define ROUND_SECTOR(x) (((x) + SECTOR_SIZE - 1) / SECTOR_SIZE)

#define DEFAULT_WIPE_BLOCK	1048576 /* 1 MiB */
#define MAX_ACTIONS 16

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

/* Common tools */
void tool_log(int level, const char *msg, void *usrptr __attribute__((unused)));
void quiet_log(int level, const char *msg, void *usrptr);

int yesDialog(const char *msg, void *usrptr);
int noDialog(const char *msg, void *usrptr);
void show_status(int errcode);
const char *uuid_or_device(const char *spec);
__attribute__ ((noreturn)) \
void usage(poptContext popt_context, int exitcode, const char *error, const char *more);
void dbg_version_and_cmd(int argc, const char **argv);
int translate_errno(int r);

typedef enum { CREATED, UNLOCKED, REMOVED  } crypt_object_op;
void tools_keyslot_msg(int keyslot, crypt_object_op op);
void tools_token_msg(int token, crypt_object_op op);

extern volatile int quit;
void set_int_block(int block);
void set_int_handler(int block);
void check_signal(int *r);
int tools_signals_blocked(void);

int tools_get_key(const char *prompt,
		  char **key, size_t *key_size,
		  uint64_t keyfile_offset, size_t keyfile_size_max,
		  const char *key_file,
		  int timeout, int verify, int pwquality,
		  struct crypt_device *cd);
void tools_passphrase_msg(int r);
int tools_is_stdin(const char *key_file);
int tools_string_to_size(struct crypt_device *cd, const char *s, uint64_t *size);
int tools_is_cipher_null(const char *cipher);

struct tools_progress_params {
	uint32_t frequency;
	struct timeval start_time;
	struct timeval end_time;
	uint64_t start_offset;
	bool batch_mode;
};

int tools_wipe_progress(uint64_t size, uint64_t offset, void *usrptr);
int tools_reencrypt_progress(uint64_t size, uint64_t offset, void *usrptr);

int tools_read_json_file(struct crypt_device *cd, const char *file, char **json, size_t *json_size, bool batch_mode);
int tools_write_json_file(struct crypt_device *cd, const char *file, const char *json);

int tools_detect_signatures(const char *device, int ignore_luks, size_t *count, bool batch_mode);
int tools_wipe_all_signatures(const char *path);

int tools_lookup_crypt_device(struct crypt_device *cd, const char *type,
		const char *data_device_path, char *name, size_t name_length);

void tools_parse_arg_value(poptContext popt_context, crypt_arg_type_info type, struct tools_arg *arg, const char *popt_arg, int popt_val, bool(*needs_size_conv_fn)(unsigned arg_id));

void tools_args_free(struct tools_arg *args, size_t args_count);

void tools_check_args(const char *action, const struct tools_arg *args, size_t args_size, poptContext popt_context);

/* each utility is required to implement it */
void tools_cleanup(void);

#define FREE_AND_NULL(x) do { free(x); x = NULL; } while (0)

/* Log */
#define log_dbg(x...) crypt_logf(NULL, CRYPT_LOG_DEBUG, x)
#define log_std(x...) crypt_logf(NULL, CRYPT_LOG_NORMAL, x)
#define log_verbose(x...) crypt_logf(NULL, CRYPT_LOG_VERBOSE, x)
#define log_err(x...) crypt_logf(NULL, CRYPT_LOG_ERROR, x)

struct tools_log_params {
	bool verbose;
	bool debug;
};

/* external token plugins */

struct tools_token_handler {
	crypt_token_handle_init_func init;
	crypt_token_handle_free_func free;
	crypt_token_version_func version;

	crypt_token_create_params_func create_params;
	crypt_token_validate_create_params_func validate_create_params;
	crypt_token_create_func create;

	crypt_token_remove_params_func remove_params;
	crypt_token_validate_remove_params_func validate_remove_params;
	crypt_token_remove_func remove;

	size_t total_args_count;

	const char *type;
	char *create_desc;
	char *remove_desc;

	bool loaded;
	void *dlhandle;
	struct poptOption popt_table_plugin[3];
	struct poptOption popt_create_args[3];
	struct poptOption popt_remove_args[3];
	struct poptOption *popt_create_ref_args;
	struct poptOption *popt_create_plg_args;
	struct poptOption *popt_remove_ref_args;
	struct poptOption *popt_remove_plg_args;
	struct tools_arg *args_plugin;
};

int tools_plugin_load(const char *type,
		struct poptOption *plugin_options,
		struct tools_token_handler *token_handler,
		struct tools_arg *core_args,
		size_t core_args_len,
		struct poptOption *popt_core_options,
		void *plugin_cb,
		bool quiet);

void tools_plugin_unload(struct tools_token_handler *th);

void tools_plugin_assign_args_to_action(struct tools_token_handler *thandle,
		struct tools_arg *args,
		size_t args_len,
		const char *action);

#endif /* CRYPTSETUP_H */
