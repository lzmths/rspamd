/*
 * Copyright (c) 2009-2012, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/***MODULE:surbl
 * rspamd module that implements SURBL url checking
 *
 * Allowed options:
 * - weight (integer): weight of symbol
 * Redirecotor options:
 * - redirector (string): address of http redirector utility in format "host:port"
 * - redirector_connect_timeout (seconds): redirector connect timeout (default: 1s)
 * - redirector_read_timeout (seconds): timeout for reading data (default: 5s)
 * - redirector_hosts_map (map string): map that contains domains to check with redirector
 * Surbl options:
 * - exceptions (map string): map of domains that should be checked via surbl using 3 (e.g. somehost.domain.com)
 *   components of domain name instead of normal 2 (e.g. domain.com)
 * - whitelist (map string): map of domains that should be whitelisted for surbl checks
 * - max_urls (integer): maximum allowed number of urls in message to be checked
 * - suffix (string): surbl address (for example insecure-bl.rambler.ru), may contain %b if bits are used (read documentation about it)
 * - bit (string): describes a prefix for a single bit
 */

#include <rdns.h>
#include "config.h"
#include "libmime/message.h"
#include "libutil/map.h"
#include "rspamd.h"
#include "surbl.h"
#include "utlist.h"
#include "unix-std.h"

static struct surbl_ctx *surbl_module_ctx = NULL;

static void surbl_test_url (struct rspamd_task *task, void *user_data);
static void surbl_dns_callback (struct rdns_reply *reply, gpointer arg);
static void surbl_dns_ip_callback (struct rdns_reply *reply, gpointer arg);
static void process_dns_results (struct rspamd_task *task,
	struct suffix_item *suffix, gchar *url, guint32 addr);


#define NO_REGEXP (gpointer) - 1

#define SURBL_ERROR surbl_error_quark ()
#define WHITELIST_ERROR 0
#define CONVERSION_ERROR 1
#define DUPLICATE_ERROR 1
GQuark
surbl_error_quark (void)
{
	return g_quark_from_static_string ("surbl-error-quark");
}

/* Initialization */
gint surbl_module_init (struct rspamd_config *cfg, struct module_ctx **ctx);
gint surbl_module_config (struct rspamd_config *cfg);
gint surbl_module_reconfig (struct rspamd_config *cfg);

module_t surbl_module = {
	"surbl",
	surbl_module_init,
	surbl_module_config,
	surbl_module_reconfig,
	NULL
};

static void
exception_insert (gpointer st, gconstpointer key, gpointer value)
{
	GHashTable **t = st;
	gint level = 0;
	const gchar *p = key;
	rspamd_ftok_t *val;

	while (*p) {
		if (*p == '.') {
			level++;
		}
		p++;
	}
	if (level >= MAX_LEVELS) {
		msg_err ("invalid domain in exceptions list: %s, levels: %d",
			(gchar *)key,
			level);
		return;
	}

	val = g_malloc (sizeof (rspamd_ftok_t));
	val->begin = key;
	val->len = strlen (key);
	if (t[level] == NULL) {
		t[level] = g_hash_table_new_full (rspamd_ftok_icase_hash,
				rspamd_ftok_icase_equal,
				g_free,
				NULL);
	}
	g_hash_table_insert (t[level], val, value);
}

static gchar *
read_exceptions_list (rspamd_mempool_t * pool,
	gchar * chunk,
	gint len,
	struct map_cb_data *data)
{
	if (data->cur_data == NULL) {
		data->cur_data = rspamd_mempool_alloc0 (pool,
				sizeof (GHashTable *) * MAX_LEVELS);
	}
	return rspamd_parse_abstract_list (pool,
			   chunk,
			   len,
			   data,
			   (insert_func) exception_insert);
}

static void
fin_exceptions_list (rspamd_mempool_t * pool, struct map_cb_data *data)
{
	GHashTable **t;
	gint i;

	if (data->prev_data) {
		t = data->prev_data;
		for (i = 0; i < MAX_LEVELS; i++) {
			if (t[i] != NULL) {
				g_hash_table_destroy (t[i]);
			}
		}
	}
}

struct rspamd_redirector_map_cb {
	GHashTable *re_hash;
	GArray *patterns;
};

static void
redirector_insert (gpointer st, gconstpointer key, gpointer value)
{
	struct rspamd_redirector_map_cb *cbdata = st;
	GHashTable *t;
	const gchar *p = key, *begin = key;
	gchar *pattern;
	ac_trie_pat_t pat;
	rspamd_regexp_t *re = NO_REGEXP;
	GError *err = NULL;

	t = cbdata->re_hash;
	while (*p && !g_ascii_isspace (*p)) {
		p++;
	}

	pat.len = p - begin;
	pattern = g_malloc (pat.len + 1);
	rspamd_strlcpy (pattern, begin, pat.len + 1);
	pat.ptr = pattern;

	g_array_append_val (cbdata->patterns, pat);

	if (g_ascii_isspace (*p)) {
		while (g_ascii_isspace (*p) && *p) {
			p++;
		}
		if (*p) {
			re = rspamd_regexp_new (p,
					"ir",
					&err);
			if (re == NULL) {
				msg_warn ("could not read regexp: %e while reading regexp %s",
					err,
					p);
				g_error_free (err);
				re = NO_REGEXP;
			}
			else {
				g_hash_table_insert (t, pattern, re);
			}
		}
	}
}

