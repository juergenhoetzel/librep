/* numbers.c -- Implement the tower of numeric types
   Copyright (C) 1993, 1994, 2000 John Harper <john@dcs.warwick.ac.uk>
   $Id$

   This file is part of librep.

   librep is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   librep is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with librep; see the file COPYING.	If not, write to
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
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <ctype.h>
#include <gmp.h>

#ifdef NEED_MEMORY_H
# include <memory.h>
#endif

DEFSTRING(div_zero, "Divide by zero");
DEFSTRING(domain_error, "Domain error");


/* Private type definitions */

typedef struct {
    repv car;
    mpz_t z;
} rep_number_z;

typedef struct {
    repv car;
    mpq_t q;
} rep_number_q;

typedef struct {
    repv car;
    double f;
} rep_number_f;

typedef struct rep_number_block_struct {
    union {
	struct rep_number_block_struct *p;
	/* ensure that the following is aligned correctly */
	mpz_t dummy_z;
	mpq_t dummy_q;
	double dummy_f;
    } next;
    rep_number data[1];
} rep_number_block;

#define rep_SIZEOF_NUMBER_BLOCK(n,t) \
    (sizeof (rep_number_block) - sizeof (rep_number) + (t) * (n))

#define rep_NUMBER(v,t) (((rep_number_ ## t *) rep_PTR(v))->t)


/* number object handling */

static rep_number_block *number_block_chain[3];
static rep_number *number_freelist[3];
static int number_allocations[3], number_sizeofs[3];
static int allocated_numbers, used_numbers;

static inline int
type_to_index (int type)
{
    return (type == rep_NUMBER_BIGNUM ? 0
	    : type == rep_NUMBER_RATIONAL ? 1 : 2);
}

static void *
make_number (int type)
{
    rep_number *cn;
    int idx = type_to_index (type);
    cn = number_freelist[idx];
    if(cn == NULL)
    {
	int i;
	rep_number_block *cb;
	rep_number *ptr, *next;
	cb = rep_alloc (rep_SIZEOF_NUMBER_BLOCK (number_allocations[idx], 
						 number_sizeofs[idx]));
	allocated_numbers += number_allocations[idx];
	cb->next.p = number_block_chain[idx];
	number_block_chain[idx] = cb;
	ptr = cb->data;
	for(i = 0; i < (number_allocations[idx] - 1); i++, ptr = next)
	{
	    next = (rep_number *) (((char *) ptr) + number_sizeofs[idx]);
	    ptr->car = (repv) next;
	}
	ptr->car = 0;
	number_freelist[idx] = (rep_number *) cb->data;
	cn = number_freelist[idx];
    }
    number_freelist[idx] = (rep_number *) cn->car;
    cn->car = rep_Number | type;
    used_numbers++;
    rep_data_after_gc += sizeof (rep_number);
    return cn;
}

static void
number_sweep(void)
{
    int idx;
    used_numbers = 0;
    for (idx = 0; idx < 3; idx++)
    {
	rep_number_block *cb = number_block_chain[idx];
	number_block_chain[idx] = 0;
	number_freelist[idx] = 0;
	while (cb != 0)
	{
	    rep_number_block *nxt = cb->next.p;
	    rep_number *newfree = 0, *newfreetail = 0, *this;
	    int i, newused = 0;
	    for (i = 0, this = cb->data;
		 i < number_allocations[idx];
		 i++, this = (rep_number *) (((char *) this)
					     + number_sizeofs[idx]))
	    {
		/* if on the freelist then the CELL_IS_8 bit
		   will be unset (since the pointer is long aligned) */
		if (rep_CELL_CONS_P(rep_VAL(this))
		    || !rep_GC_CELL_MARKEDP ((repv) this))
		{
		    if (!newfreetail)
			newfreetail = this;
		    if (!rep_CELL_CONS_P(rep_VAL(this)))
		    {
			switch (idx)
			{
			case 0:
			    mpz_clear (((rep_number_z *)this)->z);
			    break;

			case 1:
			    mpq_clear (((rep_number_q *)this)->q);
			    break;
			}
		    }
		    this->car = rep_VAL (newfree);
		    newfree = this;
		}
		else
		{
		    rep_GC_CLR_CELL ((repv) this);
		    newused++;
		}
	    }
	    if(newused == 0)
	    {
		/* Whole block unused, lets get rid of it.  */
		rep_free(cb);
		allocated_numbers -= number_allocations[idx];
	    }
	    else
	    {
		if(newfreetail != NULL)
		{
		    /* Link this mini-freelist onto the main one.  */
		    newfreetail->car = rep_VAL (number_freelist[idx]);
		    number_freelist[idx] = newfree;
		    used_numbers += newused;
		}
		/* Have to rebuild the block chain as well.  */
		cb->next.p = number_block_chain[idx];
		number_block_chain[idx] = cb;
	    }
	    cb = nxt;
	}
    }
}


/* Promotion */

static repv
dup (repv in)
{
    switch (rep_NUMERIC_TYPE (in))
    {
	rep_number_z *z;
	rep_number_q *q;
	rep_number_f *f;

    case rep_NUMBER_INT:
	return in;

    case rep_NUMBER_BIGNUM:
	z = make_number (rep_NUMBER_BIGNUM);
	mpz_init_set (z->z, rep_NUMBER(in,z));
	return rep_VAL (z);

    case rep_NUMBER_RATIONAL:
	q = make_number (rep_NUMBER_RATIONAL);
	mpq_init (q->q);
	mpq_set (q->q, rep_NUMBER(in,q));
        return rep_VAL (q);

    case rep_NUMBER_FLOAT:
	f = make_number (rep_NUMBER_FLOAT);
	f->f = rep_NUMBER(in,f);
	return rep_VAL (f);
    }
    abort ();
}

static repv
promote_to (repv in, int type)
{
    int in_type = rep_NUMERIC_TYPE (in);

    if (in_type >= type)
	return in;

    switch (in_type)
    {
	rep_number_z *z;
	rep_number_q *q;
	rep_number_f *f;

    case rep_NUMBER_INT:
	switch (type)
	{

	case rep_NUMBER_BIGNUM:
	    z = make_number (rep_NUMBER_BIGNUM);
	    mpz_init_set_si (z->z, rep_INT(in));
	    return rep_VAL (z);

	case rep_NUMBER_RATIONAL:
	    q = make_number (rep_NUMBER_RATIONAL);
	    mpq_init (q->q);
	    mpq_set_si (q->q, rep_INT(in), 1);
	    mpq_canonicalize (q->q);
	    return rep_VAL (q);

	case rep_NUMBER_FLOAT:
	    f = make_number (rep_NUMBER_FLOAT);
	    f->f = (double) rep_INT(in);
	    return rep_VAL (f);
	    break;

	default:
	    abort();
	}

    case rep_NUMBER_BIGNUM:
	switch (type)
	{
	case rep_NUMBER_RATIONAL:
	    q = make_number (rep_NUMBER_RATIONAL);
	    mpq_init (q->q);
	    mpq_set_z (q->q, rep_NUMBER(in,z));
	    return rep_VAL (q);

	case rep_NUMBER_FLOAT:
	    f = make_number (rep_NUMBER_FLOAT);
	    f->f = mpz_get_d (rep_NUMBER(in,z));
	    return rep_VAL (f);

	default:
	    abort();
	}

    case rep_NUMBER_RATIONAL:
	assert (type == rep_NUMBER_FLOAT);
	f = make_number (rep_NUMBER_FLOAT);
	f->f = mpq_get_d (rep_NUMBER(in,q));
	return rep_VAL (f);

    default:
	abort ();
    }
}

