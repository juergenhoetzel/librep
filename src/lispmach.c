/* lispmach.c -- Interpreter for compiled Lisp forms
   Copyright (C) 1993, 1994 John Harper <john@dcs.warwick.ac.uk>
   $Id$

   This file is part of Jade.

   Jade is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   Jade is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Jade; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

#define _GNU_SOURCE

/* AIX requires this to be the first thing in the file.  */
#include <config.h>
#ifdef __GNUC__
# define alloca __builtin_alloca
#else
# if HAVE_ALLOCA_H
#  include <alloca.h>
# else
#  ifdef _AIX
 #pragma alloca
#  else
#   ifndef alloca /* predefined by HP cc +Olibcalls */
char *alloca ();
#   endif
#  endif
# endif
#endif

#include "repint.h"
#include "bytecodes.h"
#include <assert.h>

/* Define this to get a list of byte-code/frequency of use */
#undef BYTE_CODE_HISTOGRAM

/* Define this to check if the compiler reserves enough stack */
#undef CHECK_STACK_USAGE

DEFSYM(bytecode_error, "bytecode-error");
DEFSYM(jade_byte_code, "jade-byte-code");
DEFSTRING(err_bytecode_error, "Invalid byte code version");
DEFSTRING(unknown_op, "Unknown lisp opcode");

#ifdef BYTE_CODE_HISTOGRAM
static u_long byte_code_usage[256];
#endif


/* Helper functions

   Note the careful use of inlining.. the icache is crucial, we want
   the VM to be as small as possible, so that as much other code as
   possible fits in cache as well. However, if a helper function is
   only called once (or maybe is in a crucial path), then inline it..

   The speedup from this (_not_ inlining everything) is _huge_ */


/* Unbind one level of the BIND-STACK and return the new head of the stack.
   Each item in the BIND-STACK may be one of:
	INTEGER
		variable binding frame
	(error . (PC . STACK-DEPTH))
		not unbound here; install exception handler at PC

   returns the number of dynamic bindings removed */
static inline int
inline_unbind_object(repv item)
{
    if(rep_INTP(item))
    {
	/* A set of symbol bindings (let or let*). */
	return rep_unbind_symbols(item);
    }
    if(rep_CONSP(item))
    {
	if (rep_CAR(item) == Qerror)
	    return 0;
	else
	{
	    rep_type *t = rep_get_data_type(rep_TYPE(rep_CAR(item)));
	    if (t->unbind != 0)
		t->unbind(item);
	    return 1;
	}
    }
    else
    {
	rep_type *t = rep_get_data_type(rep_TYPE(item));
	if (t->unbind != 0)
	    t->unbind(item);
	return 1;
    }
}

int
rep_unbind_object (repv item)
{
    return inline_unbind_object (item);
}

/* Bind one object, returning the handle to later unbind by. */
repv
rep_bind_object(repv obj)
{
    rep_type *t = rep_get_data_type(rep_TYPE(obj));
    if (t->bind != 0)
	return t->bind(obj);
    else
	return Qnil;
}

static inline void
unbind_n (repv *ptr, int n)
{
    while (n-- > 0)
	rep_unbind_object (ptr[n]);
}

/* Walk COUNT entries down the environment */
static inline repv
snap_environment (int count)
{
    register repv ptr = rep_env;
    while (count-- > 0)
	ptr = rep_CDR(ptr);
    return rep_CAR(ptr);
}

static repv
search_special_bindings (repv sym)
{
    register repv env = rep_special_bindings;
    while (env != Qnil && rep_CAAR(env) != sym)
	env = rep_CDR(env);
    return env != Qnil ? rep_CAR(env) : env;
}


/* Lisp VM. */

static inline repv
list_ref (repv list, int elt)
{
    while (rep_CONSP(list) && elt-- > 0)
	list = rep_CDR(list);
    return rep_CONSP(list) ? rep_CAR(list) : Qnil;
}

#define TOP	    (*stackp)
#define RET_POP	    (*stackp--)
#define POP	    (stackp--)
#define POPN(n)	    (stackp -= n)
#define PUSH(v)	    (*(++stackp) = (v))
#define STK_USE	    (stackp - (stackbase - 1))

#define BIND_USE	(bindp - (bindbase - 1))
#define BIND_RET_POP	(*bindp--)
#define BIND_TOP	(*bindp)
#define BIND_TOP_P	(bindp < bindbase)
#define BIND_PUSH(x)	(*(++bindp) = (x))

#define FETCH	    (*pc++)
#define FETCH2(var) ((var) = (FETCH << ARG_SHIFT), (var) += FETCH)

#define SYNC_GC				\
    do {				\
	gc_stackbase.count = STK_USE;	\
	gc_bindbase.count = BIND_USE;	\
    } while (0)

/* These macros pop as many args as required then call the specified
   function properly. */

#define CALL_1(cmd)	\
    TOP = cmd (TOP);	\
    break;
    
#define CALL_2(cmd)		\
    tmp = RET_POP;		\
    TOP = cmd (TOP, tmp);	\
    break;

#define CALL_3(cmd)			\
    tmp = RET_POP;			\
    tmp2 = RET_POP;			\
    TOP = cmd (TOP, tmp2, tmp);		\
    break;

/* Output the case statement for an instruction OP, with an embedded
   argument. The code for the instruction should start at the following
   piece of code. */
#define CASE_OP_ARG(op)							\
	case op+7:							\
	    FETCH2(arg); goto rep_CONCAT(op_, op);			\
	case op+6:							\
	    arg = FETCH; goto rep_CONCAT(op_, op);			\
	case op: case op+1: case op+2: case op+3: case op+4: case op+5:	\
	    arg = insn - op;						\
	rep_CONCAT(op_, op):

