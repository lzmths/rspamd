/*
 * Copyright (c) 2015, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in the
 *	   documentation and/or other materials provided with the distribution.
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

#include "config.h"
#include "libutil/util.h"
#include "libserver/cfg_file.h"
#include "libserver/cfg_rcl.h"
#include "libserver/worker_util.h"
#include "libserver/rspamd_control.h"
#include "unix-std.h"

#ifdef HAVE_GLOB_H
#include <glob.h>
#include <libserver/rspamd_control.h>

#endif

static gpointer init_hs_helper (struct rspamd_config *cfg);
static void start_hs_helper (struct rspamd_worker *worker);

worker_t hs_helper_worker = {
		"hs_helper",                /* Name */
		init_hs_helper,             /* Init function */
		start_hs_helper,            /* Start function */
		FALSE,                      /* No socket */
		TRUE,                       /* Unique */
		FALSE,                      /* Non threaded */
		TRUE,                       /* Killable */
		SOCK_STREAM                 /* TCP socket */
};

/*
 * Worker's context
 */
struct hs_helper_ctx {
	gchar *hs_dir;
	struct rspamd_config *cfg;
	struct event_base *ev_base;
};

static gpointer
init_hs_helper (struct rspamd_config *cfg)
{
	struct hs_helper_ctx *ctx;
	GQuark type;

	type = g_quark_try_string ("hs_helper");
	ctx = g_malloc0 (sizeof (*ctx));

	ctx->cfg = cfg;
	ctx->hs_dir = RSPAMD_DBDIR "/";

	rspamd_rcl_register_worker_option (cfg, type, "cache_dir",
			rspamd_rcl_parse_struct_string, ctx,
			G_STRUCT_OFFSET (struct hs_helper_ctx, hs_dir), 0);

	return ctx;
}

/**
 * Clean
 */
static gboolean
rspamd_hs_helper_cleanup_dir (struct hs_helper_ctx *ctx)
{
	struct stat st;
	glob_t globbuf;
	guint len, i;
	gint rc;
	gchar *pattern;
	gboolean ret = TRUE;

	if (stat (ctx->hs_dir, &st) == -1) {
		msg_err ("cannot stat path %s, %s",
				ctx->hs_dir,
				strerror (errno));
		return FALSE;
	}

	globbuf.gl_offs = 0;
	len = strlen (ctx->hs_dir) + 1 + sizeof ("*.hs");
	pattern = g_malloc (len);
	rspamd_snprintf (pattern, len, "%s%c%s", ctx->hs_dir, G_DIR_SEPARATOR, "*.hs");

	if ((rc = glob (pattern, GLOB_DOOFFS, NULL, &globbuf)) == 0) {
		for (i = 0; i < globbuf.gl_pathc; i++) {
			if (!rspamd_re_cache_is_valid_hyperscan_file (ctx->cfg->re_cache,
						globbuf.gl_pathv[i])) {
				if (unlink (globbuf.gl_pathv[i]) == -1) {
					msg_err ("cannot unlink %s: %s", globbuf.gl_pathv[i],
							strerror (errno));
					ret = FALSE;
				}
			}
		}
	}
	else if (rc != GLOB_NOMATCH) {
		msg_err ("glob %s failed: %s", pattern, strerror (errno));
		ret = FALSE;
	}

	globfree (&globbuf);
	g_free (pattern);

	return ret;
}

static gboolean
rspamd_rs_compile (struct hs_helper_ctx *ctx, struct rspamd_worker *worker)
{
	GError *err = NULL;
	static struct rspamd_srv_command srv_cmd;
	gint ncompiled;

	if (!rspamd_hs_helper_cleanup_dir (ctx)) {
		msg_warn ("cannot cleanup cache dir '%s'", ctx->hs_dir);
	}

	if ((ncompiled = rspamd_re_cache_compile_hyperscan (ctx->cfg->re_cache,
			ctx->hs_dir,
			&err)) == -1) {
		msg_err ("failed to compile re cache: %e", err);
		g_error_free (err);

		return FALSE;
	}

	msg_info ("compiled %d regular expressions to the hyperscan tree",
			ncompiled);

	srv_cmd.type = RSPAMD_SRV_HYPERSCAN_LOADED;
	srv_cmd.cmd.hs_loaded.cache_dir = ctx->hs_dir;

	rspamd_srv_send_command (worker, ctx->ev_base, &srv_cmd, NULL, NULL);

	return TRUE;
}

static gboolean
rspamd_hs_helper_reload (struct rspamd_main *rspamd_main,
		struct rspamd_worker *worker, gint fd,
		struct rspamd_control_command *cmd,
		gpointer ud)
{
	struct rspamd_control_reply rep;
	struct hs_helper_ctx *ctx = ud;

	msg_info ("recompiling hyperscan expressions after receiving reload command");
	memset (&rep, 0, sizeof (rep));
	rep.type = RSPAMD_CONTROL_RECOMPILE;

	rep.reply.recompile.status = rspamd_rs_compile (ctx, worker);

	if (write (fd, &rep, sizeof (rep)) != sizeof (rep)) {
		msg_err ("cannot write reply to the control socket: %s",
				strerror (errno));
	}

	return TRUE;
}

static void
start_hs_helper (struct rspamd_worker *worker)
{
	struct hs_helper_ctx *ctx = worker->ctx;

	ctx->ev_base = rspamd_prepare_worker (worker,
			"hs_helper",
			NULL);

	if (!rspamd_rs_compile (ctx, worker)) {
		/* Tell main not to respawn more workers */
		exit (EXIT_SUCCESS);
	}

	rspamd_control_worker_add_cmd_handler (worker, RSPAMD_CONTROL_RECOMPILE,
			rspamd_hs_helper_reload, ctx);

	event_base_loop (ctx->ev_base, 0);
	rspamd_worker_block_signals ();

	rspamd_log_close (worker->srv->logger);

	exit (EXIT_SUCCESS);
}