static repv
maybe_demote (repv in)
{
    assert (rep_NUMBERP(in));
    switch (rep_NUMERIC_TYPE(in))
    {
    case rep_NUMBER_RATIONAL:
	if (mpz_cmp_ui (mpq_denref (rep_NUMBER (in,q)), 1) == 0)
	{
	    rep_number_z *z = make_number (rep_NUMBER_BIGNUM);
	    mpz_init_set (z->z, mpq_numref (rep_NUMBER (in,q)));
	    in = rep_VAL (in);
	}
	else
	    break;
	/* fall through */

    case rep_NUMBER_BIGNUM:
	if (mpz_cmp_si (rep_NUMBER (in,z), rep_LISP_MAX_INT) <= 0
	    && mpz_cmp_si (rep_NUMBER (in,z), rep_LISP_MIN_INT) >= 0)
	{
	    in = rep_MAKE_INT (mpz_get_si (rep_NUMBER (in,z)));
	}
    }
    return in;
}

static repv
coerce (repv in, int type)
{
    int in_type = rep_NUMERIC_TYPE (in);
    if (in_type <= type)
	return in;
    switch (in_type)
    {
    case rep_NUMBER_BIGNUM:
	switch (type)
	{
	case rep_NUMBER_INT:
	    return rep_MAKE_INT (mpz_get_si (rep_NUMBER (in,z)));

	default:
	    abort ();
	}
	break;

	/* XXX implement me.. */
    case rep_NUMBER_RATIONAL:
    case rep_NUMBER_FLOAT:
    default:
	abort ();
    }
}

static void
promote (repv *n1p, repv *n2p)
{
    repv n1 = *n1p;
    repv n2 = *n2p;
    int n1_type = rep_NUMERIC_TYPE (n1);
    int n2_type = rep_NUMERIC_TYPE (n2);

    if (n1_type > n2_type)
	*n2p = promote_to (n2, n1_type);
    else if (n1_type < n2_type)
	*n1p = promote_to (n1, n2_type);
}

static repv
promote_dup (repv *n1p, repv *n2p)
{
    repv n1 = *n1p;
    repv n2 = *n2p;
    int n1_type = rep_NUMERIC_TYPE (n1);
    int n2_type = rep_NUMERIC_TYPE (n2);
    repv out = rep_NULL;

    if (n1_type > n2_type)
    {
	out = promote_to (n2, n1_type);
	*n2p = out;
    }
    else if (n1_type < n2_type)
    {
	out = promote_to (n1, n2_type);
	*n1p = out;
    }
    else
	out = dup (*n1p);

    return out;
}

repv
rep_make_long_uint (u_long in)
{
    if (in < rep_LISP_MAX_INT)
	return rep_MAKE_INT (in);
    else
    {
	rep_number_z *z = make_number (rep_NUMBER_BIGNUM);
	mpz_init_set_ui (z->z, in);
	return rep_VAL (z);
    }
}

inline repv
rep_make_long_int (long in)
{
    if (in >= rep_LISP_MIN_INT && in <= rep_LISP_MAX_INT)
	return rep_MAKE_INT (in);
    else
    {
	rep_number_z *z = make_number (rep_NUMBER_BIGNUM);
	mpz_init_set_si (z->z, in);
	return rep_VAL (z);
    }
}

u_long
rep_get_long_uint (repv in)
{
    if (rep_INTP (in))
	return rep_INT (in);
    else if (rep_NUMBERP (in))
    {
	switch (rep_NUMBER_TYPE(in))
	{
	case rep_NUMBER_BIGNUM:
	    return mpz_get_ui (rep_NUMBER(in,z));

	case rep_NUMBER_RATIONAL:
	    return (u_long) mpq_get_d (rep_NUMBER(in,q));

	case rep_NUMBER_FLOAT:
	    return (u_long) rep_NUMBER(in,f);
	}
    }
    else if (rep_CONSP (in)
	     && rep_INTP (rep_CAR (in)) && rep_INTP (rep_CDR (in)))
    {
	return rep_INT (rep_CAR (in)) | (rep_INT (rep_CDR (in)) << 24);
    }
    return 0;
}

long
rep_get_long_int (repv in)
{
    if (rep_INTP (in))
	return rep_INT (in);
    else if (rep_NUMBERP (in))
    {
	switch (rep_NUMBER_TYPE(in))
	{
	case rep_NUMBER_BIGNUM:
	    return mpz_get_ui (rep_NUMBER(in,z));

	case rep_NUMBER_RATIONAL:
	    return (u_long) mpq_get_d (rep_NUMBER(in,q));

	case rep_NUMBER_FLOAT:
	    return (u_long) rep_NUMBER(in,f);
	}
    }
    else if (rep_CONSP (in)
	     && rep_INTP (rep_CAR (in)) && rep_INTP (rep_CDR (in)))
    {
	return rep_INT (rep_CAR (in)) | (rep_INT (rep_CDR (in)) << 24);
    }
    return 0;
}

repv
rep_make_longlong_int (rep_long_long in)
{
    if (in <= rep_LISP_MAX_INT && in >= rep_LISP_MIN_INT)
	return rep_MAKE_INT (in);
    else
    {
	int sign = (in < 0) ? -1 : 1;
	unsigned rep_long_long uin = (sign < 0) ? -in : in;
	u_long bottom = (u_long) uin;
	u_long top = (u_long) (uin >> (CHAR_BIT * sizeof (long)));
	rep_number_z *z = make_number (rep_NUMBER_BIGNUM);
	mpz_init_set_ui (z->z, bottom);
	if (top != 0)
	{
	    mpz_t tem;
	    mpz_init_set_ui (tem, top);
	    mpz_mul_2exp (tem, tem, CHAR_BIT * sizeof (long));
	    mpz_add (z->z, z->z, tem);
	    mpz_clear (tem);
	}
	if (sign < 0)
	    mpz_neg (z->z, z->z);
	return rep_VAL (z);
    }
}

rep_long_long
rep_get_longlong_int (repv in)
{
    if (rep_INTP (in))
	return rep_INT (in);
    else if (rep_NUMBERP (in))
    {
	switch (rep_NUMBER_TYPE(in))
	{
	case rep_NUMBER_BIGNUM:
	    {
		int sign = mpz_sgn (rep_NUMBER(in,z));
		rep_long_long bottom, top, out;
		mpz_t tem;
		mpz_init_set (tem, rep_NUMBER(in,z));
		if (sign < 0)
		    mpz_neg (tem, tem);
		bottom = mpz_get_ui (tem);
		mpz_tdiv_q_2exp (tem, tem, CHAR_BIT * sizeof (long));
		top = mpz_get_ui (tem);
		out = bottom | (top << (CHAR_BIT * sizeof (long)));
		if (sign < 0)
		    out = -out;
		return out;
	    }

	case rep_NUMBER_RATIONAL:
	    return (rep_long_long) mpq_get_d (rep_NUMBER(in,q));

	case rep_NUMBER_FLOAT:
	    return (rep_long_long) rep_NUMBER(in,f);
	}
    }
    else if (rep_CONSP (in)
	     && rep_INTP (rep_CAR (in)) && rep_INTP (rep_CDR (in)))
    {
	rep_long_long out = rep_INT (rep_CDR (in));
	out = (out << 24) | rep_INT (rep_CAR (in));
	return out;
    }
    return 0;
}