static void
redirector_item_free (gpointer p)
{
	rspamd_regexp_t *re;
	if (p != NULL && p != NO_REGEXP) {
		re = (rspamd_regexp_t *)p;
		rspamd_regexp_unref (re);
	}
}

static gchar *
read_redirectors_list (rspamd_mempool_t * pool,
	gchar * chunk,
	gint len,
	struct map_cb_data *data)
{
	struct rspamd_redirector_map_cb *cbdata;

	if (data->cur_data == NULL) {
		cbdata = g_slice_alloc0 (sizeof (*cbdata));
		cbdata->patterns = g_array_sized_new (FALSE, FALSE,
				sizeof (ac_trie_pat_t), 32);

		cbdata->re_hash = g_hash_table_new_full (rspamd_strcase_hash,
				rspamd_strcase_equal,
				NULL,
				redirector_item_free);
		data->cur_data = cbdata;
	}

	return rspamd_parse_abstract_list (pool,
			   chunk,
			   len,
			   data,
			   (insert_func) redirector_insert);
}

void
fin_redirectors_list (rspamd_mempool_t * pool, struct map_cb_data *data)
{
	struct rspamd_redirector_map_cb *cbdata;
	guint i;
	ac_trie_pat_t *pat;

	if (data->prev_data) {
		cbdata = data->prev_data;

		for (i = 0; i < cbdata->patterns->len; i ++) {
			pat = &g_array_index (cbdata->patterns, ac_trie_pat_t, i);
			g_free ((gpointer)pat->ptr);
		}

		g_hash_table_unref (cbdata->re_hash);
		g_array_free (cbdata->patterns, TRUE);
		acism_destroy (surbl_module_ctx->redirector_trie);
		g_slice_free1 (sizeof (*cbdata), cbdata);
	}

	cbdata = data->cur_data;
	surbl_module_ctx->redirector_ptrs = cbdata->patterns;
	surbl_module_ctx->redirector_hosts = cbdata->re_hash;
	surbl_module_ctx->redirector_trie = acism_create (
			(const ac_trie_pat_t *)cbdata->patterns->data,
			cbdata->patterns->len);
	g_assert (surbl_module_ctx->redirector_trie != NULL);
}

gint
surbl_module_init (struct rspamd_config *cfg, struct module_ctx **ctx)
{
	surbl_module_ctx = g_malloc0 (sizeof (struct surbl_ctx));

	surbl_module_ctx->use_redirector = 0;
	surbl_module_ctx->suffixes = NULL;
	surbl_module_ctx->surbl_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), NULL);

	surbl_module_ctx->tld2_file = NULL;
	surbl_module_ctx->whitelist_file = NULL;
	surbl_module_ctx->redirectors = NULL;
	surbl_module_ctx->whitelist = g_hash_table_new (rspamd_strcase_hash,
			rspamd_strcase_equal);
	/* Zero exceptions hashes */
	surbl_module_ctx->exceptions = rspamd_mempool_alloc0 (
		surbl_module_ctx->surbl_pool,
		MAX_LEVELS * sizeof (GHashTable *));
	/* Register destructors */
	rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
		(rspamd_mempool_destruct_t) g_hash_table_destroy,
		surbl_module_ctx->whitelist);

	*ctx = (struct module_ctx *)surbl_module_ctx;

	return 0;
}

/*
 * Register virtual symbols for suffixes with bit wildcard
 */
static void
register_bit_symbols (struct rspamd_config *cfg, struct suffix_item *suffix,
		gint parent_id)
{
	guint i;
	GHashTableIter it;
	struct surbl_bit_item *bit;
	gpointer k, v;

	if (suffix->ips != NULL) {
		g_hash_table_iter_init (&it, suffix->ips);

		while (g_hash_table_iter_next (&it, &k, &v)) {
			bit = v;
			rspamd_symbols_cache_add_symbol (cfg->cache, bit->symbol,
					0, NULL, NULL,
					SYMBOL_TYPE_VIRTUAL, parent_id);
			msg_debug_config ("bit: %d", bit->bit);
		}
	}
	else if (suffix->bits != NULL) {
		for (i = 0; i < suffix->bits->len; i++) {
			bit = &g_array_index (suffix->bits, struct surbl_bit_item, i);
			rspamd_symbols_cache_add_symbol (cfg->cache, bit->symbol,
					0, NULL, NULL,
					SYMBOL_TYPE_VIRTUAL, parent_id);
		}
	}
	else {
		rspamd_symbols_cache_add_symbol (cfg->cache, suffix->symbol,
				0, NULL, NULL,
				SYMBOL_TYPE_VIRTUAL, parent_id);
	}
}

