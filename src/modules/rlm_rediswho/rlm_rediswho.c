/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_rediswho.c
 * @brief Session tracking using redis.
 *
 * @author Gabriel Blanchard
 *
 * @copyright 2015 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2011 TekSavvy Solutions (gabe@teksavvy.com)
 * @copyright 2000,2006 The FreeRADIUS server project
 */

RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/server/modpriv.h>
#include <freeradius-devel/util/debug.h>

#include <freeradius-devel/redis/base.h>
#include <freeradius-devel/redis/cluster.h>

typedef struct {
	fr_redis_conf_t		conf;		//!< Connection parameters for the Redis server.
						//!< Must be first field in this struct.

	fr_redis_cluster_t	*cluster;	//!< Pool O pools

	int			expiry_time;	//!< Expiry time in seconds if no updates are received for a user

	int			trim_count;	//!< How many session updates to keep track of per user.

	char const		*insert;	//!< Command for inserting session data
	char const		*trim;		//!< Command for trimming the session list.
	char const		*expire;	//!< Command for expiring entries.
} rlm_rediswho_t;

static conf_parser_t section_config[] = {
	{ FR_CONF_OFFSET_FLAGS("insert", CONF_FLAG_REQUIRED | CONF_FLAG_XLAT, rlm_rediswho_t, insert) },
	{ FR_CONF_OFFSET_FLAGS("trim", CONF_FLAG_XLAT, rlm_rediswho_t, trim) }, /* required only if trim_count > 0 */
	{ FR_CONF_OFFSET_FLAGS("expire", CONF_FLAG_REQUIRED, rlm_rediswho_t, expire) },
	CONF_PARSER_TERMINATOR
};