repv
rep_make_float (double in, rep_bool force)
{
    if (!force && floor (in) == in && in < LONG_MAX && in > LONG_MIN)
	return rep_make_long_int ((long) in);
    else
    {
	rep_number_f *f = make_number (rep_NUMBER_FLOAT);
	f->f = in;
	return rep_VAL (f);
    }
}

double
rep_get_float (repv in)
{
    if (rep_NUMERICP (in))
    {
	switch (rep_NUMERIC_TYPE (in))
	{
	case rep_NUMBER_INT:
	    return rep_INT (in);

	case rep_NUMBER_BIGNUM:
	    return mpz_get_d (rep_NUMBER(in,z));

	case rep_NUMBER_RATIONAL:
	    return mpq_get_d (rep_NUMBER(in,q));

	case rep_NUMBER_FLOAT:
	    return rep_NUMBER(in,f);
	}
    }
    return 0.0;
}

static int
number_cmp(repv v1, repv v2)
{
    if(!rep_NUMERICP(v1) || !rep_NUMERICP(v2))
	return 1;
    promote (&v1, &v2);
    switch (rep_NUMERIC_TYPE (v1))
    {
    case rep_NUMBER_INT:
	return rep_INT(v1) - rep_INT(v2);

    case rep_NUMBER_BIGNUM:
	return mpz_cmp (rep_NUMBER(v1,z), rep_NUMBER(v2,z));

    case rep_NUMBER_RATIONAL:
	return mpq_cmp (rep_NUMBER(v1,q), rep_NUMBER(v2,q));

    case rep_NUMBER_FLOAT:
	return rep_NUMBER(v1,f) - rep_NUMBER(v2,f);
    }
    return 1;
}

repv
rep_parse_number (char *buf, int len, int radix, int sign, int type)
{
    switch (type)
    {
	rep_number_z *z;
	rep_number_q *q;
	rep_number_f *f;
	char *tem;
	double d;
	int bits;

    case 0:
	switch (radix)
	{
	case 2:
	    bits = len;
	    break;

	case 8:
	    bits = len * 3;
	    break;

	case 10:
	    /* log_2 10 = 3.3219.. */
	    bits = (len * 33) / 10;
	    break;

	case 16:
	    bits = len * 4;
	    break;

	default:
	    abort();
	}

	if (bits < rep_LISP_INT_BITS)
	{
	    static const signed char map[] = {
		 0,  1,  2,  3,  4,  5,  6,  7,		/* 0x30 -> 0x37 */
		 8,  9, -1, -1, -1, -1, -1, -1,
		-1, 10, 11, 12, 13, 14, 15, 16,		/* 0x40 -> 0x48 */
		17, 18, 19, 20, 21, 22, 23, 24,
		25, 26, 27, 28, 29, 30, 31, 32,		/* 0x50 -> 0x58 */
		33, 34, 35, 36
	    };
	    long value = 0;
	    char c;
	    while ((c = *buf++) != 0)
		value = value * radix + map[toupper(c) - '0'];
	    return rep_MAKE_INT (value * sign);
	}
	else
	{
	    z = make_number (rep_NUMBER_BIGNUM);
	    if (mpz_init_set_str (z->z, buf, radix) == 0)
	    {
		if (sign < 0)
		    mpz_neg (z->z, z->z);
		return maybe_demote (rep_VAL (z));
	    }
	    else
		goto error;
	}

    case rep_NUMBER_RATIONAL:
	tem = strchr (buf, '/');
	assert (tem != 0);
	q = make_number (rep_NUMBER_RATIONAL);
	mpq_init (q->q);
	*tem++ = 0;
	if (mpz_set_str (mpq_numref (q->q), buf, radix) == 0
	    && mpz_set_str (mpq_denref (q->q), tem, radix) == 0)
	{
	    mpq_canonicalize (q->q);
	    if (sign < 0)
		mpq_neg (q->q, q->q);
	    return maybe_demote (rep_VAL (q));
	}
	else
	    goto error;

    case rep_NUMBER_FLOAT:
	d = strtod (buf, &tem);
	if (tem - buf != len)
	    goto error;
	f = make_number (rep_NUMBER_FLOAT);
	f->f = d * sign;
	return rep_VAL (f);
    }
error:
    return rep_NULL;
}

char *
rep_print_number_to_string (repv obj, int radix, int prec)
{
    char *out = 0;
    switch (rep_NUMERIC_TYPE (obj))
    {
	u_char buf[64], fmt[8], *tem;
	size_t len;

    case rep_NUMBER_INT:
	if (radix == 10)
	    tem = "%" rep_PTR_SIZED_INT_CONV "d";
	else if (radix == 16)
	    tem = "%" rep_PTR_SIZED_INT_CONV "x";
	else if (radix == 8)
	    tem = "%" rep_PTR_SIZED_INT_CONV "o";
	else
	    tem = 0;			/* XXX handle arbitrary bases */
	if (tem != 0)
	{
#ifdef HAVE_SNPRINTF
	    snprintf(buf, sizeof(buf), tem, rep_INT(obj));
#else
	    sprintf(buf, tem, rep_INT(obj));
#endif
	    out = strdup (buf);
	}
	break;

    case rep_NUMBER_BIGNUM:
	out = mpz_get_str (0, radix, rep_NUMBER(obj,z));
	break;

    case rep_NUMBER_RATIONAL:
	len = (mpz_sizeinbase (mpq_numref (rep_NUMBER (obj, q)), radix)
	       + mpz_sizeinbase (mpq_denref (rep_NUMBER (obj, q)), radix) + 4);
	out = malloc (len);
	mpz_get_str (out, radix, mpq_numref (rep_NUMBER (obj,q)));
	len = strlen (out);
	out[len++] = '/';
	mpz_get_str (out + len, radix, mpq_denref (rep_NUMBER (obj,q)));
	break;

    case rep_NUMBER_FLOAT:		/* XXX handle radix arg */
	sprintf (fmt, "%%.%dg", prec < 0 ? 16 : prec);
#ifdef HAVE_SNPRINTF
	snprintf(buf, sizeof(buf), fmt, rep_NUMBER(obj,f));
#else
	sprintf(buf, fmt, rep_NUMBER(obj,f));
#endif
	/* libc doesn't always add a point */
	if (!strchr (buf, '.') && !strchr (buf, 'e') && !strchr (buf, 'E'))
	    strcat (buf, ".");
	out = strdup (buf);
    }
    return out;
}

static void
number_prin (repv stream, repv obj)
{
    if (rep_INTP (obj))
    {
	char buf[64];
#ifdef HAVE_SNPRINTF
	snprintf(buf, sizeof(buf),
		 "%" rep_PTR_SIZED_INT_CONV "d", rep_INT(obj));
#else
	sprintf(buf, "%" rep_PTR_SIZED_INT_CONV "d", rep_INT(obj));
#endif
	rep_stream_puts(stream, buf, -1, rep_FALSE);
    }
    else
    {
	char *string = rep_print_number_to_string (obj, 10, -1);
	if (string != 0)
	{
	    rep_stream_puts (stream, string, -1, rep_FALSE);
	    free (string);
	}
	else
	    rep_stream_puts (stream, "#<unprintable number>", -1, rep_FALSE);
    }
}


/* lisp functions */