gint
surbl_module_config (struct rspamd_config *cfg)
{
	GList *cur_opt;
	struct suffix_item *new_suffix, *cur_suffix = NULL;
	struct surbl_bit_item *new_bit;
	const ucl_object_t *value, *cur, *cur_rule, *cur_bit;
	ucl_object_iter_t it = NULL;
	const gchar *redir_val, *ip_val;
	guint32 bit;
	gint cb_id, nrules = 0;
	gboolean has_subsymbols;

	if (!rspamd_config_is_module_enabled (cfg, "surbl")) {
		return TRUE;
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl", "redirector")) != NULL) {
		surbl_module_ctx->redirectors = rspamd_upstreams_create (cfg->ups_ctx);
		rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
				(rspamd_mempool_destruct_t)rspamd_upstreams_destroy,
				surbl_module_ctx->redirectors);
		LL_FOREACH (value, cur)
		{
			redir_val = ucl_obj_tostring (cur);
			if (rspamd_upstreams_add_upstream (surbl_module_ctx->redirectors,
					redir_val, 80, NULL)) {
				surbl_module_ctx->use_redirector = TRUE;
			}
		}
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl",
		"redirector_symbol")) != NULL) {
		surbl_module_ctx->redirector_symbol = ucl_obj_tostring (value);
		rspamd_symbols_cache_add_symbol (cfg->cache,
			surbl_module_ctx->redirector_symbol,
			0, NULL, NULL, SYMBOL_TYPE_COMPOSITE, -1);
	}
	else {
		surbl_module_ctx->redirector_symbol = NULL;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl", "weight")) != NULL) {
		surbl_module_ctx->weight = ucl_obj_toint (value);
	}
	else {
		surbl_module_ctx->weight = DEFAULT_SURBL_WEIGHT;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl", "url_expire")) != NULL) {
		surbl_module_ctx->url_expire = ucl_obj_todouble (value);
	}
	else {
		surbl_module_ctx->url_expire = DEFAULT_SURBL_URL_EXPIRE;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl",
		"redirector_connect_timeout")) != NULL) {
		surbl_module_ctx->connect_timeout = ucl_obj_todouble (value);
	}
	else {
		surbl_module_ctx->connect_timeout = DEFAULT_REDIRECTOR_CONNECT_TIMEOUT;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl",
		"redirector_read_timeout")) != NULL) {
		surbl_module_ctx->read_timeout = ucl_obj_todouble (value);
	}
	else {
		surbl_module_ctx->read_timeout = DEFAULT_REDIRECTOR_READ_TIMEOUT;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl",
		"redirector_hosts_map")) != NULL) {
		rspamd_map_add (cfg, ucl_obj_tostring (
				value),
			"SURBL redirectors list", read_redirectors_list, fin_redirectors_list,
			(void **)&surbl_module_ctx->redirector_map_data);
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl", "max_urls")) != NULL) {
		surbl_module_ctx->max_urls = ucl_obj_toint (value);
	}
	else {
		surbl_module_ctx->max_urls = DEFAULT_SURBL_MAX_URLS;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl", "exceptions")) != NULL) {
		if (rspamd_map_add (cfg, ucl_obj_tostring (value),
			"SURBL exceptions list", read_exceptions_list, fin_exceptions_list,
			(void **)&surbl_module_ctx->exceptions)) {
			surbl_module_ctx->tld2_file = rspamd_mempool_strdup (
				surbl_module_ctx->surbl_pool,
				ucl_obj_tostring (value) + sizeof ("file://") - 1);
		}
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "surbl", "whitelist")) != NULL) {
		if (rspamd_map_add (cfg, ucl_obj_tostring (value),
			"SURBL whitelist", rspamd_hosts_read, rspamd_hosts_fin,
			(void **)&surbl_module_ctx->whitelist)) {
			surbl_module_ctx->whitelist_file = rspamd_mempool_strdup (
				surbl_module_ctx->surbl_pool,
				ucl_obj_tostring (value) + sizeof ("file://") - 1);
		}
	}

	value = rspamd_config_get_module_opt (cfg, "surbl", "rule");
	if (value != NULL && value->type == UCL_OBJECT) {
		LL_FOREACH (value, cur_rule)
		{
			cur = ucl_obj_get_key (cur_rule, "suffix");
			if (cur == NULL) {
				msg_err_config ("surbl rule must have explicit symbol "
						"definition");
				continue;
			}
			new_suffix = rspamd_mempool_alloc0 (surbl_module_ctx->surbl_pool,
					sizeof (struct suffix_item));
			new_suffix->suffix = rspamd_mempool_strdup (
				surbl_module_ctx->surbl_pool,
				ucl_obj_tostring (cur));
			new_suffix->options = 0;
			new_suffix->bits = g_array_new (FALSE, FALSE,
					sizeof (struct surbl_bit_item));
			rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
					(rspamd_mempool_destruct_t)rspamd_array_free_hard,
					new_suffix->bits);

			cur = ucl_obj_get_key (cur_rule, "symbol");
			if (cur == NULL) {
				msg_warn_config (
					"surbl rule for suffix %s lacks symbol, using %s as symbol",
					new_suffix->suffix,
					DEFAULT_SURBL_SYMBOL);
				new_suffix->symbol = rspamd_mempool_strdup (
					surbl_module_ctx->surbl_pool,
					DEFAULT_SURBL_SYMBOL);
			}
			else {
				new_suffix->symbol = rspamd_mempool_strdup (
					surbl_module_ctx->surbl_pool,
					ucl_obj_tostring (cur));
			}

			cur = ucl_obj_get_key (cur_rule, "options");
			if (cur != NULL && cur->type == UCL_STRING) {
				if (strstr (ucl_obj_tostring (cur), "noip") != NULL) {
					new_suffix->options |= SURBL_OPTION_NOIP;
				}
			}

			cur = ucl_obj_get_key (cur_rule, "no_ip");
			if (cur != NULL && cur->type == UCL_STRING) {
				if (ucl_object_toboolean (cur)) {
					new_suffix->options |= SURBL_OPTION_NOIP;
				}
			}
			cur = ucl_obj_get_key (cur_rule, "resolve_ip");
			if (cur != NULL && cur->type == UCL_BOOLEAN) {
				if (ucl_object_toboolean (cur)) {
					new_suffix->options |= SURBL_OPTION_RESOLVEIP;
				}
			}

			if ((new_suffix->options & (SURBL_OPTION_RESOLVEIP|SURBL_OPTION_NOIP)) ==
					(SURBL_OPTION_NOIP|SURBL_OPTION_RESOLVEIP)) {
				/* Mutually exclusive options */
				msg_err_config ("options noip and resolve_ip are "
						"mutually exclusive for suffix %s", new_suffix->suffix);
			}

			cb_id = rspamd_symbols_cache_add_symbol (cfg->cache,
					"SURBL_CALLBACK",
					0,
					surbl_test_url,
					new_suffix,
					SYMBOL_TYPE_CALLBACK,
					-1);
			nrules++;
			new_suffix->callback_id = cb_id;
			has_subsymbols = FALSE;

			cur = ucl_obj_get_key (cur_rule, "bits");
			if (cur != NULL && cur->type == UCL_OBJECT) {
				it = NULL;
				while ((cur_bit =
					ucl_iterate_object (cur, &it, true)) != NULL) {
					if (ucl_object_key (cur_bit) != NULL && cur_bit->type ==
						UCL_INT) {
						gchar *p;

						bit = ucl_obj_toint (cur_bit);
						new_bit = rspamd_mempool_alloc (
							surbl_module_ctx->surbl_pool,
							sizeof (struct surbl_bit_item));
						new_bit->bit = bit;
						new_bit->symbol = rspamd_mempool_strdup (
							surbl_module_ctx->surbl_pool,
							ucl_object_key (cur_bit));
						/* Convert to uppercase */
						p = new_bit->symbol;
						while (*p) {
							*p = g_ascii_toupper (*p);
							p ++;
						}

						msg_debug_config ("add new bit suffix: %d with symbol: %s",
							(gint)new_bit->bit, new_bit->symbol);
						g_array_append_val (new_suffix->bits, *new_bit);
						has_subsymbols = TRUE;
					}
				}
			}

			cur = ucl_obj_get_key (cur_rule, "ips");

			if (cur != NULL && cur->type == UCL_OBJECT) {
				it = NULL;
				new_suffix->ips = g_hash_table_new (g_int_hash, g_int_equal);
				rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
						(rspamd_mempool_destruct_t)g_hash_table_unref,
						new_suffix->ips);

				while ((cur_bit =
						ucl_iterate_object (cur, &it, true)) != NULL) {
					if (ucl_object_key (cur_bit) != NULL) {
						gchar *p;

						ip_val = ucl_obj_tostring (cur_bit);
						new_bit = rspamd_mempool_alloc (
								surbl_module_ctx->surbl_pool,
								sizeof (struct surbl_bit_item));

						if (inet_pton (AF_INET, ip_val, &bit) != 1) {
							msg_err_config ("cannot parse ip %s: %s", ip_val,
									strerror (errno));
							continue;
						}

						new_bit->bit = bit;
						new_bit->symbol = rspamd_mempool_strdup (
								surbl_module_ctx->surbl_pool,
								ucl_object_key (cur_bit));
						/* Convert to uppercase */
						p = new_bit->symbol;
						while (*p) {
							*p = g_ascii_toupper (*p);
							p ++;
						}

						msg_debug_config ("add new IP suffix: %d with symbol: %s",
								(gint)new_bit->bit, new_bit->symbol);
						g_hash_table_insert (new_suffix->ips, &new_bit->bit,
								new_bit);
						has_subsymbols = TRUE;
					}
				}
			}

			if (!has_subsymbols) {
				/* Register just a symbol itself */
				rspamd_symbols_cache_add_symbol (cfg->cache,
						new_suffix->symbol,
						0,
						NULL, NULL,
						SYMBOL_TYPE_VIRTUAL,
						cb_id);
				nrules ++;
			}
			surbl_module_ctx->suffixes = g_list_prepend (
				surbl_module_ctx->suffixes,
				new_suffix);
		}
	}
	/* Add default suffix */
	if (surbl_module_ctx->suffixes == NULL) {
		msg_err_config ("surbl module loaded but no suffixes defined, skip "
				"checks");
		return TRUE;
	}

	if (surbl_module_ctx->suffixes != NULL) {
		rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
			(rspamd_mempool_destruct_t) g_list_free,
			surbl_module_ctx->suffixes);
	}

	cur_opt = surbl_module_ctx->suffixes;
	while (cur_opt) {
		cur_suffix = cur_opt->data;
		if (cur_suffix->bits != NULL || cur_suffix->ips != NULL) {
			register_bit_symbols (cfg, cur_suffix, cur_suffix->callback_id);
		}
		cur_opt = g_list_next (cur_opt);
	}

	msg_info_config ("init internal surbls module, %d uribl rules loaded",
			nrules);

	return TRUE;
}