DEFSTRING(max_depth, "max-lisp-depth exceeded, possible infinite recursion?");

DEFUN("jade-byte-code", Fjade_byte_code, Sjade_byte_code,
      (repv code, repv consts, repv stkreq, repv frame), rep_Subr4) /*
::doc:jade-byte-code::
jade-byte-code CODE-STRING CONST-VEC MAX-STACK [FRAME]

Evaluates the string of byte codes CODE-STRING, the constants that it
references are contained in the vector CONST-VEC. MAX-STACK is a number
defining how much stack space is required to evaluate the code.

Do *not* attempt to call this function manually, the lisp file `compiler.jl'
contains a simple compiler which translates files of lisp forms into files
of byte code. See the functions `compile-file', `compile-directory' and
`compile-lisp-lib' for more details.
::end:: */
{
    register u_char *pc;
    rep_GC_root gc_code, gc_consts;
    /* The `gcv_N' field is only filled in with the stack-size when there's
       a chance of gc.	*/
    rep_GC_n_roots gc_stackbase, gc_bindbase;

    /* this is the number of dynamic `bindings' in effect
       (including non-variable bindings). */
    int impurity;

    rep_DECLARE3(stkreq, rep_INTP);

    if(++rep_lisp_depth > rep_max_lisp_depth)
    {
	rep_lisp_depth--;
	return Fsignal(Qerror, rep_LIST_1(rep_VAL(&max_depth)));
    }

    /* Jump to this label when tail-calling but the current stack
       is insufficiently large */
again_stack: {
    register repv *stackp;
    repv *stackbase;
    register repv *bindp;
    repv *bindbase;

#if defined (__GNUC__)
    /* Using GCC's variable length auto arrays is better for this since
       the stack space is freed when leaving the containing scope */
    repv stack[(rep_INT (stkreq) & 0xffff) + 1];
    repv bindstack[(rep_INT (stkreq) >> 16) + 1];
#else
    /* Otherwise just use alloca (). When tail-calling we'll only
       allocate a new stack if the current is too small. */
    repv *stack = alloca(sizeof(repv) * ((rep_INT(stkreq) & 0xffff) + 1));
    repv *bindstack = alloca(sizeof(repv) * ((rep_INT(stkreq) >> 16) + 1));
#endif

    /* Make sure that even when the stack has no entries, the TOP
       element still != 0 (for the error-detection at label quit:) */
    stack[0] = Qt;

    /* Jump to this label when tail-calling with a large enough stack */
again:
    rep_DECLARE1(code, rep_STRINGP);
    rep_DECLARE2(consts, rep_VECTORP);

    /* Initialize the frame and stack pointers */
    stackbase = stack + 1;
    stackp = stackbase - 1;
    bindbase = bindstack;
    bindp = bindbase - 1;

    /* Push the binding frame of the function arguments */
    BIND_PUSH (frame);
    impurity = rep_SPEC_BINDINGS (frame);

    rep_PUSHGC(gc_code, code);
    rep_PUSHGC(gc_consts, consts);
    rep_PUSHGCN(gc_bindbase, bindbase, BIND_USE);
    rep_PUSHGCN(gc_stackbase, stackbase, STK_USE);

    if(rep_data_after_gc >= rep_gc_threshold)
	Fgarbage_collect(Qt);

    rep_MAY_YIELD;

    pc = rep_STR(code);

    while (1)
    {
	int insn;

	/* Some instructions jump straight back to this label after
	   completion; this is only allowed if it's impossible for
	   the instruction to have raised an exception. */
    fetch:
	insn = FETCH;

#ifdef BYTE_CODE_HISTOGRAM
	byte_code_usage[insn]++;
#endif

#ifdef CHECK_STACK_USAGE
        assert (STK_USE <= (rep_INT(stkreq) & 0xffff));
        assert (BIND_USE <= (rep_INT(stkreq) >> 16) + 1);
#endif

	switch(insn)
	{
	    int arg;
	    repv tmp, tmp2;
	    struct rep_Call lc;
	    rep_bool was_closed;

	CASE_OP_ARG(OP_CALL)
	    /* args are still available above the top of the stack,
	       this just makes things a bit easier. */
	    POPN(arg);
	    tmp = TOP;
	    lc.fun = tmp;
	    lc.args = Qnil;
	    lc.args_evalled_p = Qt;
	    rep_PUSH_CALL (lc);
	    SYNC_GC;

	    was_closed = rep_FALSE;
	    if (rep_FUNARGP(tmp))
	    {
		rep_USE_FUNARG(tmp);
		tmp = rep_FUNARG(tmp)->fun;
		was_closed = rep_TRUE;
	    }

	    switch(rep_TYPE(tmp))
	    {
	    case rep_Subr0:
		TOP = rep_SUBR0FUN(tmp)();
		break;

	    case rep_Subr1:
		TOP = rep_SUBR1FUN(tmp)(arg >= 1 ? stackp[1] : Qnil);
		break;

	    case rep_Subr2:
		switch(arg)
		{
		case 0:
		    TOP = rep_SUBR2FUN(tmp)(Qnil, Qnil);
		    break;
		case 1:
		    TOP = rep_SUBR2FUN(tmp)(stackp[1], Qnil);
		    break;
		default:
		    TOP = rep_SUBR2FUN(tmp)(stackp[1], stackp[2]);
		    break;
		}
		break;

	    case rep_Subr3:
		switch(arg)
		{
		case 0:
		    TOP = rep_SUBR3FUN(tmp)(Qnil, Qnil, Qnil);
		    break;
		case 1:
		    TOP = rep_SUBR3FUN(tmp)(stackp[1], Qnil, Qnil);
		    break;
		case 2:
		    TOP = rep_SUBR3FUN(tmp)(stackp[1], stackp[2], Qnil);
		    break;
		default:
		    TOP = rep_SUBR3FUN(tmp)(stackp[1], stackp[2], stackp[3]);
		    break;
		}
		break;

	    case rep_Subr4:
		switch(arg)
		{
		case 0:
		    TOP = rep_SUBR4FUN(tmp)(Qnil, Qnil,
					 Qnil, Qnil);
		    break;
		case 1:
		    TOP = rep_SUBR4FUN(tmp)(stackp[1], Qnil,
					 Qnil, Qnil);
		    break;
		case 2:
		    TOP = rep_SUBR4FUN(tmp)(stackp[1], stackp[2],
					 Qnil, Qnil);
		    break;
		case 3:
		    TOP = rep_SUBR4FUN(tmp)(stackp[1], stackp[2],
					 stackp[3], Qnil);
		    break;
		default:
		    TOP = rep_SUBR4FUN(tmp)(stackp[1], stackp[2],
					 stackp[3], stackp[4]);
		    break;
		}
		break;

	    case rep_Subr5:
		switch(arg)
		{
		case 0:
		    TOP = rep_SUBR5FUN(tmp)(Qnil, Qnil, Qnil,
					 Qnil, Qnil);
		    break;
		case 1:
		    TOP = rep_SUBR5FUN(tmp)(stackp[1], Qnil, Qnil,
					 Qnil, Qnil);
		    break;
		case 2:
		    TOP = rep_SUBR5FUN(tmp)(stackp[1], stackp[2], Qnil,
					 Qnil, Qnil);
		    break;
		case 3:
		    TOP = rep_SUBR5FUN(tmp)(stackp[1], stackp[2], stackp[3],
					 Qnil, Qnil);
		    break;
		case 4:
		    TOP = rep_SUBR5FUN(tmp)(stackp[1], stackp[2], stackp[3],
					 stackp[4], Qnil);
		    break;
		default:
		    TOP = rep_SUBR5FUN(tmp)(stackp[1], stackp[2], stackp[3],
					 stackp[4], stackp[5]);
		    break;
		}
		break;

	    case rep_SubrN:
		tmp2 = Qnil;
		POPN(-arg); /* reclaim my args */
		while(arg--)
		    tmp2 = Fcons(RET_POP, tmp2);
		lc.args = tmp2;
		TOP = rep_SUBRNFUN(tmp)(tmp2);
		break;

	    default:
		tmp2 = Qnil;
		if (rep_CONSP(tmp))
		{
		    POPN(-arg);
		    while (arg--)
			tmp2 = Fcons (RET_POP, tmp2);
		    lc.args = tmp2;
		    if(was_closed && rep_CAR(tmp) == Qlambda)
			TOP = rep_eval_lambda(tmp, tmp2, rep_FALSE, rep_FALSE);
		    else if(rep_CAR(tmp) == Qautoload)
		    {
			/* I can't be bothered to go to all the hassle
			   of doing this here, it's going to be slow
			   anyway so just pass it to rep_funcall.  */
			rep_POP_CALL(lc);
			TOP = rep_funcall(TOP, tmp2, rep_FALSE);
			goto end;
		    }
		    else
			goto invalid;
		}
		else if (was_closed && rep_COMPILEDP(tmp))
		{
		    repv bindings;

		    if (rep_bytecode_interpreter == 0)
			goto invalid;

		    if (impurity != 0 || *pc != OP_RETURN)
		    {
			bindings = (rep_bind_lambda_list_1
				    (rep_COMPILED_LAMBDA(tmp), stackp+1, arg));
			if(bindings != rep_NULL)
			{
			    TOP = (rep_bytecode_interpreter
				   (rep_COMPILED_CODE(tmp),
				    rep_COMPILED_CONSTANTS(tmp),
				    rep_COMPILED_STACK(tmp),
				    bindings));
			}
		    }
		    else
		    {
			/* A tail call that's safe for eliminating */

			/* snap the call stack */
			rep_call_stack = lc.next;
			rep_call_stack->fun = lc.fun;
			rep_call_stack->args = lc.args;
			rep_call_stack->args_evalled_p = lc.args_evalled_p;

			/* since impurity==0 there can only be lexical
			   bindings; these were unbound when switching
			   environments.. */

			bindings = (rep_bind_lambda_list_1
				    (rep_COMPILED_LAMBDA(tmp), stackp+1, arg));
			if(bindings != rep_NULL)
			{
			    int o_req_s, o_req_b;
			    int n_req_s, n_req_b;

			    /* set up parameters */
			    code = rep_COMPILED_CODE (tmp);
			    consts = rep_COMPILED_CONSTANTS (tmp);
			    frame = bindings;

			    rep_POPGCN; rep_POPGCN; rep_POPGC; rep_POPGC;

			    /* do the goto, after deciding if the
			       current stack allocation is sufficient. */
			    n_req_s = rep_INT (rep_COMPILED_STACK (tmp)) & 0xffff;
			    n_req_b = (rep_INT (rep_COMPILED_STACK (tmp)) >> 16) + 1;
			    o_req_s = rep_INT(stkreq) & 0xffff;
			    o_req_b = (rep_INT(stkreq) >> 16) + 1;
			    if (n_req_s > o_req_s || n_req_b > o_req_b)
			    {
				stkreq = rep_COMPILED_STACK(tmp);
				goto again_stack;
			    }
			    else
				goto again;
			}
		    }
		}
		else
		{
		invalid:
		    Fsignal(Qinvalid_function, rep_LIST_1(TOP));
		}
	        break;
	    }
	    rep_POP_CALL(lc);
	    break;

	CASE_OP_ARG(OP_PUSH)
	    PUSH(rep_VECT(consts)->array[arg]);
	    goto fetch;

	CASE_OP_ARG(OP_REFQ)
	    /* this instruction is normally only used for special
	       variables, so optimize the usual path */
	    tmp = rep_VECT(consts)->array[arg];
	    if ((rep_SYM(tmp)->car & rep_SF_SPECIAL)
		&& !(rep_SYM(tmp)->car & rep_SF_LOCAL))
	    {
		/* bytecode interpreter is allowed to assume
		   unrestricted environment.. */
		repv tem = search_special_bindings (tmp);
		if (tem != Qnil)
		{
		    tem = rep_CDR (tem);
		    if (!rep_VOIDP(tem))
		    {
			PUSH (tem);
			goto fetch;
		    }
		}
	    }
	    /* fall back to common case */
	    PUSH(Fsymbol_value(rep_VECT(consts)->array[arg], Qnil));
	    break;

	CASE_OP_ARG(OP_SETQ)
	    /* this instruction is normally only used for special
	       variables, so optimize the usual path */
	    tmp = rep_VECT(consts)->array[arg];
	    if ((rep_SYM(tmp)->car & rep_SF_SPECIAL)
		&& !(rep_SYM(tmp)->car & rep_SF_LOCAL))
	    {
		/* bytecode interpreter is allowed to assume
		   unrestricted environment.. */
		repv tem = search_special_bindings (tmp);
		if (tem != Qnil)
		{
		    rep_CDR (tem) = RET_POP;
		    goto fetch;
		}
	    }
	    /* fall back to common case */
	    Fset(rep_VECT(consts)->array[arg], RET_POP);
	    break;

	CASE_OP_ARG(OP_LIST)
	    tmp = Qnil;
	    while(arg--)
		tmp = Fcons(RET_POP, tmp);
	    PUSH(tmp);
	    goto fetch;

	CASE_OP_ARG(OP_BIND)
	    tmp = rep_VECT(consts)->array[arg];
	    tmp2 = RET_POP;
	    rep_env = Fcons (Fcons (tmp, tmp2), rep_env);
	    BIND_TOP = rep_MARK_LEX_BINDING (BIND_TOP);
	    goto fetch;

	CASE_OP_ARG(OP_BINDSPEC)
	    tmp = rep_VECT(consts)->array[arg];
	    tmp2 = RET_POP;
	    /* assuming non-restricted environment */
	    rep_special_bindings = Fcons (Fcons (tmp, tmp2),
					  rep_special_bindings);
	    BIND_TOP = rep_MARK_SPEC_BINDING (BIND_TOP);
	    impurity++;
	    goto fetch;

	CASE_OP_ARG(OP_REFN)
	    tmp = snap_environment (arg);
	    PUSH(rep_CDR(tmp));
	    goto fetch;

	CASE_OP_ARG(OP_SETN)
	    tmp = snap_environment (arg);
	    rep_CDR(tmp) = RET_POP;
	    goto fetch;

	CASE_OP_ARG(OP_REFG)
	    tmp = F_structure_ref (rep_structure,
				   rep_VECT(consts)->array[arg]);
	    if (!rep_VOIDP(tmp))
	    {
		PUSH(tmp);
		goto fetch;
	    }
	    /* fallback */
	    goto rep_CONCAT(op_,OP_REFQ);

	CASE_OP_ARG(OP_SETG)
	    tmp = rep_VECT(consts)->array[arg];
	    F_structure_set (rep_structure, tmp, RET_POP);
	    goto fetch;

	case OP_REF:
	    TOP = Fsymbol_value(TOP, Qnil);
	    break;

	case OP_SET:
	    CALL_2(Fset);

	case OP_ENCLOSE:
	    TOP = Fmake_closure (TOP, Qnil);
	    break;

	case OP_INIT_BIND:
	    BIND_PUSH (rep_NEW_FRAME);
	    goto fetch;

	case OP_UNBIND:
	    SYNC_GC;
	    impurity -= rep_unbind_object(BIND_RET_POP);
	    break;

	case OP_DUP:
	    tmp = TOP;
	    PUSH(tmp);
	    goto fetch;

	case OP_SWAP:
	    tmp = TOP;
	    TOP = stackp[-1];
	    stackp[-1] = tmp;
	    goto fetch;

	case OP_POP:
	    POP;
	    goto fetch;

	case OP_NIL:
	    PUSH(Qnil);
	    goto fetch;

	case OP_T:
	    PUSH(Qt);
	    goto fetch;

	case OP_CONS:
	    CALL_2(Fcons);

	case OP_CAR:
	    tmp = TOP;
	    if(rep_CONSP(tmp))
		TOP = rep_CAR(tmp);
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_CDR:
	    tmp = TOP;
	    if(rep_CONSP(tmp))
		TOP = rep_CDR(tmp);
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_RPLACA:
	    CALL_2(Frplaca);

	case OP_RPLACD:
	    CALL_2(Frplacd);

	case OP_NTH:
	    CALL_2(Fnth);

	case OP_NTHCDR:
	    CALL_2(Fnthcdr);

	case OP_ASET:
	    CALL_3(Faset);

	case OP_AREF:
	    CALL_2(Faref);

	case OP_LENGTH:
	    CALL_1(Flength);

	case OP_EVAL:
	    SYNC_GC;
	    CALL_1(Feval);

	case OP_ADD:
	    /* open-code fixnum arithmetic */
	    tmp = RET_POP;
	    tmp2 = TOP;
	    if (rep_INTP (tmp) && rep_INTP (tmp2))
	    {
		long x = rep_INT (tmp2) + rep_INT (tmp);
		if (x >= rep_LISP_MIN_INT && x <= rep_LISP_MAX_INT)
		{
		    TOP = rep_MAKE_INT (x);
		    goto fetch;
		}
	    }
	    TOP = rep_number_add (tmp2, tmp);
	    break;

	case OP_NEG:
	    /* open-code fixnum arithmetic */
	    tmp = TOP;
	    if (rep_INTP (tmp))
	    {
		long x = - rep_INT (tmp);
		if (x >= rep_LISP_MIN_INT && x <= rep_LISP_MAX_INT)
		{
		    TOP = rep_MAKE_INT (x);
		    goto fetch;
		}
	    }
	    TOP = rep_number_neg (tmp);
	    break;

	case OP_SUB:
	    /* open-code fixnum arithmetic */
	    tmp = RET_POP;
	    tmp2 = TOP;
	    if (rep_INTP (tmp) && rep_INTP (tmp2))
	    {
		long x = rep_INT (tmp2) - rep_INT (tmp);
		if (x >= rep_LISP_MIN_INT && x <= rep_LISP_MAX_INT)
		{
		    TOP = rep_MAKE_INT (x);
		    goto fetch;
		}
	    }
	    TOP = rep_number_sub (tmp2, tmp);
	    break;

	case OP_MUL:
	    CALL_2(rep_number_mul);

	case OP_DIV:
	    CALL_2(rep_number_div);

	case OP_REM:
	    CALL_2(Fremainder);

	case OP_LNOT:
	    CALL_1(Flognot);

	case OP_NOT:
	case OP_NULL:
	    if(TOP == Qnil)
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_LOR:
	    CALL_2(rep_number_logior);

	case OP_LXOR:
	    CALL_2(rep_number_logxor);

	case OP_LAND:
	    CALL_2(rep_number_logand);

	case OP_EQUAL:
	    tmp = RET_POP;
	    tmp2 = TOP;
	    if (rep_INTP (tmp) && rep_INTP (tmp2))
	    {
		TOP = (tmp2 == tmp) ? Qt : Qnil;
		goto fetch;
	    }
	    if(!(rep_value_cmp(tmp2, tmp)))
		TOP = Qt;
	    else
		TOP = Qnil;
	    break;

	case OP_EQ:
	    tmp = RET_POP;
	    if(TOP == tmp)
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_STRUCT_REF:
	    CALL_2 (F_external_structure_ref);

	case OP_SCM_TEST:
	    TOP = (TOP == rep_scm_f) ? Qnil : Qt;
	    goto fetch;

	case OP_GT:
	    tmp = RET_POP;
	    if(rep_value_cmp(TOP, tmp) > 0)
		TOP = Qt;
	    else
		TOP = Qnil;
	    break;

	case OP_GE:
	    tmp = RET_POP;
	    if(rep_value_cmp(TOP, tmp) >= 0)
		TOP = Qt;
	    else
		TOP = Qnil;
	    break;

	case OP_LT:
	    tmp = RET_POP;
	    if(rep_value_cmp(TOP, tmp) < 0)
		TOP = Qt;
	    else
		TOP = Qnil;
	    break;

	case OP_LE:
	    tmp = RET_POP;
	    if(rep_value_cmp(TOP, tmp) <= 0)
		TOP = Qt;
	    else
		TOP = Qnil;
	    break;

	case OP_INC:
	    tmp = TOP;
	    if (rep_INTP (tmp))
	    {
		long x = rep_INT (tmp) + 1;
		if (x <= rep_LISP_MAX_INT)
		{
		    TOP = rep_MAKE_INT (x);
		    goto fetch;
		}
	    }
	    TOP = Fplus1 (tmp);
	    break;

	case OP_DEC:
	    tmp = TOP;
	    if (rep_INTP (tmp))
	    {
		long x = rep_INT (tmp) - 1;
		if (x >= rep_LISP_MIN_INT)
		{
		    TOP = rep_MAKE_INT (x);
		    goto fetch;
		}
	    }
	    TOP = Fsub1 (tmp);
	    break;

	case OP_ASH:
	    CALL_2(Fash);

	case OP_ZEROP:
	    tmp = TOP;
	    if (rep_INTP (tmp))
	    {
		TOP = (tmp == rep_MAKE_INT (0)) ? Qt : Qnil;
		goto fetch;
	    }
	    TOP = Fzerop (tmp);
	    break;

	case OP_ATOM:
	    if(!rep_CONSP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_CONSP:
	    if(rep_CONSP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_LISTP:
	    if(rep_CONSP(TOP) || rep_NILP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_NUMBERP:
	    if(rep_NUMERICP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_STRINGP:
	    if(rep_STRINGP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_VECTORP:
	    if(rep_VECTORP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_CATCH:
	    /* This takes two arguments, TAG and THROW-repv.
	       THROW-repv is the saved copy of rep_throw_value,
	       if (car THROW-repv) == TAG we match, and we
	       leave two values on the stack, nil on top (to
	       pacify EJMP), (cdr THROW-repv) below that. */
	    tmp = RET_POP;		/* tag */
	    tmp2 = TOP;		/* rep_throw_value */
	    if(rep_CONSP(tmp2) && rep_CAR(tmp2) == tmp)
	    {
		TOP = rep_CDR(tmp2);	/* leave result at stk[1] */
		PUSH(Qnil);		/* cancel error */
	    }
	    break;

	case OP_THROW:
	    tmp = RET_POP;
	    if(!rep_throw_value)
		rep_throw_value = Fcons(TOP, tmp);
	    break;

	case OP_BINDERR:
	    /* Pop our single argument and cons it onto the bind-
	       stack in a pair with the current stack-pointer.
	       This installs an address in the code string as an
	       error handler. */
	    tmp = RET_POP;
	    BIND_PUSH (Fcons (Qerror, Fcons (tmp, rep_MAKE_INT(STK_USE))));
	    impurity++;
	    break;

	case OP_RETURN:
	    SYNC_GC;
	    unbind_n (bindbase, BIND_USE);
	    goto quit;

	case OP_UNBINDALL:
	    SYNC_GC;
	    unbind_n (bindbase + 1, BIND_USE - 1);
	    bindp = bindbase;
	    impurity = rep_SPEC_BINDINGS (BIND_TOP);
	    break;

	case OP_BOUNDP:
	    CALL_1(Fboundp);

	case OP_SYMBOLP:
	    if(rep_SYMBOLP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_GET:
	    CALL_2(Fget);

	case OP_PUT:
	    CALL_3(Fput);

	case OP_ERRORPRO:
	    /* This should be called with three values on the stack.
		1. conditions of the error handler
		2. rep_throw_value of the exception
		3. symbol to bind the error data to (or nil)

	       This function pops (1) and tests it against the error
	       in (2). If they match it sets (2) to nil, and binds the
	       error data to the symbol in (3). */
	    tmp = RET_POP;
	    if(rep_CONSP(TOP) && rep_CAR(TOP) == Qerror
	       && rep_compare_error(rep_CDR(TOP), tmp))
	    {
		repv tobind;
		/* The handler matches the error. */
		tmp = rep_CDR(TOP);	/* the error data */
		tmp2 = stackp[-1];	/* the symbol to bind to */
		if(rep_SYMBOLP(tmp2) && !rep_NILP(tmp2))
		{
		    tobind = rep_bind_symbol(Qnil, tmp2, tmp);
		    if (rep_SYM(tmp2)->car & rep_SF_SPECIAL)
			impurity++;
		}
		else
		    /* Placeholder to allow simple unbinding */
		    tobind = Qnil;
		BIND_PUSH (tobind);
		TOP = Qnil;
	    }
	    break;

	case OP_SIGNAL:
	    SYNC_GC;
	    CALL_2(Fsignal);

	case OP_QUOTIENT:
	    CALL_2(Fquotient);

	case OP_REVERSE:
	    CALL_1(Freverse);

	case OP_NREVERSE:
	    CALL_1(Fnreverse);

	case OP_ASSOC:
	    CALL_2(Fassoc);

	case OP_ASSQ:
	    CALL_2(Fassq);

	case OP_RASSOC:
	    CALL_2(Frassoc);

	case OP_RASSQ:
	    CALL_2(Frassq);

	case OP_LAST:
	    CALL_1(Flast);

	case OP_MAPCAR:
	    SYNC_GC;
	    CALL_2(Fmapcar);

	case OP_MAPC:
	    SYNC_GC;
	    CALL_2(Fmapc);

	case OP_MEMBER:
	    CALL_2(Fmember);

	case OP_MEMQ:
	    CALL_2(Fmemq);

	case OP_DELETE:
	    CALL_2(Fdelete);

	case OP_DELQ:
	    CALL_2(Fdelq);

	case OP_DELETE_IF:
	    SYNC_GC;
	    CALL_2(Fdelete_if);

	case OP_DELETE_IF_NOT:
	    SYNC_GC;
	    CALL_2(Fdelete_if_not);

	case OP_COPY_SEQUENCE:
	    CALL_1(Fcopy_sequence);

	case OP_SEQUENCEP:
	    CALL_1(Fsequencep);

	case OP_FUNCTIONP:
	    CALL_1(Ffunctionp);

	case OP_SPECIAL_FORM_P:
	    CALL_1(Fspecial_form_p);

	case OP_SUBRP:
	    CALL_1(Fsubrp);

	case OP_EQL:
	    CALL_2(Feql);

	case OP_MAX:
	    tmp = RET_POP;
	    if(rep_value_cmp(tmp, TOP) > 0)
		TOP = tmp;
	    break;

	case OP_MIN:
	    tmp = RET_POP;
	    if(rep_value_cmp(tmp, TOP) < 0)
		TOP = tmp;
	    break;

	case OP_FILTER:
	    SYNC_GC;
	    CALL_2(Ffilter);

	case OP_MACROP:
	    CALL_1(Fmacrop);

	case OP_BYTECODEP:
	    CALL_1(Fbytecodep);

	case OP_PUSHI0:
	    PUSH(rep_MAKE_INT(0));
	    goto fetch;

	case OP_PUSHI1:
	    PUSH(rep_MAKE_INT(1));
	    goto fetch;

	case OP_PUSHI2:
	    PUSH(rep_MAKE_INT(2));
	    goto fetch;

	case OP_PUSHIM1:
	    PUSH(rep_MAKE_INT(-1));
	    goto fetch;

	case OP_PUSHIM2:
	    PUSH(rep_MAKE_INT(-2));
	    goto fetch;

	case OP_PUSHI:
	    arg = FETCH;
	    if (arg < 128)
		PUSH(rep_MAKE_INT(arg));
	    else
		PUSH(rep_MAKE_INT(arg - 256));
	    goto fetch;

	case OP_PUSHIWN:
	    FETCH2(arg);
	    PUSH(rep_MAKE_INT(-arg));
	    goto fetch;

	case OP_PUSHIWP:
	    FETCH2(arg);
	    PUSH(rep_MAKE_INT(arg));
	    goto fetch;

	case OP_CAAR:
	    tmp = TOP;
	    if (rep_CONSP(tmp) && rep_CONSP(rep_CAR(tmp)))
		TOP = rep_CAAR(tmp);
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_CADR:
	    tmp = TOP;
	    if (rep_CONSP(tmp) && rep_CONSP(rep_CDR(tmp)))
		TOP = rep_CADR(tmp);
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_CDAR:
	    tmp = TOP;
	    if (rep_CONSP(tmp) && rep_CONSP(rep_CAR(tmp)))
		TOP = rep_CDAR(tmp);
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_CDDR:
	    tmp = TOP;
	    if (rep_CONSP(tmp) && rep_CONSP(rep_CDR(tmp)))
		TOP = rep_CDDR(tmp);
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_CADDR:
	    TOP = list_ref (TOP, 2);
	    goto fetch;

	case OP_CADDDR:
	    TOP = list_ref (TOP, 3);
	    goto fetch;

	case OP_CADDDDR:
	    TOP = list_ref (TOP, 4);
	    goto fetch;

	case OP_CADDDDDR:
	    TOP = list_ref (TOP, 5);
	    goto fetch;

	case OP_CADDDDDDR:
	    TOP = list_ref (TOP, 6);
	    goto fetch;

	case OP_CADDDDDDDR:
	    TOP = list_ref (TOP, 7);
	    goto fetch;

	case OP_FLOOR:
	    CALL_1(Ffloor);

	case OP_CEILING:
	    CALL_1(Fceiling);

	case OP_TRUNCATE:
	    CALL_1(Ftruncate);

	case OP_ROUND:
	    CALL_1(Fround);

	case OP_BINDOBJ:
	    tmp = RET_POP;
	    BIND_PUSH (rep_bind_object(tmp));
	    impurity++;
	    break;

	case OP_FORBID:
	    rep_FORBID;
	    PUSH (rep_PREEMPTABLE_P ? Qnil : Qt);
	    goto fetch;

	case OP_PERMIT:
	    rep_PERMIT;
	    PUSH (rep_PREEMPTABLE_P ? Qnil : Qt);
	    goto fetch;

	case OP_EXP:
	    CALL_1(Fexp);

	case OP_LOG:
	    CALL_1(Flog);

	case OP_COS:
	    CALL_1(Fcos);

	case OP_SIN:
	    CALL_1(Fsin);

	case OP_TAN:
	    CALL_1(Ftan);

	case OP_SQRT:
	    CALL_1(Fsqrt);

	case OP_EXPT:
	    CALL_2(Fexpt);

	case OP_SWAP2:
	    tmp = TOP;
	    TOP = stackp[-1];
	    stackp[-1] = stackp[-2];
	    stackp[-2] = tmp;
	    goto fetch;

	case OP_MOD:
	    CALL_2(Fmod);

	case OP_MAKE_CLOSURE:
	    CALL_2(Fmake_closure);

	case OP_UNBINDALL_0:
	    SYNC_GC;
	    unbind_n (bindbase, BIND_USE);
	    bindp = bindbase - 1;
	    impurity = 0;
	    break;

	case OP_CLOSUREP:
	    if(rep_FUNARGP(TOP))
		TOP = Qt;
	    else
		TOP = Qnil;
	    goto fetch;

	case OP_POP_ALL:
	    stackp = stackbase - 1;
	    goto fetch;

	/* Jump instructions follow */

	case OP_EJMP:
	    /* Pop the stack; if it's nil jmp pc[0,1], otherwise
	       set rep_throw_value=ARG and goto the error handler. */
	    tmp = RET_POP;
	    if(rep_NILP(tmp))
		goto do_jmp;
	    rep_throw_value = tmp;
	    goto error;

	case OP_JN:
	    if(rep_NILP(RET_POP))
		goto do_jmp;
	    pc += 2;
	    goto fetch;

	case OP_JT:
	    if(!rep_NILP(RET_POP))
		goto do_jmp;
	    pc += 2;
	    goto fetch;

	case OP_JPN:
	    if(rep_NILP(TOP))
	    {
		POP;
		goto do_jmp;
	    }
	    pc += 2;
	    goto fetch;

	case OP_JPT:
	    if(!rep_NILP(TOP))
	    {
		POP;
		goto do_jmp;
	    }
	    pc += 2;
	    goto fetch;

	case OP_JNP:
	    if(rep_NILP(TOP))
		goto do_jmp;
	    POP;
	    pc += 2;
	    goto fetch;

	case OP_JTP:
	    if(rep_NILP(TOP))
	    {
		POP;
		pc += 2;
		goto fetch;
	    }
	    /* FALL THROUGH */

	case OP_JMP:
	do_jmp:
	    pc = rep_STR(code) + ((pc[0] << ARG_SHIFT) | pc[1]);

	    /* Test if an interrupt occurred... */
	    rep_TEST_INT;
	    if(rep_INTERRUPTP)
		goto error;

	    /* ...or if it's time to gc... */
	    SYNC_GC;
	    if(rep_data_after_gc >= rep_gc_threshold)
		Fgarbage_collect(Qt);

	    /* ...or time to switch threads */
	    rep_MAY_YIELD;
	    break;

	default:
	    Fsignal(Qerror, rep_list_2(rep_VAL(&unknown_op),
				       rep_MAKE_INT(insn)));
	    goto error;
	}

	/* Check if the instruction raised an exception.

	   Checking for !TOP isn't strictly necessary, but I think
	   there may still be some broken functions that return
	   rep_NULL without setting rep_throw_value.. */
    end:
	if (rep_throw_value || !TOP)
	{
	    /* Some form of error occurred. Unwind the binding stack. */
	error:
	    while(!BIND_TOP_P)
	    {
		repv item = BIND_RET_POP;
		if(!rep_CONSP(item) || rep_CAR(item) != Qerror)
		{
		    rep_GC_root gc_throwval;
		    repv throwval = rep_throw_value;
		    rep_throw_value = rep_NULL;
		    rep_PUSHGC(gc_throwval, throwval);
		    SYNC_GC;
		    impurity -= rep_unbind_object(item);
		    rep_POPGC;
		    rep_throw_value = throwval;
		}
		else if(rep_throw_value != rep_NULL)
		{
		    item = rep_CDR(item);

		    /* item is an exception-handler, (PC . SP)

		       When the code at PC is called, it will have
		       the current stack usage set to SP, and then
		       the value of rep_throw_value pushed on top.

		       The handler can then use the EJMP instruction
		       to pass control back to the error: label, or
		       simply continue execution as normal. */

		    stackp = (stackbase - 1) + rep_INT(rep_CDR(item));
		    PUSH(rep_throw_value);
		    rep_throw_value = rep_NULL;
		    pc = rep_STR(code) + rep_INT(rep_CAR(item));
		    impurity--;
		    goto fetch;
		}
		else
		{
		    /* car is an exception handler, but rep_throw_value isn't
		       set, so there's nothing to handle. Keep unwinding. */
		    impurity--;
		}
	    }
	    TOP = rep_NULL;
	    goto quit;
	}
    }

quit:
    /* only use this var to save declaring another */
    code = TOP;

    /* close the stack scope */ }

    rep_lisp_depth--;
    rep_POPGCN; rep_POPGCN; rep_POPGC; rep_POPGC;
    return code;
}

DEFUN("validate-byte-code", Fvalidate_byte_code, Svalidate_byte_code, (repv bc_major, repv bc_minor), rep_Subr2) /*
::doc:validate-byte-code::
validate-byte-code BC-MAJOR BC-MINOR

Check that byte codes from instruction set BC-MAJOR.BC-MINOR, may be
executed. If not, an error will be signalled.
::end:: */
{
    if(!rep_INTP(bc_major) || !rep_INTP(bc_minor)
       || rep_INT(bc_major) != BYTECODE_MAJOR_VERSION
       || rep_INT(bc_minor) > BYTECODE_MINOR_VERSION)
	return Fsignal(Qbytecode_error, Qnil);
    else
	return Qt;
}

DEFUN("make-byte-code-subr", Fmake_byte_code_subr, Smake_byte_code_subr, (repv args), rep_SubrN) /*
::doc:make-byte-code-subr::
make-byte-code-subr ARGS CODE CONSTANTS STACK [DOC] [INTERACTIVE]

Return an object that can be used as the function value of a symbol.
::end:: */
{
    int len = rep_list_length(args);
    repv obj[6], vec;
    int used;

    if(len < rep_COMPILED_MIN_SLOTS)
	return rep_signal_missing_arg(len + 1);
    
    if(!rep_CONSP(rep_CAR(args)) && !rep_SYMBOLP(rep_CAR(args)))
	return rep_signal_arg_error(rep_CAR(args), 1);
    obj[0] = rep_CAR(args); args = rep_CDR(args);
    if(!rep_STRINGP(rep_CAR(args)))
	return rep_signal_arg_error(rep_CAR(args), 2);
    obj[1] = rep_CAR(args); args = rep_CDR(args);
    if(!rep_VECTORP(rep_CAR(args)))
	return rep_signal_arg_error(rep_CAR(args), 3);
    obj[2] = rep_CAR(args); args = rep_CDR(args);
    if(!rep_INTP(rep_CAR(args)))
	return rep_signal_arg_error(rep_CAR(args), 4);
    obj[3] = rep_CAR(args); args = rep_CDR(args);
    used = 4;

    if(rep_CONSP(args))
    {
	obj[used++] = rep_CAR(args); args = rep_CDR(args);
	if(rep_CONSP(args))
	{
	    obj[used++] = rep_CAR(args); args = rep_CDR(args);
	    if(rep_NILP(obj[used - 1]))
		used--;
	}
	if(used == 5 && rep_NILP(obj[used - 1]))
	    used--;
    }

    vec = Fmake_vector(rep_MAKE_INT(used), Qnil);
    if(vec != rep_NULL)
    {
	int i;
	rep_COMPILED(vec)->car = ((rep_COMPILED(vec)->car
				   & ~rep_CELL8_TYPE_MASK) | rep_Compiled);
	for(i = 0; i < used; i++)
	    rep_VECTI(vec, i) = obj[i];
    }
    return vec;
}

void
rep_lispmach_init(void)
{
    rep_ADD_SUBR(Sjade_byte_code);
    rep_INTERN(jade_byte_code);
    rep_ADD_SUBR(Svalidate_byte_code);
    rep_ADD_SUBR(Smake_byte_code_subr);
    rep_INTERN(bytecode_error); rep_ERROR(bytecode_error);
}

void
rep_lispmach_kill(void)
{
#ifdef BYTE_CODE_HISTOGRAM
    int i;
    fprintf(stderr, "\nByte code usages:\n");
    for(i = 0; i < 256; i++)
	fprintf(stderr, "\t%3d %ld\n", i, byte_code_usage[i]);
#endif
}