repv
rep_number_foldl (repv args, repv (*op)(repv, repv))
{
    if (rep_CONSP (args) && rep_NUMERICP (rep_CAR (args)))
    {
	repv sum = rep_CAR (args);
	int i = 2;
	args = rep_CDR (args);
	while (rep_CONSP (args))
	{
	    repv arg = rep_CAR (args);
	    if (!rep_NUMERICP (arg))
		return rep_signal_arg_error (arg, i);
	    sum = op (sum, arg);
	    args = rep_CDR (args);
	    i++;
	}
	return sum;
    }
    return (rep_CONSP(args) ? rep_signal_arg_error (rep_CAR (args), 1)
	    : rep_signal_missing_arg (1));
}

repv
rep_number_add (repv x, repv y)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    rep_DECLARE2 (y, rep_NUMERICP);
    out = promote_dup (&x, &y);
    switch (rep_NUMERIC_TYPE (out))
    {
	case rep_NUMBER_INT:
	    out = rep_make_long_int (rep_INT (x) + rep_INT (y));
	    break;

	case rep_NUMBER_BIGNUM:
	    mpz_add (rep_NUMBER (out,z), rep_NUMBER (x,z), rep_NUMBER (y,z));
	    out = maybe_demote (out);
	    break;

	case rep_NUMBER_RATIONAL:
	    mpq_add (rep_NUMBER (out,q), rep_NUMBER (x,q), rep_NUMBER (y,q));
	    out = maybe_demote (out);
	    break;
    
	case rep_NUMBER_FLOAT:
	    rep_NUMBER (out,f) = rep_NUMBER (x,f) + rep_NUMBER (y,f);
	    break;
    }
    return out;
}

repv
rep_number_neg (repv x)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    out = dup (x);
    switch (rep_NUMERIC_TYPE (out))
    {
    case rep_NUMBER_INT:
	out = rep_make_long_int (-rep_INT (x));
	break;

    case rep_NUMBER_BIGNUM:
	mpz_neg (rep_NUMBER(out,z), rep_NUMBER(x,z));
	break;

    case rep_NUMBER_RATIONAL:
	mpq_neg (rep_NUMBER(out,q), rep_NUMBER(x,q));
	break;

    case rep_NUMBER_FLOAT:
	rep_NUMBER(out,f) = -rep_NUMBER(x,f);
	break;
    }
    return out;
}

repv
rep_number_sub (repv x, repv y)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    rep_DECLARE2 (y, rep_NUMERICP);
    out = promote_dup (&x, &y);
    switch (rep_NUMERIC_TYPE (out))
    {
    case rep_NUMBER_INT:
	out = rep_make_long_int (rep_INT (x) - rep_INT (y));
	break;

    case rep_NUMBER_BIGNUM:
	mpz_sub (rep_NUMBER (out,z), rep_NUMBER (x,z), rep_NUMBER (y,z));
	out = maybe_demote (out);
	break;

    case rep_NUMBER_RATIONAL:
	mpq_sub (rep_NUMBER (out,q), rep_NUMBER (x,q), rep_NUMBER (y,q));
	out = maybe_demote (out);
	break;
    
    case rep_NUMBER_FLOAT:
	rep_NUMBER (out,f) = rep_NUMBER (x,f) - rep_NUMBER (y,f);
	break;
    }
    return out;
}

repv
rep_number_mul (repv x, repv y)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    rep_DECLARE2 (y, rep_NUMERICP);
    out = promote_dup (&x, &y);
    switch (rep_NUMERIC_TYPE (out))
    {
	rep_long_long int tot;

    case rep_NUMBER_INT:
	tot = ((rep_long_long) rep_INT (x)) * ((rep_long_long) rep_INT (y));
	out = rep_make_longlong_int (tot);
	break;

    case rep_NUMBER_BIGNUM:
	mpz_mul (rep_NUMBER (out,z), rep_NUMBER (x,z), rep_NUMBER (y,z));
	out = maybe_demote (out);
	break;

    case rep_NUMBER_RATIONAL:
	mpq_mul (rep_NUMBER (out,q), rep_NUMBER (x,q), rep_NUMBER (y,q));
	out = maybe_demote (out);
	break;
    
    case rep_NUMBER_FLOAT:
	rep_NUMBER (out,f) = rep_NUMBER (x,f) * rep_NUMBER (y,f);
	break;
    }
    return out;
}

repv
rep_number_div (repv x, repv y)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    rep_DECLARE2 (y, rep_NUMERICP);

    if (Fzerop (y) != Qnil)
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&div_zero)));

    out = promote_dup (&x, &y);
    switch (rep_NUMERIC_TYPE (out))
    {
    case rep_NUMBER_INT:
	if (rep_INT (x) % rep_INT (y) == 0)
	    out = rep_MAKE_INT (rep_INT (x) / rep_INT (y));
	else
	{
	    u_long uy = (rep_INT (y) < 0 ? - rep_INT (y) : rep_INT (y));
	    rep_number_q *q = make_number (rep_NUMBER_RATIONAL);
	    mpq_init (q->q);
	    mpq_set_si (q->q, rep_INT (x), uy);
	    mpq_canonicalize (q->q);
	    if (rep_INT (y) < 0)
		mpq_neg (q->q, q->q);
	    out = rep_VAL (q);
	}
	break;

    case rep_NUMBER_BIGNUM:
	{
	    mpz_t rem;
	    int sign;
	    mpz_init (rem);
	    mpz_tdiv_r (rem, rep_NUMBER (x,z), rep_NUMBER (y,z));
	    sign = mpz_sgn (rem);
	    mpz_clear (rem);
	    if (sign == 0)
	    {
		mpz_tdiv_q (rep_NUMBER (out,z),
			    rep_NUMBER (x,z), rep_NUMBER (y,z));
		out = maybe_demote (out);
	    }
	    else
	    {
		mpq_t div;
		rep_number_q *q = make_number (rep_NUMBER_RATIONAL);
		mpq_init (q->q);
		mpq_set_z (q->q, rep_NUMBER (x,z));
		mpq_init (div);
		mpq_set_z (div, rep_NUMBER (y,z));
		mpq_div (q->q, q->q, div);
		mpq_clear (div);
		out = rep_VAL (q);
	    }
	}
	break;

    case rep_NUMBER_RATIONAL:
	mpq_div (rep_NUMBER (out,q), rep_NUMBER (x,q), rep_NUMBER (y,q));
	out = maybe_demote (out);
	break;
    
    case rep_NUMBER_FLOAT:
	rep_NUMBER (out,f) = rep_NUMBER (x,f) / rep_NUMBER (y,f);
	break;
    }
    return out;
}

repv
rep_number_lognot (repv x)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (x))
    {
	rep_number_z *z;

    case rep_NUMBER_INT:
	out = rep_MAKE_INT (~rep_INT (x));
	break;

    case rep_NUMBER_BIGNUM:
	z = make_number (rep_NUMBER_BIGNUM);
	mpz_init (z->z);
	mpz_com (z->z, rep_NUMBER (x,z));
	out = rep_VAL (z);
	break;

    default:
	return rep_signal_arg_error (x, 1);
    }
    return out;
}

repv
rep_number_logior (repv x, repv y)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    rep_DECLARE2 (y, rep_NUMERICP);
    out = promote_dup (&x, &y);
    switch (rep_NUMERIC_TYPE (out))
    {
    case rep_NUMBER_INT:
	out = rep_MAKE_INT (rep_INT (x) | rep_INT (y));
	break;

    case rep_NUMBER_BIGNUM:
	mpz_ior (rep_NUMBER (out,z), rep_NUMBER (x,z), rep_NUMBER (y,z));
	break;

    default:
	return rep_signal_arg_error (x, 1);
    }
    return out;
}