gint
surbl_module_reconfig (struct rspamd_config *cfg)
{
	/* Delete pool and objects */
	rspamd_mempool_delete (surbl_module_ctx->surbl_pool);
	/* Reinit module */
	surbl_module_ctx->use_redirector = 0;
	surbl_module_ctx->suffixes = NULL;
	surbl_module_ctx->surbl_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), NULL);

	surbl_module_ctx->tld2_file = NULL;
	surbl_module_ctx->whitelist_file = NULL;
	surbl_module_ctx->redirectors = NULL;
	surbl_module_ctx->whitelist = g_hash_table_new (rspamd_strcase_hash,
			rspamd_strcase_equal);
	/* Zero exceptions hashes */
	surbl_module_ctx->exceptions = rspamd_mempool_alloc0 (
		surbl_module_ctx->surbl_pool,
		MAX_LEVELS * sizeof (GHashTable *));
	/* Register destructors */
	rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
		(rspamd_mempool_destruct_t) g_hash_table_destroy,
		surbl_module_ctx->whitelist);
	rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
		(rspamd_mempool_destruct_t) g_hash_table_destroy,
		surbl_module_ctx->redirector_hosts);

	rspamd_mempool_add_destructor (surbl_module_ctx->surbl_pool,
		(rspamd_mempool_destruct_t) g_list_free,
		surbl_module_ctx->suffixes);

	/* Perform configure */
	return surbl_module_config (cfg);
}