static conf_parser_t module_config[] = {
	REDIS_COMMON_CONFIG,

	{ FR_CONF_OFFSET("trim_count", rlm_rediswho_t, trim_count), .dflt = "-1" },

	/*
	 *	These all smash the same variables, because we don't care about them right now.
	 *	In 3.1, we should have a way of saying "parse a set of sub-sections according to a template"
	 */
	{ FR_CONF_POINTER("Start", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = section_config },
	{ FR_CONF_POINTER("Interim-Update", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = section_config },
	{ FR_CONF_POINTER("Stop", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = section_config },
	{ FR_CONF_POINTER("Accounting-On", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = section_config },
	{ FR_CONF_POINTER("Accounting-Off", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = section_config },
	{ FR_CONF_POINTER("Failed", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = section_config },

	CONF_PARSER_TERMINATOR
};

static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t rlm_rediswho_dict[];
fr_dict_autoload_t rlm_rediswho_dict[] = {
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_acct_status_type;

extern fr_dict_attr_autoload_t rlm_rediswho_dict_attr[];
fr_dict_attr_autoload_t rlm_rediswho_dict_attr[] = {
	{ .out = &attr_acct_status_type, .name = "Acct-Status-Type", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ NULL }
};

/*
 *	Query the database executing a command with no result rows
 */
static int rediswho_command(rlm_rediswho_t const *inst, request_t *request, char const *fmt)
{
	fr_redis_conn_t		*conn;

	int 			ret = -1;

	fr_redis_cluster_state_t	state;
	fr_redis_rcode_t		status = REDIS_RCODE_ERROR;
	redisReply		*reply = NULL;
	int			s_ret;

	uint8_t	const		*key = NULL;
	size_t			key_len = 0;

	int			argc;
	char const		*argv[MAX_REDIS_ARGS];
	char			argv_buf[MAX_REDIS_COMMAND_LEN];

	if (!fmt || !*fmt) return 0;

	argc = rad_expand_xlat(request, fmt, MAX_REDIS_ARGS, argv, false, sizeof(argv_buf), argv_buf);
	if (argc < 0) {
		RPEDEBUG("Invalid command: %s", fmt);
		return -1;
	}

	/*
	 *	If we've got multiple arguments, the second one is usually the key.
	 *	The Redis docs say commands should be analysed first to get key
	 *	positions, but this involves sending them to the server, which is
	 *	just as expensive as sending them to the wrong server and receiving
	 *	a redirect.
	 */
	if (argc > 1) {
		key = (uint8_t const *)argv[1];
	 	key_len = strlen((char const *)key);
	}

	for (s_ret = fr_redis_cluster_state_init(&state, &conn, inst->cluster, request, key, key_len, false);
	     s_ret == REDIS_RCODE_TRY_AGAIN;	/* Continue */
	     s_ret = fr_redis_cluster_state_next(&state, &conn, inst->cluster, request, status, &reply)) {
		reply = redisCommandArgv(conn->handle, argc, argv, NULL);
		status = fr_redis_command_status(conn, reply);
	}
	if (s_ret != REDIS_RCODE_SUCCESS) {
		RERROR("Failed inserting accounting data");
	error:
		fr_redis_reply_free(&reply);
		return -1;
	}
	if (!fr_cond_assert(reply)) goto error;

	/*
	 *	Write the response to the debug log
	 */
	fr_redis_reply_print(L_DBG_LVL_2, reply, request, 0, status);

	switch (reply->type) {
	case REDIS_REPLY_ERROR:
		break;

	case REDIS_REPLY_INTEGER:
		if (reply->integer > 0) ret = reply->integer;
		break;

	/*
	 *	We don't know to interpret this, the user has probably messed
	 *	up the queries, so print an error message and fail.
	 */
	default:
		REDEBUG("Expected type \"integer\" got type \"%s\"",
			fr_table_str_by_value(redis_reply_types, reply->type, "<UNKNOWN>"));
		break;
	}
	fr_redis_reply_free(&reply);

	return ret;
}

static unlang_action_t mod_accounting_all(unlang_result_t *p_result, rlm_rediswho_t const *inst, request_t *request,
					  char const *insert,
					  char const *trim,
					  char const *expire)
{
	int ret;

	ret = rediswho_command(inst, request, insert);
	if (ret < 0) RETURN_UNLANG_FAIL;

	/* Only trim if necessary */
	if (trim && (inst->trim_count >= 0) && (ret > inst->trim_count)) {
		if (rediswho_command(inst, request, trim) < 0) RETURN_UNLANG_FAIL;
	}

	if (rediswho_command(inst, request, expire) < 0) RETURN_UNLANG_FAIL;
	RETURN_UNLANG_OK;
}

static unlang_action_t CC_HINT(nonnull) mod_accounting(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_rediswho_t const		*inst = talloc_get_type_abort_const(mctx->mi->data, rlm_rediswho_t);
	CONF_SECTION			*conf = mctx->mi->conf;
	fr_pair_t			*vp;
	fr_dict_enum_value_t const	*dv;
	CONF_SECTION			*cs;
	char const			*insert, *trim, *expire;

	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_acct_status_type);
	if (!vp) {
		RDEBUG2("Could not find account status type in packet");
		RETURN_UNLANG_NOOP;
	}

	dv = fr_dict_enum_by_value(vp->da, &vp->data);
	if (!dv) {
		RDEBUG2("Unknown Acct-Status-Type %u", vp->vp_uint32);
		RETURN_UNLANG_NOOP;
	}

	cs = cf_section_find(conf, dv->name, NULL);
	if (!cs) {
		RDEBUG2("No subsection %s", dv->name);
		RETURN_UNLANG_NOOP;
	}

	insert = cf_pair_value(cf_pair_find(cs, "insert"));
	trim = cf_pair_value(cf_pair_find(cs, "trim"));
	expire = cf_pair_value(cf_pair_find(cs, "expire"));

	if (!insert) {
		RDEBUG("No 'insert' query - ignoring");
		RETURN_UNLANG_NOOP;
	}

	if (!expire) {
		RDEBUG("No 'expire' query - ignoring");
		RETURN_UNLANG_NOOP;
	}

	return mod_accounting_all(p_result, inst, request, insert, trim, expire);
}

static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_rediswho_t	*inst = talloc_get_type_abort(mctx->mi->data, rlm_rediswho_t);
	CONF_SECTION	*conf = mctx->mi->conf;

	inst->cluster = fr_redis_cluster_alloc(inst, conf, &inst->conf, true, NULL, NULL, NULL);
	if (!inst->cluster) return -1;

	return 0;
}

static int mod_load(void)
{
	fr_redis_version_print();

	return 0;
}

extern module_rlm_t rlm_rediswho;
module_rlm_t rlm_rediswho = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "rediswho",
		.inst_size	= sizeof(rlm_rediswho_t),
		.config		= module_config,
		.onload		= mod_load,
		.instantiate	= mod_instantiate
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{ .section = SECTION_NAME("accounting", CF_IDENT_ANY), .method = mod_accounting },
			MODULE_BINDING_TERMINATOR
		}
	}
};