repv
rep_number_logxor (repv x, repv y)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    rep_DECLARE2 (y, rep_NUMERICP);
    out = promote_dup (&x, &y);
    switch (rep_NUMERIC_TYPE (out))
    {
	mpz_t tem;

    case rep_NUMBER_INT:
	out = rep_MAKE_INT (rep_INT (x) ^ rep_INT (y));
	break;

    case rep_NUMBER_BIGNUM:
	/* XXX is this correct: x^y = x|y & ~(x&y) */
	mpz_init (tem);
	mpz_ior (tem, rep_NUMBER (x,z), rep_NUMBER (y,z));
	mpz_and (rep_NUMBER (out,z), rep_NUMBER (x,z), rep_NUMBER (y,z));
	mpz_com (rep_NUMBER (out,z), rep_NUMBER (out,z));
	mpz_and (rep_NUMBER (out,z), rep_NUMBER (out,z), tem);
	mpz_clear (tem);
	break;

    default:
	return rep_signal_arg_error (x, 1);
    }
    return out;
}

repv
rep_number_logand (repv x, repv y)
{
    repv out;
    rep_DECLARE1 (x, rep_NUMERICP);
    rep_DECLARE2 (y, rep_NUMERICP);
    out = promote_dup (&x, &y);
    switch (rep_NUMERIC_TYPE (out))
    {
    case rep_NUMBER_INT:
	out = rep_MAKE_INT (rep_INT (x) & rep_INT (y));
	break;

    case rep_NUMBER_BIGNUM:
	mpz_and (rep_NUMBER (out,z), rep_NUMBER (x,z), rep_NUMBER (y,z));
	break;

    default:
	return rep_signal_arg_error (x, 1);
    }
    return out;
}

DEFUN("+", Fplus, Splus, (repv args), rep_SubrN) /*
::doc:+::
+ NUMBERS...

Adds all NUMBERS together. If no arguments are given returns 0.
::end:: */
{
    if (args == Qnil)
	return rep_MAKE_INT (0);
    else
	return rep_number_foldl (args, rep_number_add);
}

DEFUN("-", Fminus, Sminus, (repv args), rep_SubrN) /*
::doc:-::
- NUMBER [NUMBERS...]

Either returns the negation of NUMBER or the value of NUMBER minus
NUMBERS
::end:: */
{
    if (args == Qnil)
	return rep_signal_missing_arg (1);
    else if (!rep_CONSP (rep_CDR (args)))
	return rep_number_neg (rep_CAR (args));
    else
	return rep_number_foldl (args, rep_number_sub);
}

DEFUN("*", Fproduct, Sproduct, (repv args), rep_SubrN) /*
::doc:*::
* NUMBERS...

Multiplies all NUMBERS together. If no numbers are given returns 1.
::end:: */
{
    if (args == Qnil)
	return rep_MAKE_INT (1);
    else
	return rep_number_foldl (args, rep_number_mul);
}

DEFUN("/", Fdivide, Sdivide, (repv args), rep_SubrN) /*
::doc:/::
/ NUMBERS...

Divides NUMBERS (in left-to-right order).
::end:: */
{
    return rep_number_foldl (args, rep_number_div);
}

DEFUN("remainder", Fremainder, Sremainder, (repv n1, repv n2), rep_Subr2) /*
::doc:remainder::
remainder DIVIDEND DIVISOR

Returns the integer remainder after dividing DIVIDEND by DIVISOR.
::end:: */
{
    repv out;
    rep_DECLARE1(n1, rep_NUMERICP);
    rep_DECLARE2(n2, rep_NUMERICP);
    if(Fzerop (n2) != Qnil)
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&div_zero)));

    out = promote_dup (&n1, &n2);
    switch (rep_NUMERIC_TYPE (out))
    {
    case rep_NUMBER_INT:
	out = rep_MAKE_INT (rep_INT (n1) % rep_INT (n2));
	break;

    case rep_NUMBER_BIGNUM:
	mpz_tdiv_r (rep_NUMBER(out,z), rep_NUMBER(n1,z), rep_NUMBER(n2,z));
	out = maybe_demote (out);
	break;

    default:
	return rep_signal_arg_error (n1, 1);
    }
    return out;
}

DEFUN("mod", Fmod, Smod, (repv n1, repv n2), rep_Subr2) /*
::doc:mod::
mod DIVIDEND DIVISOR

Returns the value of DIVIDEND modulo DIVISOR; unlike the % (remainder)
function the behaviour of `mod' is well-defined for negative arguments,
we have that,

	(mod X Y) == X - (* Y (floor (/ X Y))),	for Y not equal to zero

assuming that (floor Z) gives the least integer greater than or equal to Z,
and that floating point division is used.
::end:: */
{
    repv out;
    rep_DECLARE1(n1, rep_NUMERICP);
    rep_DECLARE2(n2, rep_NUMERICP);
    if(Fzerop (n2) != Qnil)
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&div_zero)));

    out = promote_dup (&n1, &n2);
    switch (rep_NUMERIC_TYPE (out))
    {
	long tem;
	int sign;

    case rep_NUMBER_INT:
	/* This code from GNU Emacs */
	tem = rep_INT (n1) % rep_INT (n2);
	/* If the "remainder" comes out with the wrong sign, fix it.  */
	if (rep_INT (n2) < 0 ? tem > 0 : tem < 0)
	    tem += rep_INT (n2);
	out = rep_MAKE_INT (tem);
	break;

    case rep_NUMBER_BIGNUM:
	mpz_tdiv_r (rep_NUMBER(out,z), rep_NUMBER(n1,z), rep_NUMBER(n2,z));
	/* If the "remainder" comes out with the wrong sign, fix it.  */
	sign = mpz_sgn (rep_NUMBER(out,z));
	if (mpz_sgn (rep_NUMBER(n2,z)) < 0 ? sign > 0 : sign < 0)
	    mpz_add (rep_NUMBER(out,z), rep_NUMBER(out,z), rep_NUMBER(n2,z));
	out = maybe_demote (out);
	break;

    default:
	return rep_signal_arg_error (n1, 1);
    }
    return out;
}

DEFUN("quotient", Fquotient, Squotient, (repv x, repv y), rep_Subr2) /*
::doc:quotient::
quotient DIVIDEND DIVISOR

Returns the integer quotient from dividing integers DIVIDEND and
DIVISOR.
::end:: */
{
    repv out;
    rep_DECLARE1 (x, rep_INTEGERP);
    rep_DECLARE2 (y, rep_INTEGERP);
    out = promote_dup (&x, &y);
    if (rep_INTP (x))
	out = rep_MAKE_INT (rep_INT (x) / rep_INT (y));
    else
    {
	mpz_tdiv_q (rep_NUMBER(out,z), rep_NUMBER(x,z), rep_NUMBER(y,z));
	out = maybe_demote (out);
    }
    return out;
}