static gchar *
format_surbl_request (rspamd_mempool_t * pool,
	rspamd_ftok_t * hostname,
	struct suffix_item *suffix,
	gboolean append_suffix,
	GError ** err,
	gboolean forced,
	GHashTable *tree,
	struct rspamd_url *url)
{
	GHashTable *t;
	gchar *result = NULL;
	const gchar *p, *dots[MAX_LEVELS];
	gint r, i, dots_num = 0, level = MAX_LEVELS;
	gsize slen, len;
	gboolean found_exception = FALSE;
	rspamd_ftok_t f;

	if (G_LIKELY (suffix != NULL)) {
		slen = strlen (suffix->suffix);
	}
	else if (!append_suffix) {
		slen = 0;
	}
	else {
		g_assert_not_reached ();
	}

	len = hostname->len + slen + 2;

	p = hostname->begin;

	while (p - hostname->begin < (gint)hostname->len && dots_num < MAX_LEVELS) {
		if (*p == '.') {
			dots[dots_num] = p;
			dots_num++;
		}

		p++;
	}

	/* Check for numeric expressions */
	if (url->flags & RSPAMD_URL_FLAG_NUMERIC) {
		/* This is ip address */
		if (suffix != NULL && (suffix->options & SURBL_OPTION_NOIP) != 0) {
			/* Ignore such requests */
			msg_info_pool ("ignore request of ip url for list %s",
					suffix->symbol);
			return NULL;
		}

		if (dots_num == 3) {
			/* IPv4 address */
			result = rspamd_mempool_alloc (pool, len);
			r = rspamd_snprintf (result, len, "%*s.%*s.%*s.%*s",
					(gint) (hostname->len - (dots[2] - hostname->begin + 1)),
					dots[2] + 1,
					(gint) (dots[2] - dots[1] - 1),
					dots[1] + 1,
					(gint) (dots[1] - dots[0] - 1),
					dots[0] + 1,
					(gint) (dots[0] - hostname->begin),
					hostname->begin);
		}
		else {
			/* Just pring ip as is */
			result = rspamd_mempool_alloc (pool, len);
			r = rspamd_snprintf (result, len, "%*s",
					(gint)hostname->len, hostname->begin);
		}
	}
	else {
		/* Not a numeric url */
		result = rspamd_mempool_alloc (pool, len);
		/* Now we should try to check for exceptions */
		if (!forced) {
			for (i = MAX_LEVELS - 1; i >= 0; i--) {
				t = surbl_module_ctx->exceptions[i];
				if (t != NULL && dots_num >= i + 1) {
					f.begin = dots[dots_num - i - 1] + 1;
					f.len = hostname->len -
						(dots[dots_num - i - 1] - hostname->begin + 1);
					if (g_hash_table_lookup (t, &f) != NULL) {
						level = dots_num - i - 1;
						found_exception = TRUE;
						break;
					}
				}
			}
		}

		if (found_exception || url->tldlen == 0) {
			if (level != MAX_LEVELS) {
				if (level == 0) {
					r = rspamd_snprintf (result,
							len,
							"%T",
							hostname);
				}
				else {
					r = rspamd_snprintf (result, len, "%*s",
							(gint)(hostname->len -
									(dots[level - 1] - hostname->begin + 1)),
									dots[level - 1] + 1);
				}
			}
			else if (dots_num >= 2) {
				r = rspamd_snprintf (result, len, "%*s",
						(gint)(hostname->len -
								(dots[dots_num - 2] - hostname->begin + 1)),
								dots[dots_num - 2] + 1);
			}
			else {
				r = rspamd_snprintf (result,
						len,
						"%T",
						hostname);
			}
		}
		else {
			r = rspamd_snprintf (result,
						len,
						"%*s",
						url->tldlen,
						url->tld);
		}
	}

