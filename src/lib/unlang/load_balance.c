/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
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
 *
 * @file unlang/load_balance.c
 * @brief Implementation of the unlang "load-balance" keyword.
 *
 * @copyright 2006-2019 The FreeRADIUS server project
 */
#include <freeradius-devel/server/rcode.h>
#include <freeradius-devel/util/hash.h>
#include <freeradius-devel/util/rand.h>

#include "load_balance_priv.h"
#include "module_priv.h"

#define unlang_redundant_load_balance unlang_load_balance

static unlang_action_t unlang_load_balance_next(unlang_result_t *p_result, request_t *request,
						unlang_stack_frame_t *frame)
{
	unlang_frame_state_redundant_t	*redundant = talloc_get_type_abort(frame->state, unlang_frame_state_redundant_t);
	unlang_group_t			*g = unlang_generic_to_group(frame->instruction);

#ifdef STATIC_ANALYZER
	if (!redundant->found) RETURN_UNLANG_FAIL;
#endif
	/*
	 *	Set up the first round versus subsequent ones.
	 */
	if (!redundant->child) {
		redundant->child = redundant->found;

	} else {
		/*
		 *	child is NULL on the first pass.  But if it's
		 *	back to the found one, then we're done.
		 */
		if (redundant->child == redundant->found) {
			/* DON'T change p_result, as it is taken from the child */
			return UNLANG_ACTION_CALCULATE_RESULT;
		}

		RDEBUG4("%s resuming", frame->instruction->debug_name);

		/*
		 *	We are in a resumed frame.  The module we
		 *	chose failed, so we have to go through the
		 *	process again.
		 */

		fr_assert(frame->instruction->type != UNLANG_TYPE_LOAD_BALANCE); /* this is never called again */

		/*
		 *	If the current child says "return", then do
		 *	so.
		 */
		if ((p_result->rcode != RLM_MODULE_NOT_SET) &&
		    (redundant->child->actions.actions[p_result->rcode] == MOD_ACTION_RETURN)) {
			/* DON'T change p_result, as it is taken from the child */
			return UNLANG_ACTION_CALCULATE_RESULT;
		}
	}

	/*
	 *	Push the child, and yield for a later return.
	 */
	if (unlang_interpret_push(NULL, request, redundant->child,
				  FRAME_CONF(RLM_MODULE_NOT_SET, UNLANG_SUB_FRAME), UNLANG_NEXT_STOP) < 0) {
		return UNLANG_ACTION_STOP_PROCESSING;
	}

	/*
	 *	Now that we've pushed this child, make the next call
	 *	use the next child, wrapping around to the beginning.
	 *
	 *	@todo - track the one we chose, and if it fails, do
	 *	the load-balancing again, except this time skipping
	 *	the failed module.  AND, keep track of multiple failed
	 *	modules in the unlang_frame_state_redundant_t
	 *	structure.
	 */
	redundant->child = redundant->child->next;
	if (!redundant->child) redundant->child = g->children;

	repeatable_set(frame);

	return UNLANG_ACTION_PUSHED_CHILD;
}

