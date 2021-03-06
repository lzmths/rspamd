/* Copyright (c) 2010-2012, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
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

#include "lua_common.h"
#include "mem_pool.h"

/***
 * @module rspamd_mempool
 * Rspamd memory pool is used to allocate memory attached to specific objects,
 * namely it was initially used for memory allocation for rspamd_task.
 *
 * All memory allocated by the pool is destroyed when the associated object is
 * destroyed. This allows a sort of controlled garbage collection for memory
 * allocated from the pool. Memory pools are extensively used by rspamd internal
 * components and provide some powerful features, such as destructors or
 * persistent variables.
 * @example
local mempool = require "rspamd_mempool"
local pool = mempool.create()

pool:set_variable('a', 'bcd', 1, 1.01, false)
local v1, v2, v3, v4 = pool:get_variable('a', 'string,double,double,bool')
pool:destroy()
 */

/* Lua bindings */
/***
 * @function mempool.create([size])
 * Creates a memory pool of a specified `size` or platform dependent optimal size (normally, a page size)
 * @param {number} size size of a page inside pool
 * @return {rspamd_mempool} new pool object (that should be removed by explicit call to `pool:destroy()`)
 */
LUA_FUNCTION_DEF (mempool, create);
/***
 * @method mempool:add_destructor(func)
 * Adds new destructor function to the pool
 * @param {function} func function to be called when the pool is destroyed
 */
LUA_FUNCTION_DEF (mempool, add_destructor);
/***
 * @method mempool:destroy()
 * Destroys memory pool cleaning all variables and calling all destructors registered (both C and Lua ones)
 */
LUA_FUNCTION_DEF (mempool, delete);
LUA_FUNCTION_DEF (mempool, stat);
LUA_FUNCTION_DEF (mempool, suggest_size);
/***
 * @method mempool:set_variable(name, [value1[, value2 ...]])
 * Sets a variable that's valid during memory pool lifetime. This function allows
 * to pack multiple values inside a single variable. Currently supported types are:
 *
 * - `string`: packed as null terminated C string (so no `\0` are allowed)
 * - `number`: packed as C double
 * - `boolean`: packed as bool
 * @param {string} name variable's name to set
 */
LUA_FUNCTION_DEF (mempool, set_variable);
/***
 * @method mempool:get_variable(name[, type])
 * Unpacks mempool variable to lua If `type` is not specified, then a variable is
 * assumed to be zero-terminated C string. Otherwise, `type` is a comma separated (spaces are ignored)
 * list of types that should be unpacked from a variable's content. The following types
 * are supported:
 *
 * - `string`: null terminated C string (so no `\0` are allowed)
 * - `double`: returned as lua number
 * - `int`: unpack a single integer
 * - `int64`: unpack 64-bits integer
 * - `boolean`: unpack boolean
 * @param {string} name variable's name to get
 * @param {string} type list of types to be extracted
 * @return {variable list} list of variables extracted (but **not** a table)
 */
LUA_FUNCTION_DEF (mempool, get_variable);
/***
 * @method mempool:has_variable(name)
 * Checks if the specified variable `name` exists in the memory pool
 * @param {string} name variable's name to get
 * @return {boolean} `true` if variable exists and `false` otherwise
 */
LUA_FUNCTION_DEF (mempool, has_variable);

/***
 * @method mempool:delete_variable(name)
 * Removes the specified variable `name` from the memory pool
 * @param {string} name variable's name to remove
 * @return {boolean} `true` if variable exists and has been removed
 */
LUA_FUNCTION_DEF (mempool, delete_variable);

