/*
 * LUKS - Linux Unified Key Setup v2, token handling
 *
 * Copyright (C) 2016-2021 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2016-2021 Milan Broz
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

#include <ctype.h>
#include <dlfcn.h>
#include <assert.h>

#include "luks2_internal.h"

static struct crypt_token_handler_internal token_handlers[LUKS2_TOKENS_MAX] = {
	/* keyring builtin token */
	{
	  .version = 1,
	  .u = {
		  .v1 = { .name = LUKS2_TOKEN_KEYRING,
			  .open = keyring_open,
			  .validate = keyring_validate,
			  .dump = keyring_dump }
	       }
	}
};

#if USE_EXTERNAL_TOKENS
static void *token_dlvsym(struct crypt_device *cd,
		void *handle,
		const char *symbol,
		const char *version)
{
	char *error;
	void *sym;

	log_dbg(cd, "Loading symbol %s@%s.", symbol, version);

	sym = dlvsym(handle, symbol, version);
	error = dlerror();

	if (error)
		log_dbg(cd, "Error: %s.", error);

	return sym;
}
#endif

static bool token_validate_v1(struct crypt_device *cd, const crypt_token_handler *h)
{
	if (!h)
		return false;

	if (!h->name) {
		log_dbg(cd, "Token handler does not provide name attribute.");
		return false;
	}

	if (!h->open) {
		log_dbg(cd, "Token handler does not provide open function.");
		return false;
	}

	return true;
}

#if USE_EXTERNAL_TOKENS
static bool token_validate_v2(struct crypt_device *cd, const struct crypt_token_handler_internal *h)
{
	if (!h)
		return false;

	if (!token_validate_v1(cd, &h->u.v1))
		return false;

	if (!h->u.v2.version) {
		log_dbg(cd, "Token handler does not provide " CRYPT_TOKEN_ABI_VERSION " function.");
		return false;
	}

	return true;
}

static bool external_token_name_valid(const char *name)
{
	if (!*name || strlen(name) > LUKS2_TOKEN_NAME_MAX)
		return false;

	while (*name) {
		if (!isalnum(*name) && *name != '-' && *name != '_')
			return false;
		name++;
	}

	return true;
}
#endif