static unlang_action_t unlang_load_balance(unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_frame_state_redundant_t	*redundant;
	unlang_group_t			*g = unlang_generic_to_group(frame->instruction);
	unlang_load_balance_t		*gext = NULL;

	uint32_t count = 0;

	if (!g->num_children) RETURN_UNLANG_NOOP;

	gext = unlang_group_to_load_balance(g);

	RDEBUG4("%s setting up", frame->instruction->debug_name);

	redundant = talloc_get_type_abort(frame->state,
					  unlang_frame_state_redundant_t);

	if (gext && gext->vpt) {
		uint32_t hash, start;
		ssize_t slen;
		char buffer[1024];

		/*
		 *	Integer data types let the admin
		 *	select which frame is being used.
		 */
		if (tmpl_is_attr(gext->vpt) &&
		    ((tmpl_attr_tail_da(gext->vpt)->type == FR_TYPE_UINT8) ||
		     (tmpl_attr_tail_da(gext->vpt)->type == FR_TYPE_UINT16) ||
		     (tmpl_attr_tail_da(gext->vpt)->type == FR_TYPE_UINT32) ||
		     (tmpl_attr_tail_da(gext->vpt)->type == FR_TYPE_UINT64))) {
			fr_pair_t *vp;

			slen = tmpl_find_vp(&vp, request, gext->vpt);
			if (slen < 0) {
				REDEBUG("Failed finding attribute %s", gext->vpt->name);
				goto randomly_choose;
			}

			switch (tmpl_attr_tail_da(gext->vpt)->type) {
			case FR_TYPE_UINT8:
				start = ((uint32_t) vp->vp_uint8) % g->num_children;
				break;

			case FR_TYPE_UINT16:
				start = ((uint32_t) vp->vp_uint16) % g->num_children;
				break;

			case FR_TYPE_UINT32:
				start = vp->vp_uint32 % g->num_children;
				break;

			case FR_TYPE_UINT64:
				start = (uint32_t) (vp->vp_uint64 % ((uint64_t) g->num_children));
				break;

			default:
				goto randomly_choose;
			}

		} else {
			uint8_t *octets = NULL;

			/*
			 *	If the input is an IP address, prefix, etc., we don't need to convert it to a
			 *	string.  We can just hash the raw data directly.
			 */
			slen = tmpl_expand(&octets, buffer, sizeof(buffer), request, gext->vpt);
			if (slen <= 0) goto randomly_choose;

			hash = fr_hash(octets, slen);

			start = hash % g->num_children;
		}

		RDEBUG3("load-balance starting at child %d", (int) start);

		count = 0;
		for (redundant->child = redundant->found = g->children;
		     redundant->child != NULL;
		     redundant->child = redundant->child->next) {
			count++;
			if (count == start) {
				redundant->found = redundant->child;
				break;
			}
		}

	} else {
	randomly_choose:
		count = 0;

		/*
		 *	Choose a child at random.
		 *
		 *	@todo - leverage the "power of 2", as per
		 *      lib/io/network.c.  This is good enough for
		 *      most purposes.  However, in order to do this,
		 *      we need to track active callers across
		 *      *either* multiple modules in one thread, *or*
		 *      across multiple threads.
		 *
		 *	We don't have thread-specific instance data
		 *	for this load-balance section.  So for now,
		 *	just pick a random child.
		 */
		for (redundant->child = redundant->found = g->children;
		     redundant->child != NULL;
		     redundant->child = redundant->child->next) {
			count++;

			if ((count * (fr_rand() & 0xffffff)) < (uint32_t) 0x1000000) {
				redundant->found = redundant->child;
			}
		}
	}

	/*
	 *	Plain "load-balance".  Just do one child.
	 */
	if (frame->instruction->type == UNLANG_TYPE_LOAD_BALANCE) {
		if (unlang_interpret_push(NULL, request, redundant->found,
					  FRAME_CONF(RLM_MODULE_NOT_SET, UNLANG_SUB_FRAME), UNLANG_NEXT_STOP) < 0) {
			return UNLANG_ACTION_STOP_PROCESSING;
		}
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	"redundant" and "redundant-load-balance" starts at
	 *	"found", but we need to indicate that we're at the
	 *	first child.
	 */
	redundant->child = NULL;

	frame->process = unlang_load_balance_next;
	return unlang_load_balance_next(p_result, request, frame);
}


static unlang_t *compile_load_balance_subsection(unlang_t *parent, unlang_compile_ctx_t *unlang_ctx, CONF_SECTION *cs,
						 unlang_type_t type)
{
	char const			*name2;
	unlang_t			*c;
	unlang_group_t			*g;
	unlang_load_balance_t		*gext;

	tmpl_rules_t			t_rules;

	/*
	 *	We allow unknown attributes here.
	 */
	t_rules = *(unlang_ctx->rules);
	t_rules.attr.allow_unknown = true;
	RULES_VERIFY(&t_rules);

	/*
	 *	No children?  Die!
	 */
	if (!cf_item_next(cs, NULL)) {
		cf_log_err(cs, "%s sections cannot be empty", unlang_ops[type].name);
		return NULL;
	}

	if (!unlang_compile_limit_subsection(cs, cf_section_name1(cs))) return NULL;

	c = unlang_compile_section(parent, unlang_ctx, cs, type);
	if (!c) return NULL;

	g = unlang_generic_to_group(c);

	/*
	 *	Allow for keyed load-balance / redundant-load-balance sections.
	 */
	name2 = cf_section_name2(cs);

	/*
	 *	Inside of the "modules" section, it's a virtual
	 *	module.  The name is a module name, not a key.
	 */
	if (name2) {
		if (strcmp(cf_section_name1(cf_item_to_section(cf_parent(cs))), "modules") == 0) name2 = NULL;
	}

	if (name2) {
		fr_token_t quote;
		ssize_t slen;

		/*
		 *	Create the template.  All attributes and xlats are
		 *	defined by now.
		 */
		quote = cf_section_name2_quote(cs);
		gext = unlang_group_to_load_balance(g);
		slen = tmpl_afrom_substr(gext, &gext->vpt,
					 &FR_SBUFF_IN(name2, strlen(name2)),
					 quote,
					 NULL,
					 &t_rules);
		if (!gext->vpt) {
			cf_canonicalize_error(cs, slen, "Failed parsing argument", name2);
			talloc_free(g);
			return NULL;
		}

		fr_assert(gext->vpt != NULL);

		/*
		 *	Fixup the templates
		 */
		if (!pass2_fixup_tmpl(g, &gext->vpt, cf_section_to_item(cs), unlang_ctx->rules->attr.dict_def)) {
			talloc_free(g);
			return NULL;
		}

		switch (gext->vpt->type) {
		default:
			cf_log_err(cs, "Invalid type in '%s': data will not result in a load-balance key", name2);
			talloc_free(g);
			return NULL;

			/*
			 *	Allow only these ones.
			 */
		case TMPL_TYPE_XLAT:
		case TMPL_TYPE_ATTR:
		case TMPL_TYPE_EXEC:
			break;
		}
	}

	return c;
}

static unlang_t *unlang_compile_load_balance(unlang_t *parent, unlang_compile_ctx_t *unlang_ctx, CONF_ITEM const *ci)
{
	return compile_load_balance_subsection(parent, unlang_ctx, cf_item_to_section(ci), UNLANG_TYPE_LOAD_BALANCE);
}


static unlang_t *unlang_compile_redundant_load_balance(unlang_t *parent, unlang_compile_ctx_t *unlang_ctx, CONF_ITEM const *ci)
{
	return compile_load_balance_subsection(parent, unlang_ctx, cf_item_to_section(ci), UNLANG_TYPE_REDUNDANT_LOAD_BALANCE);
}


void unlang_load_balance_init(void)
{
	unlang_register(&(unlang_op_t){
			.name = "load-balance",
			.type = UNLANG_TYPE_LOAD_BALANCE,
			.flag = UNLANG_OP_FLAG_DEBUG_BRACES | UNLANG_OP_FLAG_RCODE_SET,

			.compile = unlang_compile_load_balance,
			.interpret = unlang_load_balance,

			.unlang_size = sizeof(unlang_load_balance_t),
			.unlang_name = "unlang_load_balance_t",

			.frame_state_size = sizeof(unlang_frame_state_redundant_t),
			.frame_state_type = "unlang_frame_state_redundant_t",
		});

	unlang_register(&(unlang_op_t){
			.name = "redundant-load-balance",
			.type = UNLANG_TYPE_REDUNDANT_LOAD_BALANCE,	
			.flag = UNLANG_OP_FLAG_DEBUG_BRACES | UNLANG_OP_FLAG_RCODE_SET,

			.compile = unlang_compile_redundant_load_balance,
			.interpret = unlang_redundant_load_balance,

			.unlang_size = sizeof(unlang_load_balance_t),
			.unlang_name = "unlang_load_balance_t",

			.frame_state_size = sizeof(unlang_frame_state_redundant_t),
			.frame_state_type = "unlang_frame_state_redundant_t",
		});
}