DEFUN("lognot", Flognot, Slognot, (repv num), rep_Subr1) /*
::doc:lognot::
lognot NUMBER

Returns the bitwise logical `not' of NUMBER.
::end:: */
{
    rep_DECLARE1(num, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (num))
    {
	rep_number_z *z;

    case rep_NUMBER_INT:
	return rep_MAKE_INT (~rep_INT (num));

    case rep_NUMBER_BIGNUM:
	z = make_number (rep_NUMBER_BIGNUM);
	mpz_init (z->z);
	mpz_com (z->z, rep_NUMBER (num,z));
	return rep_VAL (z);

    default:
	return rep_signal_arg_error (num, 1);
    }
}

DEFUN("logior", Flogior, Slogior, (repv args), rep_SubrN) /*
::doc:logior::
logior NUMBERS...

Returns the bitwise logical `inclusive-or' of its arguments.
::end:: */
{
    if (args == Qnil)
	return rep_MAKE_INT (0);
    else
	return rep_number_foldl (args, rep_number_logior);
}

DEFUN("logxor", Flogxor, Slogxor, (repv args), rep_SubrN) /*
::doc:logxor::
logxor NUMBERS...

Returns the bitwise logical `exclusive-or' of its arguments.
::end:: */
{
    return rep_number_foldl (args, rep_number_logxor);
}

DEFUN("logand", Flogand, Slogand, (repv args), rep_SubrN) /*
::doc:logand::
logand NUMBERS...

Returns the bitwise logical `and' of its arguments.
::end:: */
{
    return rep_number_foldl (args, rep_number_logand);
}

DEFUN("eql", Feql, Seql, (repv arg1, repv arg2), rep_Subr2) /*
::doc:eql::
eql ARG1 ARG2

Similar to `eq' except that numbers with the same value will always be
considered `eql' (this may or may not be the case with `eq').
::end:: */
{
    if(rep_NUMERICP (arg1) && rep_NUMERICP (arg2))
	return number_cmp (arg1, arg2) == 0 ? Qt : Qnil;
    else
	return arg1 == arg2 ? Qt : Qnil;
}

DEFUN("zerop", Fzerop, Szerop, (repv num), rep_Subr1) /*
::doc:zerop::
zerop NUMBER

Return t if NUMBER is zero.
::end:: */
{
    if(rep_NUMERICP (num))
    {
	switch (rep_NUMERIC_TYPE (num))
	{
	case rep_NUMBER_INT:
	    return rep_INT (num) == 0 ? Qt : Qnil;

	case rep_NUMBER_BIGNUM:
	    return mpz_sgn (rep_NUMBER(num,z)) == 0 ? Qt : Qnil;

	case rep_NUMBER_RATIONAL:
	    return mpq_sgn (rep_NUMBER(num,q)) == 0 ? Qt : Qnil;

	case rep_NUMBER_FLOAT:
	    return rep_NUMBER(num,f) == 0 ? Qt : Qnil;
	}
    }
    return Qnil;
}

DEFUN("1+", Fplus1, Splus1, (repv num), rep_Subr1) /*
::doc:1+::
1+ NUMBER

Return NUMBER plus 1.
::end:: */
{
    rep_DECLARE1(num, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (num))
    {
	mpq_t temq;

    case rep_NUMBER_INT:
	return rep_make_long_int (rep_INT (num) + 1);

    case rep_NUMBER_BIGNUM:
	num = dup (num);
	mpz_add_ui (rep_NUMBER (num,z), rep_NUMBER (num,z), 1);
	return maybe_demote (num);

    case rep_NUMBER_RATIONAL:
	num = dup (num);
	mpq_init (temq);
	mpq_set_ui (temq, 1, 1);
	mpq_add (rep_NUMBER (num,q), rep_NUMBER (num,q), temq);
	mpq_clear (temq);
	return maybe_demote (num);

    case rep_NUMBER_FLOAT:
	num = dup (num);
	rep_NUMBER (num,f) = rep_NUMBER (num,f) + 1;
	return num;
    }
    abort ();
}

DEFUN("1-", Fsub1, Ssub1, (repv num), rep_Subr1) /*
::doc:1-::
1- NUMBER

Return NUMBER minus 1.
::end:: */
{
    rep_DECLARE1(num, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (num))
    {
	mpq_t temq;

    case rep_NUMBER_INT:
	return rep_make_long_int (rep_INT (num) - 1);

    case rep_NUMBER_BIGNUM:
	num = dup (num);
	mpz_sub_ui (rep_NUMBER (num,z), rep_NUMBER (num,z), 1);
	return maybe_demote (num);

    case rep_NUMBER_RATIONAL:
	num = dup (num);
	mpq_init (temq);
	mpq_set_si (temq, 1, 1);
	mpq_sub (rep_NUMBER (num,q), rep_NUMBER (num,q), temq);
	mpq_clear (temq);
	return maybe_demote (num);

    case rep_NUMBER_FLOAT:
	num = dup (num);
	rep_NUMBER (num,f) = rep_NUMBER (num,f) - 1;
	return num;
    }
    abort ();
}

DEFUN("ash", Fash, Sash, (repv num, repv shift), rep_Subr2) /*
::doc:ash::
ash NUMBER COUNT

Use an arithmetic shift to shift the bits in NUMBER by COUNT bits to
the left, a negative COUNT means shift right.

Both NUMBER and COUNT must be integers.
::end:: */
{
    rep_DECLARE1(num, rep_INTEGERP);
    rep_DECLARE2(shift, rep_INTEGERP);

    shift = coerce (shift, rep_NUMBER_INT);
    switch (rep_NUMERIC_TYPE (num))
    {
	rep_number_z *z;
	rep_long_long tot;

    case rep_NUMBER_INT:
	if (rep_INT (shift) >= rep_LISP_INT_BITS)
	{
	    num = promote_to (num, rep_NUMBER_BIGNUM);
	    goto do_bignum;
	}
	else
	{
	    if (rep_INT (shift) > 0)
		tot = ((rep_long_long) rep_INT (num)) << rep_INT (shift);
	    else
		tot = ((rep_long_long) rep_INT (num)) >> -rep_INT (shift);
	}
	return rep_make_longlong_int (tot);

    case rep_NUMBER_BIGNUM:
    do_bignum:
	z = make_number (rep_NUMBER_BIGNUM);
	mpz_init (z->z);
	if (rep_INT (shift) > 0)
	    mpz_mul_2exp (z->z, rep_NUMBER (num,z), rep_INT (shift));
	else
	    mpz_div_2exp (z->z, rep_NUMBER (num,z), - rep_INT (shift));
	return maybe_demote (rep_VAL (z));

    default:
	return rep_signal_arg_error (num, 1);
    }
}

DEFUN("floor", Ffloor, Sfloor, (repv arg), rep_Subr1) /*
::doc:floor::
floor NUMBER

Round NUMBER downwards to the nearest integer less than or equal to
NUMBER.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (arg))
    {
	double d;

    case rep_NUMBER_INT:
    case rep_NUMBER_BIGNUM:
	return arg;

	/* XXX R4RS says `inexact in -> inexact out'. Why? */
    default:
        if (rep_NUMBER_RATIONAL_P (arg))
	    d = mpq_get_d (rep_NUMBER(arg,q));
	else
	    d = rep_NUMBER(arg,f);
	return rep_make_long_int ((long) floor (d));
    }
    abort ();
}	