	url->surbl = result;
	url->surbllen = r;

	if (tree != NULL) {
		if (g_hash_table_lookup (tree, result) != NULL) {
			msg_debug_pool ("url %s is already registered", result);
			g_set_error (err, SURBL_ERROR,
				DUPLICATE_ERROR,
				"URL is duplicated: %s",
				result);
			return NULL;
		}
		else {
			g_hash_table_insert (tree, result, url);
		}
	}

	if (!forced &&
		g_hash_table_lookup (surbl_module_ctx->whitelist, result) != NULL) {
		msg_debug_pool ("url %s is whitelisted", result);
		g_set_error (err, SURBL_ERROR,
			WHITELIST_ERROR,
			"URL is whitelisted: %s",
			result);
		return NULL;
	}


	if (append_suffix) {
		rspamd_snprintf (result + r, len - r, ".%s", suffix->suffix);
	}

	msg_debug_pool ("request: %s, dots: %d, level: %d, orig: %*s",
		result,
		dots_num,
		level,
		(gint)hostname->len,
		hostname->begin);

	return result;
}

static void
make_surbl_requests (struct rspamd_url *url, struct rspamd_task *task,
	struct suffix_item *suffix, gboolean forced, GHashTable *tree)
{
	gchar *surbl_req;
	rspamd_ftok_t f;
	GError *err = NULL;
	struct dns_param *param;

	f.begin = url->host;
	f.len = url->hostlen;

	if (suffix->options & SURBL_OPTION_RESOLVEIP) {
		/*
		 * We need to get url real TLD, resolve it with no suffix and then
		 * check against surbl using reverse octets printing
		 */
		surbl_req = format_surbl_request (task->task_pool, &f, suffix, FALSE,
				&err, forced, tree, url);

		if (surbl_req == NULL) {
			if (err != NULL) {
				if (err->code != WHITELIST_ERROR && err->code != DUPLICATE_ERROR) {
					msg_info_task ("cannot format url string for surbl %*s, %e",
							url->urllen, url->string,
							err);
				}
				g_error_free (err);
				return;
			}
		}
		else {
			/* XXX: We make merely A request here */
			param =
					rspamd_mempool_alloc (task->task_pool,
							sizeof (struct dns_param));
			param->url = url;
			param->task = task;
			param->suffix = suffix;
			param->host_resolve =
					rspamd_mempool_strdup (task->task_pool, surbl_req);
			debug_task ("send surbl dns ip request %s", surbl_req);

			if (make_dns_request_task (task,
					surbl_dns_ip_callback,
					(void *) param, RDNS_REQUEST_A, surbl_req)) {
				param->w = rspamd_session_get_watcher (task->s);
				rspamd_session_watcher_push (task->s);
			}
		}
	}
	else if ((surbl_req = format_surbl_request (task->task_pool, &f, suffix, TRUE,
		&err, forced, tree, url)) != NULL) {
		param =
			rspamd_mempool_alloc (task->task_pool, sizeof (struct dns_param));
		param->url = url;
		param->task = task;
		param->suffix = suffix;
		param->host_resolve =
			rspamd_mempool_strdup (task->task_pool, surbl_req);
		debug_task ("send surbl dns request %s", surbl_req);

		if (make_dns_request_task (task,
				surbl_dns_callback,
				(void *) param, RDNS_REQUEST_A, surbl_req)) {
			param->w = rspamd_session_get_watcher (task->s);
			rspamd_session_watcher_push (task->s);
		}
	}
	else if (err != NULL) {
		if (err->code != WHITELIST_ERROR && err->code != DUPLICATE_ERROR) {
			msg_info_task ("cannot format url string for surbl %*s, %e",
					url->urllen,
					url->string, err);
		}
		g_error_free (err);
		return;
	}
}

static void
process_dns_results (struct rspamd_task *task,
	struct suffix_item *suffix,
	gchar *url,
	guint32 addr)
{
	guint i;
	struct surbl_bit_item *bit;

	if (suffix->ips && g_hash_table_size (suffix->ips) > 0) {

		bit = g_hash_table_lookup (suffix->ips, &addr);
		if (bit != NULL) {
			rspamd_task_insert_result (task, bit->symbol, 1,
				g_list_prepend (NULL,
				rspamd_mempool_strdup (task->task_pool, url)));
		}
	}
	else if (suffix->bits != NULL && suffix->bits->len > 0) {
		for (i = 0; i < suffix->bits->len; i ++) {

			bit = &g_array_index (suffix->bits, struct surbl_bit_item, i);
			debug_task ("got result(%d) AND bit(%d): %d",
				(gint)addr,
				(gint)ntohl (bit->bit),
				(gint)bit->bit & (gint)ntohl (addr));
			if (((gint)bit->bit & (gint)ntohl (addr)) != 0) {
				rspamd_task_insert_result (task, bit->symbol, 1,
					g_list_prepend (NULL,
					rspamd_mempool_strdup (task->task_pool, url)));
			}
		}
	}
	else {
		rspamd_task_insert_result (task, suffix->symbol, 1,
			g_list_prepend (NULL,
			rspamd_mempool_strdup (task->task_pool, url)));
	}
}

