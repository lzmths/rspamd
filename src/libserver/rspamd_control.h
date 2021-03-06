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

#ifndef RSPAMD_RSPAMD_CONTROL_H
#define RSPAMD_RSPAMD_CONTROL_H

#include "config.h"
#include <event.h>

struct rspamd_main;
struct rspamd_worker;

enum rspamd_control_type {
	RSPAMD_CONTROL_STAT = 0,
	RSPAMD_CONTROL_RELOAD,
	RSPAMD_CONTROL_RERESOLVE,
	RSPAMD_CONTROL_RECOMPILE,
	RSPAMD_CONTROL_HYPERSCAN_LOADED,
	RSPAMD_CONTROL_MAX
};

enum rspamd_srv_type {
	RSPAMD_SRV_SOCKETPAIR = 0,
	RSPAMD_SRV_HYPERSCAN_LOADED,
};

struct rspamd_control_command {
	enum rspamd_control_type type;
	union {
		struct {
			guint unused;
		} stat;
		struct {
			guint unused;
		} reload;
		struct {
			guint unused;
		} reresolve;
		struct {
			guint unused;
		} recompile;
		struct {
			gpointer cache_dir;
		} hs_loaded;
	} cmd;
};

struct rspamd_control_reply {
	enum rspamd_control_type type;
	union {
		struct {
			guint conns;
			gdouble uptime;
			gdouble utime;
			gdouble systime;
			gulong maxrss;
		} stat;
		struct {
			guint status;
		} reload;
		struct {
			guint status;
		} reresolve;
		struct {
			guint status;
		} recompile;
		struct {
			guint status;
		} hs_loaded;
	} reply;
};

#define PAIR_ID_LEN 16
struct rspamd_srv_command {
	enum rspamd_srv_type type;
	guint64 id;
	union {
		struct {
			gint af;
			gchar pair_id[PAIR_ID_LEN];
			guint pair_num;
		} spair;
		struct {
			gpointer cache_dir;
		} hs_loaded;
	} cmd;
};

struct rspamd_srv_reply {
	enum rspamd_srv_type type;
	guint64 id;
	union {
		struct {
			gint code;
		} spair;
		struct {
			gint unused;
		} hs_loaded;
	} reply;
};

typedef gboolean (*rspamd_worker_control_handler) (struct rspamd_main *rspamd_main,
		struct rspamd_worker *worker, gint fd,
		struct rspamd_control_command *cmd,
		gpointer ud);

typedef void (*rspamd_srv_reply_handler) (struct rspamd_worker *worker,
		struct rspamd_srv_reply *rep, gint rep_fd,
		gpointer ud);

/**
 * Process client socket connection
 */
void rspamd_control_process_client_socket (struct rspamd_main *rspamd_main,
		gint fd);

/**
 * Register default handlers for a worker
 */
void rspamd_control_worker_add_default_handler (struct rspamd_worker *worker,
		struct event_base *ev_base);

/**
 * Register custom handler for a specific control command for this worker
 */
void rspamd_control_worker_add_cmd_handler (struct rspamd_worker *worker,
		enum rspamd_control_type type,
		rspamd_worker_control_handler handler,
		gpointer ud);

/**
 * Start watching on srv pipe
 */
void rspamd_srv_start_watching (struct rspamd_worker *worker,
		struct event_base *ev_base);


/**
 * Send command to srv pipe and read reply calling the specified callback at the
 * end
 */
void rspamd_srv_send_command (struct rspamd_worker *worker,
		struct event_base *ev_base,
		struct rspamd_srv_command *cmd,
		rspamd_srv_reply_handler handler, gpointer ud);
#endif
