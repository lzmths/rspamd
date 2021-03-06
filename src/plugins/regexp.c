/*
 * Copyright (c) 2009-2015, Vsevolod Stakhov
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

/***MODULE:regexp
 * rspamd module that implements different regexp rules
 */


#include "config.h"
#include "libmime/message.h"
#include "expression.h"
#include "mime_expressions.h"
#include "libutil/map.h"
#include "lua/lua_common.h"

struct regexp_module_item {
	struct rspamd_expression *expr;
	const gchar *symbol;
	struct ucl_lua_funcdata *lua_function;
};

struct regexp_ctx {
	struct module_ctx ctx;
	rspamd_mempool_t *regexp_pool;
	gsize max_size;
};

static struct regexp_ctx *regexp_module_ctx = NULL;

static void process_regexp_item (struct rspamd_task *task, void *user_data);


/* Initialization */
gint regexp_module_init (struct rspamd_config *cfg, struct module_ctx **ctx);
gint regexp_module_config (struct rspamd_config *cfg);
gint regexp_module_reconfig (struct rspamd_config *cfg);

module_t regexp_module = {
	"regexp",
	regexp_module_init,
	regexp_module_config,
	regexp_module_reconfig,
	NULL
};

/* Process regexp expression */
static gboolean
read_regexp_expression (rspamd_mempool_t * pool,
	struct regexp_module_item *chain,
	const gchar *symbol,
	const gchar *line,
	struct rspamd_config *cfg)
{
	struct rspamd_expression *e = NULL;
	GError *err = NULL;

	if (!rspamd_parse_expression (line, 0, &mime_expr_subr, cfg, pool, &err,
			&e)) {
		msg_warn_pool ("%s = \"%s\" is invalid regexp expression: %e", symbol,
				line,
				err);
		g_error_free (err);

		return FALSE;
	}

	g_assert (e != NULL);
	chain->expr = e;

	return TRUE;
}


/* Init function */
gint
regexp_module_init (struct rspamd_config *cfg, struct module_ctx **ctx)
{
	regexp_module_ctx = g_malloc (sizeof (struct regexp_ctx));

	regexp_module_ctx->regexp_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), NULL);

	*ctx = (struct module_ctx *)regexp_module_ctx;

	return 0;
}

gint
regexp_module_config (struct rspamd_config *cfg)
{
	struct regexp_module_item *cur_item;
	const ucl_object_t *sec, *value, *elt;
	ucl_object_iter_t it = NULL;
	gint res = TRUE, id, nre = 0, nlua = 0;

	if (!rspamd_config_is_module_enabled (cfg, "regexp")) {
		return TRUE;
	}

	sec = ucl_object_find_key (cfg->rcl_obj, "regexp");
	if (sec == NULL) {
		msg_err_config ("regexp module enabled, but no rules are defined");
		return TRUE;
	}

	regexp_module_ctx->max_size = 0;

	while ((value = ucl_iterate_object (sec, &it, true)) != NULL) {
		if (g_ascii_strncasecmp (ucl_object_key (value), "max_size",
			sizeof ("max_size") - 1) == 0) {
			regexp_module_ctx->max_size = ucl_obj_toint (value);
			rspamd_re_cache_set_limit (cfg->re_cache, regexp_module_ctx->max_size);
		}
		else if (g_ascii_strncasecmp (ucl_object_key (value), "max_threads",
			sizeof ("max_threads") - 1) == 0) {
			msg_warn_config ("regexp module is now single threaded, max_threads is ignored");
		}
		else if (value->type == UCL_STRING) {
			cur_item = rspamd_mempool_alloc0 (regexp_module_ctx->regexp_pool,
					sizeof (struct regexp_module_item));
			cur_item->symbol = ucl_object_key (value);
			if (!read_regexp_expression (regexp_module_ctx->regexp_pool,
				cur_item, ucl_object_key (value),
				ucl_obj_tostring (value), cfg)) {
				res = FALSE;
			}
			else {
				rspamd_symbols_cache_add_symbol (cfg->cache,
						cur_item->symbol,
						0,
						process_regexp_item,
						cur_item,
						SYMBOL_TYPE_NORMAL, -1);
				nre ++;
			}
		}
		else if (value->type == UCL_USERDATA) {
			/* Just a lua function */
			cur_item = rspamd_mempool_alloc0 (regexp_module_ctx->regexp_pool,
					sizeof (struct regexp_module_item));
			cur_item->symbol = ucl_object_key (value);
			cur_item->lua_function = ucl_object_toclosure (value);
			rspamd_symbols_cache_add_symbol (cfg->cache,
				cur_item->symbol,
				0,
				process_regexp_item,
				cur_item,
				SYMBOL_TYPE_NORMAL, -1);
			nlua ++;
		}
		else if (value->type == UCL_OBJECT) {
			const gchar *description = NULL, *group = NULL,
					*metric = DEFAULT_METRIC;
			gdouble score = 0.0;
			gboolean one_shot = FALSE, is_lua = FALSE, valid_expression = TRUE;

			/* We have some lua table, extract its arguments */
			elt = ucl_object_find_key (value, "callback");

			if (elt == NULL || elt->type != UCL_USERDATA) {

				/* Try plain regexp expression */
				elt = ucl_object_find_any_key (value, "regexp", "re", NULL);

				if (elt != NULL && ucl_object_type (elt) == UCL_STRING) {
					cur_item = rspamd_mempool_alloc0 (regexp_module_ctx->regexp_pool,
							sizeof (struct regexp_module_item));
					cur_item->symbol = ucl_object_key (value);
					if (!read_regexp_expression (regexp_module_ctx->regexp_pool,
							cur_item, ucl_object_key (value),
							ucl_obj_tostring (elt), cfg)) {
						res = FALSE;
					}
					else {
						valid_expression = TRUE;
						nre ++;
					}
				}
				else {
					msg_err_config (
							"no callback/expression defined for regexp symbol: "
									"%s", ucl_object_key (value));
				}
			}
			else {
				is_lua = TRUE;
				nlua ++;
				cur_item = rspamd_mempool_alloc0 (
						regexp_module_ctx->regexp_pool,
						sizeof (struct regexp_module_item));
				cur_item->symbol = ucl_object_key (value);
				cur_item->lua_function = ucl_object_toclosure (value);
			}

			if (cur_item && (is_lua || valid_expression)) {
				id = rspamd_symbols_cache_add_symbol (cfg->cache,
						cur_item->symbol,
						0,
						process_regexp_item,
						cur_item,
						SYMBOL_TYPE_NORMAL, -1);

				elt = ucl_object_find_key (value, "condition");

				if (elt != NULL && ucl_object_type (elt) == UCL_USERDATA) {
					struct ucl_lua_funcdata *conddata;

					conddata = ucl_object_toclosure (elt);
					rspamd_symbols_cache_add_condition (cfg->cache, id,
							conddata->L, conddata->idx);
				}

				elt = ucl_object_find_key (value, "metric");

				if (elt) {
					metric = ucl_object_tostring (elt);
				}

				elt = ucl_object_find_key (value, "description");

				if (elt) {
					description = ucl_object_tostring (elt);
				}

				elt = ucl_object_find_key (value, "group");

				if (elt) {
					group = ucl_object_tostring (elt);
				}

				elt = ucl_object_find_key (value, "score");

				if (elt) {
					score = ucl_object_todouble (elt);
				}

				elt = ucl_object_find_key (value, "one_shot");

				if (elt) {
					one_shot = ucl_object_toboolean (elt);
				}

				rspamd_config_add_metric_symbol (cfg, metric, cur_item->symbol,
						score, description, group, one_shot, FALSE);
			}
		}
		else {
			msg_warn_config ("unknown type of attribute %s for regexp module",
				ucl_object_key (value));
		}
	}

	msg_info_config ("init internal regexp module, %d regexp rules and %d "
			"lua rules are loaded", nre, nlua);

	return res;
}