static int
crypt_token_load_external(struct crypt_device *cd, const char *name, struct crypt_token_handler_internal *ret)
{
#if USE_EXTERNAL_TOKENS
	struct crypt_token_handler_v2 *token;
	void *h;
	char buf[512];
	int r;

	if (!ret || !name)
		return -EINVAL;

	if (!external_token_name_valid(name)) {
		log_dbg(cd, "External token name (%.*s) invalid.", LUKS2_TOKEN_NAME_MAX, name);
		return -EINVAL;
	}

	token = &ret->u.v2;

	r = snprintf(buf, sizeof(buf), "libcryptsetup-token-%s.so", name);
	if (r < 0 || (size_t)r >= sizeof(buf))
		return -EINVAL;

	log_dbg(cd, "Trying to load %s.", buf);

	h = dlopen(buf, RTLD_LAZY);
	if (!h) {
		log_dbg(NULL, "%s", dlerror());
		return -EINVAL;
	}
	dlerror();

	token->name = strdup(name);
	token->open = token_dlvsym(cd, h, CRYPT_TOKEN_ABI_OPEN, CRYPT_TOKEN_ABI_VERSION1);
	token->buffer_free = token_dlvsym(cd, h, CRYPT_TOKEN_ABI_BUFFER_FREE, CRYPT_TOKEN_ABI_VERSION1);
	token->validate = token_dlvsym(cd, h, CRYPT_TOKEN_ABI_VALIDATE, CRYPT_TOKEN_ABI_VERSION1);
	token->dump = token_dlvsym(cd, h, CRYPT_TOKEN_ABI_DUMP, CRYPT_TOKEN_ABI_VERSION1);
	token->open_pin = token_dlvsym(cd, h, CRYPT_TOKEN_ABI_OPEN_PIN, CRYPT_TOKEN_ABI_VERSION1);
	token->version = token_dlvsym(cd, h, CRYPT_TOKEN_ABI_VERSION, CRYPT_TOKEN_ABI_VERSION1);

	if (!token_validate_v2(cd, ret)) {
		free(CONST_CAST(void *)token->name);
		dlclose(h);
		memset(token, 0, sizeof(*token));
		return -EINVAL;
	}

	/* Token loaded, possible error here means only debug message fail and can be ignored */
	r = snprintf(buf, sizeof(buf), "%s", token->version() ?: "");
	if (r < 0 || (size_t)r >= sizeof(buf))
		*buf = '\0';

	log_dbg(cd, "Token handler %s-%s loaded sucessfuly.", token->name, buf);

	token->dlhandle = h;
	ret->version = 2;

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int is_builtin_candidate(const char *type)
{
	return !strncmp(type, LUKS2_BUILTIN_TOKEN_PREFIX, LUKS2_BUILTIN_TOKEN_PREFIX_LEN);
}

static int crypt_token_find_free(struct crypt_device *cd, const char *name, int *index)
{
	int i;

	if (is_builtin_candidate(name)) {
		log_dbg(cd, "'" LUKS2_BUILTIN_TOKEN_PREFIX "' is reserved prefix for builtin tokens.");
		return -EINVAL;
	}

	for (i = 0; i < LUKS2_TOKENS_MAX && token_handlers[i].u.v1.name; i++) {
		if (!strcmp(token_handlers[i].u.v1.name, name)) {
			log_dbg(cd, "Keyslot handler %s is already registered.", name);
			return -EINVAL;
		}
	}

	if (i == LUKS2_TOKENS_MAX)
		return -EINVAL;

	if (index)
		*index = i;

	return 0;
}

int crypt_token_register(const crypt_token_handler *handler)
{
	int i, r;

	if (!token_validate_v1(NULL, handler))
		return -EINVAL;

	r = crypt_token_find_free(NULL, handler->name, &i);
	if (r < 0)
		return r;

	token_handlers[i].version = 1;
	token_handlers[i].u.v1 = *handler;
	return 0;
}

void crypt_token_unload_external_all(struct crypt_device *cd)
{
	int i;

	for (i = LUKS2_TOKENS_MAX - 1; i >= 0; i--) {
		if (token_handlers[i].version < 2)
			continue;

		log_dbg(cd, "Unloading %s token handler.", token_handlers[i].u.v2.name);

		free(CONST_CAST(void *)token_handlers[i].u.v2.name);

		if (dlclose(CONST_CAST(void *)token_handlers[i].u.v2.dlhandle))
			log_dbg(cd, "%s", dlerror());
	}
}

static const void
*LUKS2_token_handler_type(struct crypt_device *cd, const char *type)
{
	int i;

	for (i = 0; i < LUKS2_TOKENS_MAX && token_handlers[i].u.v1.name; i++)
		if (!strcmp(token_handlers[i].u.v1.name, type))
			return &token_handlers[i].u;

	if (i >= LUKS2_TOKENS_MAX)
		return NULL;

	if (is_builtin_candidate(type))
		return NULL;

	if (crypt_token_load_external(cd, type, &token_handlers[i]))
		return NULL;

	return &token_handlers[i].u;
}

static const void
*LUKS2_token_handler(struct crypt_device *cd, int token)
{
	struct luks2_hdr *hdr;
	json_object *jobj1, *jobj2;

	if (token < 0)
		return NULL;

	if (!(hdr = crypt_get_hdr(cd, CRYPT_LUKS2)))
		return NULL;

	if (!(jobj1 = LUKS2_get_token_jobj(hdr, token)))
		return NULL;

	if (!json_object_object_get_ex(jobj1, "type", &jobj2))
		return NULL;

	return LUKS2_token_handler_type(cd, json_object_get_string(jobj2));
}

static int LUKS2_token_find_free(struct luks2_hdr *hdr)
{
	int i;

	for (i = 0; i < LUKS2_TOKENS_MAX; i++)
		if (!LUKS2_get_token_jobj(hdr, i))
			return i;

	return -EINVAL;
}

int LUKS2_token_create(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	int token,
	const char *json,
	int commit)
{
	const crypt_token_handler *h;
	json_object *jobj_tokens, *jobj_type, *jobj;
	enum json_tokener_error jerr;
	char num[16];

	if (token == CRYPT_ANY_TOKEN) {
		if (!json)
			return -EINVAL;
		token = LUKS2_token_find_free(hdr);
	}

	if (token < 0 || token >= LUKS2_TOKENS_MAX)
		return -EINVAL;

	if (!json_object_object_get_ex(hdr->jobj, "tokens", &jobj_tokens))
		return -EINVAL;

	snprintf(num, sizeof(num), "%d", token);

	/* Remove token */
	if (!json)
		json_object_object_del(jobj_tokens, num);
	else {

		jobj = json_tokener_parse_verbose(json, &jerr);
		if (!jobj) {
			log_dbg(cd, "Token JSON parse failed.");
			return -EINVAL;
		}

		if (LUKS2_token_validate(cd, hdr->jobj, jobj, num)) {
			json_object_put(jobj);
			return -EINVAL;
		}

		json_object_object_get_ex(jobj, "type", &jobj_type);
		h = LUKS2_token_handler_type(cd, json_object_get_string(jobj_type));

		if (is_builtin_candidate(json_object_get_string(jobj_type)) && !h) {
			log_dbg(cd, "%s is builtin token candidate with missing handler",
				json_object_get_string(jobj_type));
			json_object_put(jobj);
			return -EINVAL;
		}

		if (h && h->validate && h->validate(cd, json)) {
			json_object_put(jobj);
			log_dbg(cd, "Token type %s validation failed.", h->name);
			return -EINVAL;
		}

		json_object_object_add(jobj_tokens, num, jobj);
		if (LUKS2_check_json_size(cd, hdr)) {
			log_dbg(cd, "Not enough space in header json area for new token.");
			json_object_object_del(jobj_tokens, num);
			return -ENOSPC;
		}
	}

	if (commit)
		return LUKS2_hdr_write(cd, hdr) ?: token;

	return token;
}

crypt_token_info LUKS2_token_status(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	int token,
	const char **type)
{
	const char *tmp;
	const crypt_token_handler *th;
	json_object *jobj_type, *jobj_token;

	if (token < 0 || token >= LUKS2_TOKENS_MAX)
		return CRYPT_TOKEN_INVALID;

	if (!(jobj_token = LUKS2_get_token_jobj(hdr, token)))
		return CRYPT_TOKEN_INACTIVE;

	json_object_object_get_ex(jobj_token, "type", &jobj_type);
	tmp = json_object_get_string(jobj_type);

	if ((th = LUKS2_token_handler_type(cd, tmp))) {
		if (type)
			*type = th->name;
		return is_builtin_candidate(tmp) ? CRYPT_TOKEN_INTERNAL : CRYPT_TOKEN_EXTERNAL;
	}

	if (type)
		*type = tmp;

	return is_builtin_candidate(tmp) ? CRYPT_TOKEN_INTERNAL_UNKNOWN : CRYPT_TOKEN_EXTERNAL_UNKNOWN;
}

static int LUKS2_token_open(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	int token,
	const char *pin,
	size_t pin_size,
	char **buffer,
	size_t *buffer_len,
	void *usrptr)
{
	const char *json;
	const struct crypt_token_handler_v2 *h;
	int r;

	if (!(h = LUKS2_token_handler(cd, token)))
		return -ENOENT;

	if (h->validate) {
		if (LUKS2_token_json_get(cd, hdr, token, &json))
			return -EINVAL;

		if (h->validate(cd, json)) {
			log_dbg(cd, "Token %d (%s) validation failed.", token, h->name);
			return -EINVAL;
		}
	}

	if (pin && !h->open_pin)
		r = -ENOENT;
	else if (pin)
		r = h->open_pin(cd, token, pin, pin_size, buffer, buffer_len, usrptr);
	else
		r = h->open(cd, token, buffer, buffer_len, usrptr);
	if (r < 0)
		log_dbg(cd, "Token %d (%s) open failed with %d.", token, h->name, r);

	return r;
}

static void LUKS2_token_buffer_free(struct crypt_device *cd,
		int token,
		void *buffer,
		size_t buffer_len)
{
	const crypt_token_handler *h = LUKS2_token_handler(cd, token);

	if (h && h->buffer_free)
		h->buffer_free(buffer, buffer_len);
	else {
		crypt_safe_memzero(buffer, buffer_len);
		free(buffer);
	}
}

static int LUKS2_keyslot_open_by_token(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	int token,
	int segment,
	const char *buffer,
	size_t buffer_len,
	struct volume_key **vk)
{
	const crypt_token_handler *h;
	json_object *jobj_token, *jobj_token_keyslots, *jobj;
	unsigned int num = 0;
	int i, r;

	if (!(h = LUKS2_token_handler(cd, token)))
		return -ENOENT;

	jobj_token = LUKS2_get_token_jobj(hdr, token);
	if (!jobj_token)
		return -EINVAL;

	json_object_object_get_ex(jobj_token, "keyslots", &jobj_token_keyslots);
	if (!jobj_token_keyslots)
		return -EINVAL;

	/* Try to open keyslot referenced in token */
	r = -EINVAL;
	for (i = 0; i < (int) json_object_array_length(jobj_token_keyslots) && r < 0; i++) {
		jobj = json_object_array_get_idx(jobj_token_keyslots, i);
		num = atoi(json_object_get_string(jobj));
		log_dbg(cd, "Trying to open keyslot %u with token %d (type %s).", num, token, h->name);
		r = LUKS2_keyslot_open(cd, num, segment, buffer, buffer_len, vk);
	}

	if (r < 0)
		return r;

	return num;
}

int LUKS2_token_open_and_activate(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	int token,
	const char *name,
	const char *type,
	const char *pin,
	size_t pin_size,
	uint32_t flags,
	void *usrptr)
{
	bool use_keyring;
	int keyslot, r;
	char *buffer;
	size_t buffer_len;
	json_object *jobj_token, *jobj_type;
	struct volume_key *vk = NULL;

	if (type) {
		if (!(jobj_token = LUKS2_get_token_jobj(hdr, token)))
			return -ENOENT;
		if (!json_object_object_get_ex(jobj_token, "type", &jobj_type))
			return -EINVAL;
		if (strcmp(type, json_object_get_string(jobj_type)))
			return -ENOENT;
	}

	r = LUKS2_token_open(cd, hdr, token, pin, pin_size, &buffer, &buffer_len, usrptr);
	if (r < 0)
		return r;

	r = LUKS2_keyslot_open_by_token(cd, hdr, token,
					(flags & CRYPT_ACTIVATE_ALLOW_UNBOUND_KEY) ?
					CRYPT_ANY_SEGMENT : CRYPT_DEFAULT_SEGMENT,
					buffer, buffer_len, &vk);

	LUKS2_token_buffer_free(cd, token, buffer, buffer_len);

	if (r < 0)
		return r;

	keyslot = r;

	if (!crypt_use_keyring_for_vk(cd))
		use_keyring = false;
	else
		use_keyring = ((name && !crypt_is_cipher_null(crypt_get_cipher(cd))) ||
			       (flags & CRYPT_ACTIVATE_KEYRING_KEY));

	if (use_keyring) {
		if (!(r = LUKS2_volume_key_load_in_keyring_by_keyslot(cd, hdr, vk, keyslot)))
			flags |= CRYPT_ACTIVATE_KEYRING_KEY;
	}

	if (r >= 0 && name)
		r = LUKS2_activate(cd, name, vk, flags);

	if (r < 0)
		crypt_drop_keyring_key(cd, vk);
	crypt_free_volume_key(vk);

	return r < 0 ? r : keyslot;
}

int LUKS2_token_open_and_activate_any(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	const char *name,
	const char *type,
	const char *pin,
	size_t pin_size,
	uint32_t flags,
	void *usrptr)
{
	char *buffer;
	json_object *tokens_jobj, *type_jobj;
	size_t buffer_len;
	int keyslot, token, r = -EINVAL;
	struct volume_key *vk = NULL;

	json_object_object_get_ex(hdr->jobj, "tokens", &tokens_jobj);

	json_object_object_foreach(tokens_jobj, slot, val) {
		if (type) {
			if (!json_object_object_get_ex(val, "type", &type_jobj))
				return -EINVAL;
			if (strcmp(type, json_object_get_string(type_jobj)))
				continue;
		}
		token = atoi(slot);

		r = LUKS2_token_open(cd, hdr, token, pin, pin_size, &buffer, &buffer_len, usrptr);
		if (r < 0)
			continue;

		r = LUKS2_keyslot_open_by_token(cd, hdr, token,
						(flags & CRYPT_ACTIVATE_ALLOW_UNBOUND_KEY) ?
						CRYPT_ANY_SEGMENT : CRYPT_DEFAULT_SEGMENT,
						buffer, buffer_len, &vk);
		LUKS2_token_buffer_free(cd, token, buffer, buffer_len);
		if (r >= 0)
			break;
	}

	keyslot = r;

	if (r >= 0 && (name || (flags & CRYPT_ACTIVATE_KEYRING_KEY)) && crypt_use_keyring_for_vk(cd)) {
		if (!(r = LUKS2_volume_key_load_in_keyring_by_keyslot(cd, hdr, vk, keyslot)))
			flags |= CRYPT_ACTIVATE_KEYRING_KEY;
	}

	if (r >= 0 && name)
		r = LUKS2_activate(cd, name, vk, flags);

	if (r < 0)
		crypt_drop_keyring_key(cd, vk);
	crypt_free_volume_key(vk);

	return r < 0 ? r : keyslot;
}

void LUKS2_token_dump(struct crypt_device *cd, int token)
{
	const crypt_token_handler *h;
	json_object *jobj_token;

	h = LUKS2_token_handler(cd, token);
	if (h && h->dump) {
		jobj_token = LUKS2_get_token_jobj(crypt_get_hdr(cd, CRYPT_LUKS2), token);
		if (jobj_token)
			h->dump(cd, json_object_to_json_string_ext(jobj_token,
				JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	}
}

int LUKS2_token_json_get(struct crypt_device *cd __attribute__((unused)), struct luks2_hdr *hdr,
			   int token, const char **json)
{
	json_object *jobj_token;

	jobj_token = LUKS2_get_token_jobj(hdr, token);
	if (!jobj_token)
		return -EINVAL;

	*json = json_object_to_json_string_ext(jobj_token,
		JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	return 0;
}

static int assign_one_keyslot(struct crypt_device *cd, struct luks2_hdr *hdr,
			      int token, int keyslot, int assign)
{
	json_object *jobj1, *jobj_token, *jobj_token_keyslots;
	char num[16];

	log_dbg(cd, "Keyslot %i %s token %i.", keyslot, assign ? "assigned to" : "unassigned from", token);

	jobj_token = LUKS2_get_token_jobj(hdr, token);
	if (!jobj_token)
		return -EINVAL;

	json_object_object_get_ex(jobj_token, "keyslots", &jobj_token_keyslots);
	if (!jobj_token_keyslots)
		return -EINVAL;

	snprintf(num, sizeof(num), "%d", keyslot);
	if (assign) {
		jobj1 = LUKS2_array_jobj(jobj_token_keyslots, num);
		if (!jobj1)
			json_object_array_add(jobj_token_keyslots, json_object_new_string(num));
	} else {
		jobj1 = LUKS2_array_remove(jobj_token_keyslots, num);
		if (jobj1)
			json_object_object_add(jobj_token, "keyslots", jobj1);
	}

	return 0;
}

static int assign_one_token(struct crypt_device *cd, struct luks2_hdr *hdr,
			    int keyslot, int token, int assign)
{
	json_object *jobj_keyslots;
	int r = 0;

	if (!LUKS2_get_token_jobj(hdr, token))
		return -EINVAL;

	if (keyslot == CRYPT_ANY_SLOT) {
		json_object_object_get_ex(hdr->jobj, "keyslots", &jobj_keyslots);

		json_object_object_foreach(jobj_keyslots, key, val) {
			UNUSED(val);
			r = assign_one_keyslot(cd, hdr, token, atoi(key), assign);
			if (r < 0)
				break;
		}
	} else
		r = assign_one_keyslot(cd, hdr, token, keyslot, assign);

	return r;
}

int LUKS2_token_assign(struct crypt_device *cd, struct luks2_hdr *hdr,
			int keyslot, int token, int assign, int commit)
{
	json_object *jobj_tokens;
	int r = 0;

	if (token == CRYPT_ANY_TOKEN) {
		json_object_object_get_ex(hdr->jobj, "tokens", &jobj_tokens);

		json_object_object_foreach(jobj_tokens, key, val) {
			UNUSED(val);
			r = assign_one_token(cd, hdr, keyslot, atoi(key), assign);
			if (r < 0)
				break;
		}
	} else
		r = assign_one_token(cd, hdr, keyslot, token, assign);

	if (r < 0)
		return r;

	if (commit)
		return LUKS2_hdr_write(cd, hdr) ?: token;

	return token;
}

static int token_is_assigned(struct luks2_hdr *hdr, int keyslot, int token)
{
	int i;
	json_object *jobj, *jobj_token_keyslots,
		    *jobj_token = LUKS2_get_token_jobj(hdr, token);

	if (!jobj_token)
		return -ENOENT;

	json_object_object_get_ex(jobj_token, "keyslots", &jobj_token_keyslots);

	for (i = 0; i < (int) json_object_array_length(jobj_token_keyslots); i++) {
		jobj = json_object_array_get_idx(jobj_token_keyslots, i);
		if (keyslot == atoi(json_object_get_string(jobj)))
			return 0;
	}

	return -ENOENT;
}

int LUKS2_token_is_assigned(struct crypt_device *cd __attribute__((unused)), struct luks2_hdr *hdr,
			    int keyslot, int token)
{
	if (keyslot < 0 || keyslot >= LUKS2_KEYSLOTS_MAX || token < 0 || token >= LUKS2_TOKENS_MAX)
		return -EINVAL;

	return token_is_assigned(hdr, keyslot, token);
}

int LUKS2_tokens_count(struct luks2_hdr *hdr)
{
	json_object *jobj_tokens = LUKS2_get_tokens_jobj(hdr);
	if (!jobj_tokens)
		return -EINVAL;

	return json_object_object_length(jobj_tokens);
}

int LUKS2_token_assignment_copy(struct crypt_device *cd,
			struct luks2_hdr *hdr,
			int keyslot_from,
			int keyslot_to,
			int commit)
{
	int i, r;

	if (keyslot_from < 0 || keyslot_from >= LUKS2_KEYSLOTS_MAX || keyslot_to < 0 || keyslot_to >= LUKS2_KEYSLOTS_MAX)
		return -EINVAL;

	r = LUKS2_tokens_count(hdr);
	if (r <= 0)
		return r;

	for (i = 0; i < LUKS2_TOKENS_MAX; i++) {
		if (!token_is_assigned(hdr, keyslot_from, i)) {
			if ((r = assign_one_token(cd, hdr, keyslot_to, i, 1)))
				return r;
		}
	}

	return commit ? LUKS2_hdr_write(cd, hdr) : 0;
}