static const struct luaL_reg mempoollib_m[] = {
	LUA_INTERFACE_DEF (mempool, add_destructor),
	LUA_INTERFACE_DEF (mempool, stat),
	LUA_INTERFACE_DEF (mempool, suggest_size),
	LUA_INTERFACE_DEF (mempool, set_variable),
	LUA_INTERFACE_DEF (mempool, get_variable),
	LUA_INTERFACE_DEF (mempool, has_variable),
	LUA_INTERFACE_DEF (mempool, delete_variable),
	LUA_INTERFACE_DEF (mempool, delete),
	{"destroy", lua_mempool_delete},
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

static const struct luaL_reg mempoollib_f[] = {
	LUA_INTERFACE_DEF (mempool, create),
	{NULL, NULL}
};

/*
 * Struct for lua destructor
 */

struct lua_mempool_udata {
	lua_State *L;
	gint cbref;
	rspamd_mempool_t *mempool;
};

struct memory_pool_s *
rspamd_lua_check_mempool (lua_State * L, gint pos)
{
	void *ud = luaL_checkudata (L, pos, "rspamd{mempool}");
	luaL_argcheck (L, ud != NULL, pos, "'mempool' expected");
	return ud ? *((struct memory_pool_s **)ud) : NULL;
}


static int
lua_mempool_create (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_mempool_new (rspamd_mempool_suggest_size (), NULL), **pmempool;

	if (mempool) {
		pmempool = lua_newuserdata (L, sizeof (struct memory_pool_s *));
		rspamd_lua_setclass (L, "rspamd{mempool}", -1);
		*pmempool = mempool;
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static void
lua_mempool_destructor_func (gpointer p)
{
	struct lua_mempool_udata *ud = p;

	lua_rawgeti (ud->L, LUA_REGISTRYINDEX, ud->cbref);
	if (lua_pcall (ud->L, 0, 0, 0) != 0) {
		msg_info ("call to destructor failed: %s", lua_tostring (ud->L, -1));
	}
	luaL_unref (ud->L, LUA_REGISTRYINDEX, ud->cbref);
}

static int
lua_mempool_add_destructor (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);
	struct lua_mempool_udata *ud;

	if (mempool) {
		if (lua_isfunction (L, 2)) {
			ud = rspamd_mempool_alloc (mempool,
					sizeof (struct lua_mempool_udata));
			lua_pushvalue (L, 2);
			/* Get a reference */
			ud->cbref = luaL_ref (L, LUA_REGISTRYINDEX);
			ud->L = L;
			ud->mempool = mempool;
			rspamd_mempool_add_destructor (mempool,
				lua_mempool_destructor_func,
				ud);
		}
		else {
			msg_err ("trying to add destructor without function");
		}
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static int
lua_mempool_delete (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);

	if (mempool) {
		rspamd_mempool_delete (mempool);
		return 0;
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static int
lua_mempool_stat (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);

	if (mempool) {

	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static int
lua_mempool_suggest_size (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);

	if (mempool) {
		lua_pushinteger (L, rspamd_mempool_suggest_size ());
		return 0;
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static int
lua_mempool_set_variable (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);
	const gchar *var = luaL_checkstring (L, 2);
	gpointer value;
	gchar *vp;
	union {
		gdouble d;
		const gchar *s;
		gboolean b;
	} val;
	gsize slen;
	gint i, len = 0, type;

	if (mempool && var) {

		for (i = 3; i <= lua_gettop (L); i ++) {
			type = lua_type (L, i);

			if (type == LUA_TNUMBER) {
				/* We have some ambiguity here between integer and double */
				len += sizeof (gdouble);
			}
			else if (type == LUA_TBOOLEAN) {
				len += sizeof (gboolean);
			}
			else if (type == LUA_TSTRING) {
				(void)lua_tolstring (L, i, &slen);
				len += slen + 1;
			}
			else {
				msg_err ("cannot handle lua type %s", lua_typename (L, type));
			}
		}

		if (len == 0) {
			msg_err ("no values specified");
		}
		else {
			value = rspamd_mempool_alloc (mempool, len);
			vp = value;

			for (i = 3; i <= lua_gettop (L); i ++) {
				type = lua_type (L, i);

				if (type == LUA_TNUMBER) {
					val.d = lua_tonumber (L, i);
					memcpy (vp, &val, sizeof (gdouble));
					vp += sizeof (gdouble);
				}
				else if (type == LUA_TBOOLEAN) {
					val.b = lua_toboolean (L, i);
					memcpy (vp, &val, sizeof (gboolean));
					vp += sizeof (gboolean);
				}
				else if (type == LUA_TSTRING) {
					val.s = lua_tolstring (L, i, &slen);
					memcpy (vp, val.s, slen + 1);
					vp += slen + 1;
				}
				else {
					msg_err ("cannot handle lua type %s", lua_typename (L, type));
				}
			}

			rspamd_mempool_set_variable (mempool, var, value, NULL);
		}

		return 0;
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}


static int
lua_mempool_get_variable (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);
	const gchar *var = luaL_checkstring (L, 2);
	const gchar *type = NULL, *pt;
	gchar *value, *pv;
	guint len, nvar, slen;

	if (mempool && var) {
		value = rspamd_mempool_get_variable (mempool, var);

		if (lua_gettop (L) >= 3) {
			type = luaL_checkstring (L, 3);
		}

		if (value) {

			if (type) {
				pt = type;
				pv = value;
				nvar = 0;

				while ((len = strcspn (pt, ", ")) > 0) {
					if (len == sizeof ("double") - 1 &&
							g_ascii_strncasecmp (pt, "double", len) == 0) {
						lua_pushnumber (L, *(gdouble *)pv);
						pv += sizeof (gdouble);
					}
					else if (len == sizeof ("int") - 1 &&
							g_ascii_strncasecmp (pt, "int", len) == 0) {
						lua_pushnumber (L, *(gint *)pv);
						pv += sizeof (gint);
					}
					else if (len == sizeof ("int64") - 1 &&
							g_ascii_strncasecmp (pt, "int64", len) == 0) {
						lua_pushnumber (L, *(gint64 *)pv);
						pv += sizeof (gint64);
					}
					else if (len == sizeof ("bool") - 1 &&
							g_ascii_strncasecmp (pt, "bool", len) == 0) {
						lua_pushboolean (L, *(gboolean *)pv);
						pv += sizeof (gboolean);
					}
					else if (len == sizeof ("string") - 1 &&
							g_ascii_strncasecmp (pt, "string", len) == 0) {
						slen = strlen ((const gchar *)pv);
						lua_pushlstring (L, (const gchar *)pv, slen);
						pv += slen + 1;
					}
					else {
						msg_err ("unknown type for get_variable: %s", pt);
						lua_pushnil (L);
					}

					pt += len;
					pt += strspn (pt, ", ");

					nvar ++;
				}

				return nvar;
			}

			lua_pushstring (L, value);
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static int
lua_mempool_has_variable (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);
	const gchar *var = luaL_checkstring (L, 2);
	gboolean ret = FALSE;

	if (mempool && var) {
		if (rspamd_mempool_get_variable (mempool, var) != NULL) {
			ret = TRUE;
		}
	}

	lua_pushboolean (L, ret);

	return 1;
}

static int
lua_mempool_delete_variable (lua_State *L)
{
	struct memory_pool_s *mempool = rspamd_lua_check_mempool (L, 1);
	const gchar *var = luaL_checkstring (L, 2);
	gboolean ret = FALSE;

	if (mempool && var) {
		if (rspamd_mempool_get_variable (mempool, var) != NULL) {
			ret = TRUE;

			rspamd_mempool_remove_variable (mempool, var);
		}
	}

	lua_pushboolean (L, ret);

	return 1;
}

static gint
lua_load_mempool (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, mempoollib_f);

	return 1;
}

void
luaopen_mempool (lua_State * L)
{
	luaL_newmetatable (L, "rspamd{mempool}");
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{mempool}");
	lua_rawset (L, -3);

	luaL_register (L, NULL,				mempoollib_m);
	rspamd_lua_add_preload (L, "rspamd_mempool", lua_load_mempool);

	lua_pop (L, 1);                      /* remove metatable from stack */
}