gint
regexp_module_reconfig (struct rspamd_config *cfg)
{
	struct module_ctx saved_ctx;

	saved_ctx = regexp_module_ctx->ctx;
	rspamd_mempool_delete (regexp_module_ctx->regexp_pool);
	memset (regexp_module_ctx, 0, sizeof (*regexp_module_ctx));
	regexp_module_ctx->ctx = saved_ctx;
	regexp_module_ctx->regexp_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), NULL);

	return regexp_module_config (cfg);
}

static gboolean rspamd_lua_call_expression_func(
		struct ucl_lua_funcdata *lua_data, struct rspamd_task *task,
		GArray *args, gint *res)
{
	lua_State *L = lua_data->L;
	struct rspamd_task **ptask;
	struct expression_argument *arg;
	gint pop = 0, i, nargs = 0;

	lua_rawgeti (L, LUA_REGISTRYINDEX, lua_data->idx);
	/* Now we got function in top of stack */
	ptask = lua_newuserdata (L, sizeof(struct rspamd_task *));
	rspamd_lua_setclass (L, "rspamd{task}", -1);
	*ptask = task;

	/* Now push all arguments */
	if (args) {
		for (i = 0; i < (gint)args->len; i ++) {
			arg = &g_array_index (args, struct expression_argument, i);
			if (arg) {
				switch (arg->type) {
				case EXPRESSION_ARGUMENT_NORMAL:
					lua_pushstring (L, (const gchar *) arg->data);
					break;
				case EXPRESSION_ARGUMENT_BOOL:
					lua_pushboolean (L, (gboolean) GPOINTER_TO_SIZE(arg->data));
					break;
				default:
					msg_err_task ("cannot pass custom params to lua function");
					return FALSE;
				}
			}
		}
		nargs = args->len;
	}

	if (lua_pcall (L, nargs + 1, 1, 0) != 0) {
		msg_info_task ("call to lua function failed: %s", lua_tostring (L, -1));
		return FALSE;
	}
	pop++;

	if (lua_type (L, -1) == LUA_TNUMBER) {
		*res = lua_tonumber (L, -1);
	}
	else if (lua_type (L, -1) == LUA_TBOOLEAN) {
		*res = lua_toboolean (L, -1);
	}
	else {
		msg_info_task ("lua function must return a boolean");
	}

	lua_pop (L, pop);

	return TRUE;
}


static void
process_regexp_item (struct rspamd_task *task, void *user_data)
{
	struct regexp_module_item *item = user_data;
	gint res = FALSE;

	/* Non-threaded version */
	if (item->lua_function) {
		/* Just call function */
		res = FALSE;
		if (!rspamd_lua_call_expression_func (item->lua_function, task, NULL,
				&res)) {
			msg_err_task ("error occurred when checking symbol %s",
					item->symbol);
		}
	}
	else {
		/* Process expression */
		if (item->expr) {
			res = rspamd_process_expression (item->expr, 0, task);
		}
		else {
			msg_warn_task ("FIXME: %s symbol is broken with new expressions",
					item->symbol);
		}
	}

	if (res) {
		rspamd_task_insert_result (task, item->symbol, res, NULL);
	}
}