DEFUN("ceiling", Fceiling, Sceiling, (repv arg), rep_Subr1) /*
::doc:ceiling::
ceiling NUMBER

Round NUMBER upwards to the nearest integer greater than or equal to
NUMBER.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (arg))
    {
	double d;

    case rep_NUMBER_INT:
    case rep_NUMBER_BIGNUM:
	return arg;

    default:
        if (rep_NUMBER_RATIONAL_P (arg))
	    d = mpq_get_d (rep_NUMBER(arg,q));
	else
	    d = rep_NUMBER(arg,f);
	return rep_make_long_int ((long) ceil (d));
    }
    abort ();
}

DEFUN("truncate", Ftruncate, Struncate, (repv arg), rep_Subr1) /*
::doc:truncate::
truncate NUMBER

Round NUMBER to the nearest integer between NUMBER and zero.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (arg))
    {
	double d;

    case rep_NUMBER_INT:
    case rep_NUMBER_BIGNUM:
	return arg;

    default:
        if (rep_NUMBER_RATIONAL_P (arg))
	    d = mpq_get_d (rep_NUMBER(arg,q));
	else
	    d = rep_NUMBER(arg,f);
	d = (d < 0.0) ? -floor (-d) : floor (d);
	return rep_make_long_int ((long) d);
    }
    abort ();
}

DEFUN("round", Fround, Sround, (repv arg), rep_Subr1) /*
::doc:round::
round NUMBER

Round NUMBER to the nearest integer. Halfway cases are rounded to the
nearest even integer.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    switch (rep_NUMERIC_TYPE (arg))
    {
	double d, plus_half, result;

    case rep_NUMBER_INT:
    case rep_NUMBER_BIGNUM:
	return arg;

    default:
        if (rep_NUMBER_RATIONAL_P (arg))
	    d = mpq_get_d (rep_NUMBER(arg,q));
	else
	    d = rep_NUMBER(arg,f);
	/* from guile */
	plus_half = d + 0.5;
	result = floor (plus_half);
	/* Adjust so that the round is towards even.  */
	d = ((plus_half == result && plus_half / 2 != floor (plus_half / 2))
	     ? result - 1 : result);
	return rep_make_long_int ((long) d);
    }
    abort ();
}

DEFUN("exp", Fexp, Sexp, (repv arg), rep_Subr1) /*
::doc:exp::
exp X

Return `e' (the base of natural logarithms) raised to the power X.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    return rep_make_float (exp (rep_get_float (arg)), rep_TRUE);
}

DEFUN("log", Flog, Slog, (repv arg), rep_Subr1) /*
::doc:log::
log X

Return the natural logarithm of X. An arithmetic error is signalled if
X is less than zero.
::end:: */
{
    double d;
    rep_DECLARE1 (arg, rep_NUMERICP);
    d = rep_get_float (arg);
    if (d >= 0)
	return rep_make_float (log (d), rep_TRUE);
    else
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&domain_error)));
}

DEFUN("sin", Fsin, Ssin, (repv arg), rep_Subr1) /*
::doc:sin::
sin X

Returns the sine of X, in radians.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    return rep_make_float (sin (rep_get_float (arg)), rep_TRUE);
}

DEFUN("cos", Fcos, Scos, (repv arg), rep_Subr1) /*
::doc:cos::
cos X

Returns the cosine of X, in radians.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    return rep_make_float (cos (rep_get_float (arg)), rep_TRUE);
}

DEFUN("tan", Ftan, Stan, (repv arg), rep_Subr1) /*
::doc:tan::
tan X

Returns the tangent of X, in radians.
::end:: */
{
    rep_DECLARE1 (arg, rep_NUMERICP);
    return rep_make_float (tan (rep_get_float (arg)), rep_TRUE);
}

DEFUN("asin", Fasin, Sasin, (repv arg), rep_Subr1) /*
::doc:asin::
asin X

Return the arc sine of X (the value whose sine is X), in radians.
::end:: */
{
    double d;
    rep_DECLARE1 (arg, rep_NUMERICP);
    d = rep_get_float (arg);
    if (d >= -1.0 && d <= 1.0)
	return rep_make_float (asin (d), rep_TRUE);
    else
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&domain_error)));
}

DEFUN("acos", Facos, Sacos, (repv arg), rep_Subr1) /*
::doc:acos::
acos X

Return the arc cosine of X (the value whose cosine is X), in radians.
::end:: */
{
    double d;
    rep_DECLARE1 (arg, rep_NUMERICP);
    d = rep_get_float (arg);
    if (d >= -1.0 && d <= 1.0)
	return rep_make_float (acos (d), rep_TRUE);
    else
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&domain_error)));
}

DEFUN("atan", Fatan, Satan, (repv y, repv x), rep_Subr2) /*
::doc:atan::
atan X

Returns the arc tangent of X (the value whose tangent is X), in
radians.

atan Y X

Returns the arc tangent of Y/X, in radians. The signs of both arguments
are used to determine the quadrant of the result, and X is permitted to
be zero.
::end:: */
{
    rep_DECLARE1 (y, rep_NUMERICP);
    if (!rep_NUMERICP (x))
	return rep_make_float (atan (rep_get_float (y)), rep_TRUE);
    else
	return rep_make_float (atan2 (rep_get_float (y),
				      rep_get_float (x)), rep_TRUE);
}

DEFUN("sqrt", Fsqrt, Ssqrt, (repv arg), rep_Subr1) /*
::doc:sqrt::
sqrt X

Returns the nonnegative square root of X. If X is negative, signals an
arithmetic error (should return a complex number).
::end:: */
{
    double d;
    rep_DECLARE1 (arg, rep_NUMERICP);
    d = rep_get_float (arg);
    if (d >= 0)
	return rep_make_float (sqrt (d), rep_TRUE);
    else
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&domain_error)));
}

DEFUN("expt", Fexpt, Sexpt, (repv arg1, repv arg2), rep_Subr2) /*
::doc:expt::
expt X Y

Returns X raised to the power Y.

If X is negative and Y is a non-integer, then an arithmetic error is
signalled (mathematically should return a complex number).
::end:: */
{
    double x, y;
    rep_DECLARE1 (arg1, rep_NUMERICP);
    rep_DECLARE1 (arg2, rep_NUMERICP);
    x = rep_get_float (arg1);
    y = rep_get_float (arg2);
    if (x >= 0 || ceil (y) == y)
	return rep_make_float (pow (x, y), rep_FALSE);
    else
	return Fsignal (Qarith_error, rep_LIST_1 (rep_VAL (&domain_error)));
}

DEFUN("gcd", Fgcd, Sgcd, (repv x, repv y), rep_Subr2) /*
::doc:gcd::
gcd X Y

Return the greatest common divisor of the integers X and Y.
::end:: */
{
    repv out;
    rep_DECLARE1 (x, rep_INTEGERP);
    rep_DECLARE1 (y, rep_INTEGERP);
    out = promote_dup (&x, &y);
    if (rep_INTP (x))
    {
	/* Euclid's algorithm */
	long m = rep_INT (x), n = rep_INT (y);
	while(m != 0)
	{
	    long t = n % m;
	    n = m;
	    m = t;
	}
	out = rep_MAKE_INT (n);
    }
    else
	mpz_gcd (rep_NUMBER(out,z), rep_NUMBER(x,z), rep_NUMBER(y,z));
    return out;
}

DEFUN("numberp", Fnumberp, Snumberp, (repv arg), rep_Subr1) /*
::doc:numberp::
numberp ARG

Return t if ARG is a number.
::end:: */
{
    return rep_NUMERICP (arg) ? Qt : Qnil;
}