static void
surbl_dns_callback (struct rdns_reply *reply, gpointer arg)
{
	struct dns_param *param = (struct dns_param *)arg;
	struct rspamd_task *task;
	struct rdns_reply_entry *elt;

	task = param->task;
	/* If we have result from DNS server, this url exists in SURBL, so increase score */
	if (reply->code == RDNS_RC_NOERROR && reply->entries) {
		msg_info_task ("<%s> domain [%s] is in surbl %s",
				param->task->message_id,
			param->host_resolve, param->suffix->suffix);
		elt = reply->entries;
		if (elt->type == RDNS_REQUEST_A) {
			process_dns_results (param->task, param->suffix,
				param->host_resolve, (guint32)elt->content.a.addr.s_addr);
		}
	}
	else {
		msg_debug_task ("<%s> domain [%s] is not in surbl %s",
			param->task->message_id, param->host_resolve,
			param->suffix->suffix);
	}

	rspamd_session_watcher_pop (param->task->s, param->w);
}

static void
surbl_dns_ip_callback (struct rdns_reply *reply, gpointer arg)
{
	struct dns_param *param = (struct dns_param *) arg;
	struct rspamd_task *task;
	struct rdns_reply_entry *elt;
	GString *to_resolve;
	guint32 ip_addr;

	task = param->task;
	/* If we have result from DNS server, this url exists in SURBL, so increase score */
	if (reply->code == RDNS_RC_NOERROR && reply->entries) {

		LL_FOREACH (reply->entries, elt) {

			if (elt->type == RDNS_REQUEST_A) {
				to_resolve = g_string_sized_new (
						strlen (param->suffix->suffix) +
						sizeof ("255.255.255.255."));
				ip_addr = elt->content.a.addr.s_addr;

				/* Big endian <4>.<3>.<2>.<1> */
				rspamd_printf_gstring (to_resolve, "%d.%d.%d.%d.%s",
						ip_addr >> 24 & 0xff,
						ip_addr >> 16 & 0xff,
						ip_addr >> 8 & 0xff,
						ip_addr & 0xff, param->suffix->suffix);

				if (make_dns_request_task (task,
						surbl_dns_callback,
						param, RDNS_REQUEST_A, to_resolve->str)) {
					rspamd_session_watcher_push_specific (task->s, param->w);
				}

				g_string_free (to_resolve, TRUE);
			}
		}
	}
	else {
		msg_debug_task ("<%s> domain [%s] cannot be resolved for SURBL check %s",
				param->task->message_id, param->host_resolve,
				param->suffix->suffix);

	}

	rspamd_session_watcher_pop (param->task->s, param->w);
}

static void
free_redirector_session (void *ud)
{
	struct redirector_param *param = (struct redirector_param *)ud;

	rspamd_http_connection_unref (param->conn);
	close (param->sock);
}

static void
surbl_redirector_error (struct rspamd_http_connection *conn,
	GError *err)
{
	struct redirector_param *param = (struct redirector_param *)conn->ud;
	struct rspamd_task *task;

	task = param->task;
	msg_err_task ("connection with http server %s terminated incorrectly: %e",
		rspamd_inet_address_to_string (rspamd_upstream_addr (param->redirector)),
		err);
	rspamd_upstream_fail (param->redirector);
	rspamd_session_remove_event (param->task->s, free_redirector_session,
			param);
}

static int
surbl_redirector_finish (struct rspamd_http_connection *conn,
		struct rspamd_http_message *msg)
{
	struct redirector_param *param = (struct redirector_param *)conn->ud;
	struct rspamd_task *task;
	gint r, urllen;
	struct rspamd_url *redirected_url;
	const rspamd_ftok_t *hdr;
	gchar *urlstr;

	task = param->task;

	if (msg->code == 200) {
		hdr = rspamd_http_message_find_header (msg, "Uri");

		if (hdr != NULL) {
			msg_info_task ("<%s> got reply from redirector: '%*s' -> '%T'",
					param->task->message_id,
					param->url->urllen, param->url->string,
					hdr);
			urllen = hdr->len;
			urlstr = rspamd_mempool_alloc (task->task_pool,
					urllen + 1);
			redirected_url = rspamd_mempool_alloc (task->task_pool,
					sizeof (*redirected_url));
			rspamd_strlcpy (urlstr, hdr->begin, urllen + 1);
			r = rspamd_url_parse (redirected_url, urlstr, urllen,
					task->task_pool);

			if (r == URI_ERRNO_OK) {
				make_surbl_requests (redirected_url,
						param->task,
						param->suffix,
						FALSE,
						param->tree);
			}
			else {
				msg_info_task ("cannot parse redirector reply: %s", urlstr);
			}
		}
	}
	else {
		msg_info_task ("<%s> could not resolve '%*s' on redirector",
				param->task->message_id,
				param->url->urllen, param->url->string);
	}

