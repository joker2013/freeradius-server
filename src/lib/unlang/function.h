#pragma once
/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/**
 * $Id$
 *
 * @file unlang/function.h
 * @brief Declarations for generic unlang functions.
 *
 * These are a useful alternative to module methods for library code.
 * They're more light weight, and don't require instance data lookups
 * to function.
 *
 * @copyright 2021 The FreeRADIUS server project
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <freeradius-devel/unlang/interpret.h>
#include <freeradius-devel/server/rcode.h>
#include <freeradius-devel/server/request.h>
#include <freeradius-devel/server/signal.h>


typedef enum {
	UNLANG_FUNCTION_TYPE_WITH_RESULT,	//!< Function with a result.
	UNLANG_FUNCTION_TYPE_NO_RESULT,		//!< Function without a result.
} unlang_function_type_t;

/** A generic function pushed by a module or xlat to functions deeper in the C call stack to create resumption points
 *
 * @param[in] p_result		The module return code and priority.
 * @param[in] request		The current request.
 * @param[in,out] uctx		Provided by whatever pushed the function.  Is opaque to the
 *				interpreter, but should be usable by the function.
 *				All input (args) and output will be done using this structure.
 * @return an #unlang_action_t.
 */
typedef unlang_action_t (*unlang_function_with_result_t)(unlang_result_t *p_result, request_t *request, void *uctx);

/** A generic function pushed by a module or xlat to functions deeper in the C call stack to create resumption points
 *
 * @note Returning UNLANG_ACTION_FAIL has an identical effect to returning UNLANG_ACTION_CALCULATE_RESULT
 *	 and will not be visible to the caller.
 *
 * @param[in] request		The current request.
 * @param[in,out] uctx		Provided by whatever pushed the function.  Is opaque to the
 *				interpreter, but should be usable by the function.
 *				All input (args) and output will be done using this structure.
 * @return an #unlang_action_t.
 */
typedef unlang_action_t (*unlang_function_no_result_t)(request_t *request, void *uctx);

/** Function to call if the request was signalled
 *
 * @param[in] request		The current request.
 * @param[in] action		We're being signalled with.
 * @param[in,out] uctx		Provided by whatever pushed the function.  Is opaque to the
 *				interpreter, but should be usable by the function.
 *				All input (args) and output will be done using this structure.
 */
typedef void (*unlang_function_signal_t)(request_t *request, fr_signal_t action, void *uctx);

int		unlang_function_clear(request_t *request) CC_HINT(warn_unused_result);

/** Set a new signal function for an existing function frame
 *
 * The function frame being modified must be at the top of the stack.
 *
 * @param[in] _request		The current request.
 * @param[in] _signal		The signal function to set.
 * @param[in] _sigmask		Signals to block.
 * @return
 *	- 0 on success.
 *      - -1 on failure.
 */
#define		unlang_function_signal_set(_request, _signal, _sigmask) \
		_unlang_function_signal_set(_request, _signal, _sigmask, STRINGIFY(_signal))
int		_unlang_function_signal_set(request_t *request, unlang_function_signal_t signal, fr_signal_t sigmask, char const *name)
		CC_HINT(warn_unused_result);

/** Set a new repeat function for an existing function frame
 *
 * The function frame being modified must be at the top of the stack.
 *
 * @param[in] _request		The current request.
 * @param[in] _repeat		the repeat function to set.
 * @return
 *	- 0 on success.
 *      - -1 on failure.
 */
#define		unlang_function_repeat_set(_request, _repeat) \
		_Generic((&(_repeat)), \
			unlang_function_with_result_t: _unlang_function_repeat_set(_request,\
										   (void *)(_repeat), \
										   STRINGIFY(_repeat), \
										   UNLANG_FUNCTION_TYPE_WITH_RESULT), \
			unlang_function_no_result_t: _unlang_function_repeat_set(_request,\
										 (void *)(_repeat), \
										 STRINGIFY(_repeat), \
										 UNLANG_FUNCTION_TYPE_NO_RESULT) \
		)
int		_unlang_function_repeat_set(request_t *request, void *repeat, char const *name, unlang_function_type_t type)
		CC_HINT(warn_unused_result);

/** Push a generic function onto the unlang stack that produces a result
 *
 * These can be pushed by any other type of unlang op to allow a submodule or function
 * deeper in the C call stack to establish a new resumption point.
 *
 * @note If you're pushing a function onto the stack to resume execution in a module, you're probably
 *	 doing it wrong.  Use unlang_module_yield() instead, and change the process function for the
 *	 module.
 *
 * @param[in] _result_p		Where to write the result.
 * @param[in] _request		The current request.
 * @param[in] _func		to call going up the stack.
 * @param[in] _repeat		function to call going back down the stack (may be NULL).
 *				This may be the same as func.
 * @param[in] _signal		function to call if the request is signalled.
 * @param[in] _sigmask		Signals to block.
 * @param[in] _top_frame	Return out of the unlang interpreter when popping this frame.
 * @param[in] _uctx		to pass to func(s).
 * @return
 *	- UNLANG_ACTION_PUSHED_CHILD on success.
 *	- UNLANG_ACTION_FAIL on failure.
 */
#define	unlang_function_push_with_result(_result_p, _request, _func, _repeat, _signal, _sigmask, _top_frame, _uctx) \
		_unlang_function_push_with_result(_result_p, _request, \
						  _func, STRINGIFY(_func), \
						  _repeat, STRINGIFY(_repeat), \
						  _signal, _sigmask, STRINGIFY(_signal), \
						  _top_frame, _uctx)

unlang_action_t _unlang_function_push_with_result(unlang_result_t *p_result,
						  request_t *request,
						  unlang_function_with_result_t func, char const *func_name,
						  unlang_function_with_result_t repeat, char const *repeat_name,
						  unlang_function_signal_t signal, fr_signal_t sigmask, char const *signal_name,
						  bool top_frame, void *uctx) CC_HINT(warn_unused_result);

/** Push a generic function onto the unlang stack
 *
 * These can be pushed by any other type of unlang op to allow a submodule or function
 * deeper in the C call stack to establish a new resumption point.
 *
 * @note If you're pushing a function onto the stack to resume execution in a module, you're probably
 *	 doing it wrong.  Use unlang_module_yield() instead, and change the process function for the
 *	 module.
 *
 * @param[in] _request		The current request.
 * @param[in] _func		to call going up the stack.
 * @param[in] _repeat		function to call going back down the stack (may be NULL).
 *				This may be the same as func.
 * @param[in] _signal		function to call if the request is signalled.
 * @param[in] _sigmask		Signals to block.
 * @param[in] _top_frame	Return out of the unlang interpreter when popping this frame.
 * @param[in] _uctx		to pass to func(s).
 * @return
 *	- UNLANG_ACTION_PUSHED_CHILD on success.
 *	- UNLANG_ACTION_FAIL on failure.
 */
#define	unlang_function_push(_request, _func, _repeat, _signal, _sigmask, _top_frame, _uctx) \
		_unlang_function_push_no_result(_request, \
						_func, STRINGIFY(_func), \
						_repeat, STRINGIFY(_repeat), \
						_signal, _sigmask, STRINGIFY(_signal), \
						 _top_frame, _uctx)

unlang_action_t _unlang_function_push_no_result(request_t *request,
						unlang_function_no_result_t func, char const *func_name,
						unlang_function_no_result_t repeat, char const *repeat_name,
						unlang_function_signal_t signal, fr_signal_t sigmask, char const *signal_name,
						bool top_frame, void *uctx) CC_HINT(warn_unused_result);
#ifdef __cplusplus
}
#endif