DEFUN("integerp", Fintegerp, Sintegerp, (repv arg), rep_Subr1) /*
::doc:integerp::
integerp ARG

Return t if ARG is a integer.
::end:: */
{
    return (rep_INTP (arg)
	    || (rep_NUMBERP (arg)
		&& rep_NUMBER_BIGNUM_P (arg))) ? Qt : Qnil;
}

DEFUN("fixnump", Ffixnump, Sfixnump, (repv arg), rep_Subr1) /*
::doc:fixnump::
fixnump ARG

Return t if ARG is a fixnum (i.e. an integer that fits in a Lisp
pointer).
::end:: */
{
    return rep_INTP (arg) ? Qt : Qnil;
}

DEFUN("rationalp", Frationalp, Srationalp, (repv arg), rep_Subr1) /*
::doc:rationalp::
rationalp ARG

Return t if ARG is a rational number.
::end:: */
{
    return (rep_INTP (arg)
	    || (rep_NUMBERP (arg)
		&& (rep_NUMBER_BIGNUM_P (arg)
		    || rep_NUMBER_RATIONAL_P (arg)))) ? Qt : Qnil;
}

DEFUN("realp", Frealp, Srealp, (repv arg), rep_Subr1) /*
::doc:realp::
realp ARG

Return t if ARG is a real number.
::end:: */
{
    return rep_NUMERICP (arg) ? Qt : Qnil;
}

DEFUN("exactp", Fexactp, Sexactp, (repv arg), rep_Subr1) /*
::doc:exactp::
exactp ARG

Return t if ARG is an exact number.
::end:: */
{
    return Frationalp (arg);
}

DEFUN("inexactp", Finexactp, Sinexactp, (repv arg), rep_Subr1) /*
::doc:inexactp::
inexactp ARG

Return t if ARG is an inexact number.
::end:: */
{
    return Fexactp (arg) == Qnil ? Qt : Qnil;
}

DEFUN("exact->inexact", Fexact_to_inexact,
      Sexact_to_inexact, (repv arg), rep_Subr1) /*
::doc:exact->inexact::
exact->inexact X

Returns an inexact (i.e. floating point) representation of X.
::end:: */
{
    rep_DECLARE1(arg, rep_NUMERICP);
    if (!rep_INTP (arg) && rep_NUMBER_FLOAT_P (arg))
	return arg;
    else
	return rep_make_float (rep_get_float (arg), rep_TRUE);
}

DEFUN("inexact->exact", Finexact_to_exact,
      Sinexact_to_exact, (repv arg), rep_Subr1) /*
::doc:inexact->exact::
inexact->exact X

Returns an exact representation of X. This may involve a loss of
accuracy.
::end:: */
{
    rep_DECLARE1(arg, rep_NUMERICP);
    if (rep_INTP (arg) || !rep_NUMBER_FLOAT_P (arg))
	return arg;
    else
    {
	/* XXX is there a way to decide if it's rationalizable? */
	double d = floor (rep_get_float (arg));
	if (d >= rep_LISP_MIN_INT && d <= rep_LISP_MAX_INT)
	    return rep_MAKE_INT ((long) d);
	else
	{
	    rep_number_z *z = make_number (rep_NUMBER_BIGNUM);
	    mpz_init_set_d (z->z, d);
	    return rep_VAL (z);
	}
    }
}

DEFUN("numerator", Fnumerator, Snumerator, (repv x), rep_Subr1) /*
::doc:numerator::
numerator X

Return the numerator of rational number X.
::end:: */
{
    rep_DECLARE1(x, rep_NUMERICP);
    if (rep_INTP (x) || rep_NUMBER_BIGNUM_P (x))
	return x;
    else if (rep_NUMBER_RATIONAL_P (x))
    {
	rep_number_z *z = make_number (rep_NUMBER_BIGNUM);
	mpz_init_set (z->z, mpq_numref (rep_NUMBER(x,q)));
	return maybe_demote (rep_VAL (z));
    }
    else
	return rep_signal_arg_error (x, 1);
}

DEFUN("denominator", Fdenominator, Sdenominator, (repv x), rep_Subr1) /*
::doc:denominator::
denominator X

Return the denominator of rational number X.
::end:: */
{
    rep_DECLARE1(x, rep_NUMERICP);
    if (rep_INTP (x) || rep_NUMBER_BIGNUM_P (x))
	return rep_MAKE_INT (1);
    else if (rep_NUMBER_RATIONAL_P (x))
    {
	rep_number_z *z = make_number (rep_NUMBER_BIGNUM);
	mpz_init_set (z->z, mpq_denref (rep_NUMBER(x,q)));
	return maybe_demote (rep_VAL (z));
    }
    else
	return rep_signal_arg_error (x, 1);
}


/* init */

void
rep_numbers_init (void)
{
    int i;
    rep_register_type(rep_Int, "integer", number_cmp,
		      number_prin, number_prin,
		      0, 0, 0, 0, 0, 0, 0, 0, 0);
    rep_register_type(rep_Number, "number", number_cmp,
		      number_prin, number_prin,
		      number_sweep, 0, 0, 0, 0, 0, 0, 0, 0);

    number_sizeofs[0] = sizeof (rep_number_z);
    number_sizeofs[1] = sizeof (rep_number_q);
    number_sizeofs[2] = sizeof (rep_number_f);
    for (i = 0; i < 3; i++)
    {
	number_allocations[i] = ((2040 - sizeof (rep_number_block))
				 / number_sizeofs[i]);
    }

    rep_ADD_SUBR(Splus);
    rep_ADD_SUBR(Sminus);
    rep_ADD_SUBR(Sproduct);
    rep_ADD_SUBR(Sdivide);
    rep_ADD_SUBR(Sremainder);
    rep_ADD_SUBR(Smod);
    rep_ADD_SUBR(Squotient);
    rep_ADD_SUBR(Slognot);
    rep_ADD_SUBR(Seql);
    rep_ADD_SUBR(Slogior);
    rep_ADD_SUBR(Slogxor);
    rep_ADD_SUBR(Slogand);
    rep_ADD_SUBR(Szerop);
    rep_ADD_SUBR(Splus1);
    rep_ADD_SUBR(Ssub1);
    rep_ADD_SUBR(Sash);
    rep_ADD_SUBR(Sfloor);
    rep_ADD_SUBR(Sceiling);
    rep_ADD_SUBR(Struncate);
    rep_ADD_SUBR(Sround);
    rep_ADD_SUBR(Sexp);
    rep_ADD_SUBR(Slog);
    rep_ADD_SUBR(Ssin);
    rep_ADD_SUBR(Scos);
    rep_ADD_SUBR(Stan);
    rep_ADD_SUBR(Sasin);
    rep_ADD_SUBR(Sacos);
    rep_ADD_SUBR(Satan);
    rep_ADD_SUBR(Ssqrt);
    rep_ADD_SUBR(Sexpt);
    rep_ADD_SUBR(Sgcd);
    rep_ADD_SUBR(Snumberp);
    rep_ADD_SUBR(Sintegerp);
    rep_ADD_SUBR(Sfixnump);
    rep_ADD_SUBR(Srationalp);
    rep_ADD_SUBR(Srealp);
    rep_ADD_SUBR(Sexactp);
    rep_ADD_SUBR(Sinexactp);
    rep_ADD_SUBR(Sexact_to_inexact);
    rep_ADD_SUBR(Sinexact_to_exact);
    rep_ADD_SUBR(Snumerator);
    rep_ADD_SUBR(Sdenominator);
}