	rspamd_upstream_ok (param->redirector);
	rspamd_session_remove_event (param->task->s, free_redirector_session,
			param);

	return 0;
}


static void
register_redirector_call (struct rspamd_url *url, struct rspamd_task *task,
	struct suffix_item *suffix, const gchar *rule, GHashTable *tree)
{
	gint s = -1;
	struct redirector_param *param;
	struct timeval *timeout;
	struct upstream *selected;
	struct rspamd_http_message *msg;

	selected = rspamd_upstream_get (surbl_module_ctx->redirectors,
			RSPAMD_UPSTREAM_ROUND_ROBIN, url->host, url->hostlen);

	if (selected) {
		s = rspamd_inet_address_connect (rspamd_upstream_addr (selected),
				SOCK_STREAM, TRUE);
	}

	if (s == -1) {
		msg_info_task ("<%s> cannot create tcp socket failed: %s",
			task->message_id,
			strerror (errno));
		make_surbl_requests (url, task, suffix, FALSE, tree);
		return;
	}

	param =
		rspamd_mempool_alloc (task->task_pool,
			sizeof (struct redirector_param));
	param->url = url;
	param->task = task;
	param->conn = rspamd_http_connection_new (NULL, surbl_redirector_error,
			surbl_redirector_finish,
			RSPAMD_HTTP_CLIENT_SIMPLE,
			RSPAMD_HTTP_CLIENT, NULL);
	msg = rspamd_http_new_message (HTTP_REQUEST);
	msg->url = rspamd_fstring_assign (msg->url, url->string, url->urllen);
	param->sock = s;
	param->suffix = suffix;
	param->redirector = selected;
	param->tree = tree;
	timeout = rspamd_mempool_alloc (task->task_pool, sizeof (struct timeval));
	double_to_tv (surbl_module_ctx->read_timeout, timeout);

	rspamd_session_add_event (task->s,
		free_redirector_session,
		param,
		g_quark_from_static_string ("surbl"));

	rspamd_http_connection_write_message (param->conn, msg, NULL,
			NULL, param, s, timeout, task->ev_base);

	msg_info_task (
		"<%s> registered redirector call for %*s to %s, according to rule: %s",
		task->message_id,
		url->urllen, url->string,
		rspamd_upstream_name (param->redirector),
		rule);
}

static gint
surbl_redirector_trie_cb (int strnum, int textpos, void *context)
{
	struct rspamd_url *url = context;

	if (textpos == (gint)url->tldlen) {
		return strnum + 1;
	}

	return 0;
}

static void
surbl_tree_url_callback (gpointer key, gpointer value, void *data)
{
	struct redirector_param *param = data;
	struct rspamd_task *task;
	struct rspamd_url *url = value;
	rspamd_regexp_t *re;
	gint idx = 0, state = 0;
	ac_trie_pat_t *pat;
	gboolean found = FALSE;

	task = param->task;
	debug_task ("check url %*s", url->urllen, url->string);

	if (url->hostlen <= 0) {
		return;
	}

	if (surbl_module_ctx->use_redirector) {
		/* Search in trie */
		if (surbl_module_ctx->redirector_trie) {
			idx = acism_lookup (surbl_module_ctx->redirector_trie, url->tld,
					url->tldlen, surbl_redirector_trie_cb, url, &state, true);
			if (idx > 0) {
				pat = &g_array_index (surbl_module_ctx->redirector_ptrs,
						ac_trie_pat_t, idx - 1);
				/* Try to find corresponding regexp */
				re = g_hash_table_lookup (
						surbl_module_ctx->redirector_hosts,
						pat->ptr);
				if (re == NULL) {
					/* Perform exact match */
					if (pat->len == url->tldlen && memcmp (pat->ptr,
							url->tld, pat->len) == 0) {
						found = TRUE;
					}
				}
				else if (rspamd_regexp_search (re, url->string, 0,
						NULL, NULL, TRUE, NULL)) {
					found = TRUE;
				}

				if (found) {
					if (surbl_module_ctx->redirector_symbol != NULL) {
						rspamd_task_insert_result (param->task,
								surbl_module_ctx->redirector_symbol,
								1,
								g_list_prepend (NULL, rspamd_mempool_strdup
										(task->task_pool, pat->ptr)));
					}
					register_redirector_call (url,
							param->task,
							param->suffix,
							pat->ptr,
							param->tree);

					return;
				}
			}
		}
		make_surbl_requests (url, param->task, param->suffix, FALSE,
			param->tree);
	}
	else {
		make_surbl_requests (url, param->task, param->suffix, FALSE,
			param->tree);
	}
}

static void
surbl_test_url (struct rspamd_task *task, void *user_data)
{
	struct redirector_param param;
	struct suffix_item *suffix = user_data;

	param.task = task;
	param.suffix = suffix;
	param.tree = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	rspamd_mempool_add_destructor (task->task_pool,
		(rspamd_mempool_destruct_t)g_hash_table_unref,
		param.tree);
	g_hash_table_foreach (task->urls, surbl_tree_url_callback, &param);
}
