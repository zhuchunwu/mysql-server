/*****************************************************************************

Copyright (c) 1994, 2013, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/*******************************************************************//**
@file rem/rem0cmp.cc
Comparison services for records

Created 7/1/1994 Heikki Tuuri
************************************************************************/

#include "rem0cmp.h"

#ifdef UNIV_NONINL
#include "rem0cmp.ic"
#endif

#include "ha_prototypes.h"

#include "handler0alter.h"
#include "srv0srv.h"

#include <algorithm>

using std::min;

/*		ALPHABETICAL ORDER
		==================

The records are put into alphabetical order in the following
way: let F be the first field where two records disagree.
If there is a character in some position n where the
records disagree, the order is determined by comparison of
the characters at position n, possibly after
collating transformation. If there is no such character,
but the corresponding fields have different lengths, then
if the data type of the fields is paddable,
shorter field is padded with a padding character. If the
data type is not paddable, longer field is considered greater.
Finally, the SQL null is bigger than any other value.

At the present, the comparison functions return 0 in the case,
where two records disagree only in the way that one
has more fields than the other. */

#ifdef UNIV_DEBUG
/*************************************************************//**
Used in debug checking of cmp_dtuple_... .
This function is used to compare a data tuple to a physical record. If
dtuple has n fields then rec must have either m >= n fields, or it must
differ from dtuple in some of the m fields rec has.
@return 1, 0, -1, if dtuple is greater, equal, less than rec,
respectively, when only the common first fields are compared */
static
int
cmp_debug_dtuple_rec_with_match(
/*============================*/
	const dtuple_t*	dtuple,	/*!< in: data tuple */
	const rec_t*	rec,	/*!< in: physical record which differs from
				dtuple in some of the common fields, or which
				has an equal number or more fields than
				dtuple */
	const ulint*	offsets,/*!< in: array returned by rec_get_offsets();
				may be NULL for ROW_FORMAT=REDUNDANT */
	ulint		n_cmp,	/*!< in: number of fields to compare */
	ulint*		matched_fields)/*!< in/out: number of already
				completely  matched fields; when function
				returns, contains the value for current
				comparison */
	__attribute__((nonnull, warn_unused_result));
#endif /* UNIV_DEBUG */

/*************************************************************//**
Compare two data fields.
@return	1, 0, -1, if a is greater, equal, less than b, respectively */
UNIV_INLINE
int
innobase_mysql_cmp(
/*===============*/
	ulint		prtype,		/*!< in: precise type */
	const byte*	a,		/*!< in: data field */
	unsigned int	a_length,	/*!< in: data field length,
					not UNIV_SQL_NULL */
	const byte*	b,		/*!< in: data field */
	unsigned int	b_length)	/*!< in: data field length,
					not UNIV_SQL_NULL */
{
#ifdef UNIV_DEBUG
	switch (prtype & DATA_MYSQL_TYPE_MASK) {
	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
	case MYSQL_TYPE_VARCHAR:
		break;
	default:
		ut_error;
	}
#endif /* UNIV_DEBUG */

	uint cs_num = dtype_get_charset_coll(prtype);

	if (CHARSET_INFO* cs = get_charset(cs_num, MYF(MY_WME))) {
		int cmp = cs->coll->strnncollsp(
			cs, a, a_length, b, b_length, 0);
		return(cmp < 0 ? -1 : !!cmp);
	}

	ib_logf(IB_LOG_LEVEL_FATAL,
		"Unable to find charset-collation %u", cs_num);
	return(0);
}

/*************************************************************//**
Returns TRUE if two columns are equal for comparison purposes.
@return	TRUE if the columns are considered equal in comparisons */

ibool
cmp_cols_are_equal(
/*===============*/
	const dict_col_t*	col1,	/*!< in: column 1 */
	const dict_col_t*	col2,	/*!< in: column 2 */
	ibool			check_charsets)
					/*!< in: whether to check charsets */
{
	if (dtype_is_non_binary_string_type(col1->mtype, col1->prtype)
	    && dtype_is_non_binary_string_type(col2->mtype, col2->prtype)) {

		/* Both are non-binary string types: they can be compared if
		and only if the charset-collation is the same */

		if (check_charsets) {
			return(dtype_get_charset_coll(col1->prtype)
			       == dtype_get_charset_coll(col2->prtype));
		} else {
			return(TRUE);
		}
	}

	if (dtype_is_binary_string_type(col1->mtype, col1->prtype)
	    && dtype_is_binary_string_type(col2->mtype, col2->prtype)) {

		/* Both are binary string types: they can be compared */

		return(TRUE);
	}

	if (col1->mtype != col2->mtype) {

		return(FALSE);
	}

	if (col1->mtype == DATA_INT
	    && (col1->prtype & DATA_UNSIGNED)
	    != (col2->prtype & DATA_UNSIGNED)) {

		/* The storage format of an unsigned integer is different
		from a signed integer: in a signed integer we OR
		0x8000... to the value of positive integers. */

		return(FALSE);
	}

	return(col1->mtype != DATA_INT || col1->len == col2->len);
}

/*************************************************************//**
Innobase uses this function to compare two data fields for which the data type
is such that we must compare whole fields or call MySQL to do the comparison
@return	1, 0, -1, if a is greater, equal, less than b, respectively */
static
int
cmp_whole_field(
/*============*/
	ulint		mtype,		/*!< in: main type */
	ulint		prtype,		/*!< in: precise type */
	const byte*	a,		/*!< in: data field */
	unsigned int	a_length,	/*!< in: data field length,
					not UNIV_SQL_NULL */
	const byte*	b,		/*!< in: data field */
	unsigned int	b_length)	/*!< in: data field length,
					not UNIV_SQL_NULL */
{
	float		f_1;
	float		f_2;
	double		d_1;
	double		d_2;
	int		swap_flag	= 1;

	switch (mtype) {
		int	cmp;

	case DATA_DECIMAL:
		/* Remove preceding spaces */
		for (; a_length && *a == ' '; a++, a_length--) { }
		for (; b_length && *b == ' '; b++, b_length--) { }

		if (*a == '-') {
			if (*b != '-') {
				return(-1);
			}

			a++; b++;
			a_length--;
			b_length--;

			swap_flag = -1;

		} else if (*b == '-') {

			return(1);
		}

		while (a_length > 0 && (*a == '+' || *a == '0')) {
			a++; a_length--;
		}

		while (b_length > 0 && (*b == '+' || *b == '0')) {
			b++; b_length--;
		}

		if (a_length != b_length) {
			if (a_length < b_length) {
				return(-swap_flag);
			}

			return(swap_flag);
		}

		while (a_length > 0 && *a == *b) {

			a++; b++; a_length--;
		}

		if (a_length == 0) {

			return(0);
		}

		if (*a > *b) {
			return(swap_flag);
		}

		return(-swap_flag);
	case DATA_DOUBLE:
		d_1 = mach_double_read(a);
		d_2 = mach_double_read(b);

		if (d_1 > d_2) {
			return(1);
		} else if (d_2 > d_1) {
			return(-1);
		}

		return(0);

	case DATA_FLOAT:
		f_1 = mach_float_read(a);
		f_2 = mach_float_read(b);

		if (f_1 > f_2) {
			return(1);
		} else if (f_2 > f_1) {
			return(-1);
		}

		return(0);
	case DATA_VARCHAR:
	case DATA_CHAR:
		cmp = my_charset_latin1.coll->strnncollsp(
			&my_charset_latin1, a, a_length, b, b_length, 0);

		if (cmp < 0) {
			return(-1);
		} else if (cmp) {
			return(1);
		} else {
			return(0);
		}
	case DATA_BLOB:
		if (prtype & DATA_BINARY_TYPE) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"comparing a binary BLOB"
				" using a character set collation!");
			ut_ad(0);
		}
		/* fall through */
	case DATA_VARMYSQL:
	case DATA_MYSQL:
		return(innobase_mysql_cmp(prtype,
					  a, a_length, b, b_length));
	case DATA_GEOMETRY:
		return(0);
	default:
		ib_logf(IB_LOG_LEVEL_FATAL,
			"unknown data type number %lu",
			(ulong) mtype);
	}

	return(0);
}

/** Compare two data fields.
@param[in]	mtype	main type
@param[in]	prtype	precise type
@param[in]	data1	data field
@param[in]	len1	length of data1 in bytes, or UNIV_SQL_NULL
@param[in]	data2	data field
@param[in]	len2	length of data2 in bytes, or UNIV_SQL_NULL
@return	the comparison result of data1 and data2
@retval	0 if data1 is equal to data2
@retval	-1 if data1 is less than data2
@retval	1 if data1 is greater than data2 */
inline
int
cmp_data(
	ulint		mtype,
	ulint		prtype,
	const byte*	data1,
	ulint		len1,
	const byte*	data2,
	ulint		len2)
{
	if (len1 == UNIV_SQL_NULL || len2 == UNIV_SQL_NULL) {
		if (len1 == len2) {
			return(0);
		}

		/* We define the SQL null to be the smallest possible
		value of a field. */
		return(len1 == UNIV_SQL_NULL ? -1 : 1);
	}

	switch (mtype) {
	case DATA_FIXBINARY:
	case DATA_BINARY:
	case DATA_INT:
	case DATA_SYS_CHILD:
	case DATA_SYS:
		break;
	case DATA_BLOB:
		if (prtype & DATA_BINARY_TYPE) {
			break;
		}
		/* fall through */
	default:
		return(cmp_whole_field(mtype, prtype,
				       data1, (unsigned) len1,
				       data2, (unsigned) len2));
	}

	ulint len = min(len1, len2);

	if (int ret = memcmp(data1, data2, len)) {
		return(ret < 0 ? -1 : 1);
	} else if (len1 == len2) {
		return(0);
	}

	const ulint	pad = dtype_get_pad_char(mtype, prtype);

	if (pad == ULINT_UNDEFINED) {
		return(len < len1 ? 1 : -1);
	}

	if (len < len1) {
		do {
			byte	b = data1[len++];
			if (b != pad) {
				return(b < pad ? -1 : 1);
			}
		} while (len < len1);
	} else {
		ut_ad(len < len2);

		do {
			byte	b = data2[len++];
			if (b != pad) {
				return(pad < b ? -1 : 1);
			}
		} while (len < len2);
	}

	return(0);
}

/*************************************************************//**
This function is used to compare two data fields for which we know the
data type.
@return	1, 0, -1, if data1 is greater, equal, less than data2, respectively */

int
cmp_data_data(
/*==========*/
	ulint		mtype,	/*!< in: main type */
	ulint		prtype,	/*!< in: precise type */
	const byte*	data1,	/*!< in: data field (== a pointer to a memory
				buffer) */
	ulint		len1,	/*!< in: data field length or UNIV_SQL_NULL */
	const byte*	data2,	/*!< in: data field (== a pointer to a memory
				buffer) */
	ulint		len2)	/*!< in: data field length or UNIV_SQL_NULL */
{
	return(cmp_data(mtype, prtype, data1, len1, data2, len2));
}

/*************************************************************//**
This function is used to compare a data tuple to a physical record.
Only dtuple->n_fields_cmp first fields are taken into account for
the data tuple! If we denote by n = n_fields_cmp, then rec must
have either m >= n fields, or it must differ from dtuple in some of
the m fields rec has. If rec has an externally stored field we do not
compare it but return with value 0 if such a comparison should be
made.
@return 1, 0, -1, if dtuple is greater, equal, less than rec,
respectively, when only the common first fields are compared, or until
the first externally stored field in rec */

int
cmp_dtuple_rec_with_match_low(
/*==========================*/
	const dtuple_t*	dtuple,	/*!< in: data tuple */
	const rec_t*	rec,	/*!< in: physical record which differs from
				dtuple in some of the common fields, or which
				has an equal number or more fields than
				dtuple */
	const ulint*	offsets,/*!< in: array returned by rec_get_offsets();
				may be NULL for ROW_FORMAT=REDUNDANT */
	ulint		n_cmp,	/*!< in: number of fields to compare */
	ulint*		matched_fields) /*!< in/out: number of already completely
				matched fields; when function returns,
				contains the value for current comparison */
{
	ulint		cur_field;	/* current field number */
	int		ret;		/* return value */

	ut_ad(dtuple_check_typed(dtuple));
	ut_ad(!offsets || rec_offs_validate(rec, NULL, offsets));

	cur_field = *matched_fields;

	ut_ad(n_cmp > 0);
	ut_ad(n_cmp <= dtuple_get_n_fields(dtuple));
	ut_ad(cur_field <= n_cmp);
	ut_ad(cur_field <= (offsets
			    ? rec_offs_n_fields(offsets)
			    : rec_get_n_fields_old(rec)));

	if (cur_field == 0) {
		ulint	rec_info = rec_get_info_bits(
			rec, offsets && rec_offs_comp(offsets));
		ulint	tup_info = dtuple_get_info_bits(dtuple);

		if (UNIV_UNLIKELY(rec_info & REC_INFO_MIN_REC_FLAG)) {
			ret = !(tup_info & REC_INFO_MIN_REC_FLAG);
			goto order_resolved;
		} else if (UNIV_UNLIKELY(tup_info & REC_INFO_MIN_REC_FLAG)) {
			ret = -1;
			goto order_resolved;
		}
	}

	/* Match fields in a loop */

	for (; cur_field < n_cmp; cur_field++) {
		const byte*	rec_b_ptr;
		const dfield_t*	dtuple_field
			= dtuple_get_nth_field(dtuple, cur_field);
		const byte*	dtuple_b_ptr
			= static_cast<const byte*>(
				dfield_get_data(dtuple_field));
		const dtype_t*	type
			= dfield_get_type(dtuple_field);
		ulint		dtuple_f_len
			= dfield_get_len(dtuple_field);
		ulint		rec_f_len;

		if (rec_get_nth_field_ext(rec, offsets, cur_field,
					  rec_b_ptr, rec_f_len)) {
			/* We should never compare against an
			externally stored field.  Only clustered index
			records can contain externally stored fields,
			and the first fields (primary key fields)
			should already differ. */
			ut_error;
		}

		ut_ad(!dfield_is_ext(dtuple_field));

		ret = cmp_data(type->mtype, type->prtype,
			       dtuple_b_ptr, dtuple_f_len,
			       rec_b_ptr, rec_f_len);
		if (ret) {
			goto order_resolved;
		}
	}

	ret = 0;	/* If we ran out of fields, dtuple was equal to rec
			up to the common fields */
order_resolved:
	ut_ad((ret >= - 1) && (ret <= 1));
	ut_ad(ret == cmp_debug_dtuple_rec_with_match(dtuple, rec, offsets,
						     n_cmp, matched_fields));
	ut_ad(*matched_fields == cur_field); /* In the debug version, the
					     above cmp_debug_... sets
					     *matched_fields to a value */
	*matched_fields = cur_field;

	return(ret);
}

/**************************************************************//**
Compares a data tuple to a physical record.
@see cmp_dtuple_rec_with_match
@return 1, 0, -1, if dtuple is greater, equal, less than rec, respectively */

int
cmp_dtuple_rec(
/*===========*/
	const dtuple_t*	dtuple,	/*!< in: data tuple */
	const rec_t*	rec,	/*!< in: physical record */
	const ulint*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint	matched_fields	= 0;

	ut_ad(rec_offs_validate(rec, NULL, offsets));
	return(cmp_dtuple_rec_with_match(dtuple, rec, offsets,
					 &matched_fields));
}

/**************************************************************//**
Checks if a dtuple is a prefix of a record. The last field in dtuple
is allowed to be a prefix of the corresponding field in the record.
@return	TRUE if prefix */

ibool
cmp_dtuple_is_prefix_of_rec(
/*========================*/
	const dtuple_t*	dtuple,	/*!< in: data tuple */
	const rec_t*	rec,	/*!< in: physical record */
	const ulint*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint	n_fields;
	ulint	matched_fields	= 0;

	ut_ad(rec_offs_validate(rec, NULL, offsets));
	n_fields = dtuple_get_n_fields(dtuple);

	if (n_fields > rec_offs_n_fields(offsets)) {
		ut_ad(0);
		return(FALSE);
	}

	cmp_dtuple_rec_with_match(dtuple, rec, offsets, &matched_fields);
	return(matched_fields == n_fields);
}

/*************************************************************//**
Compare two physical record fields.
@retval 1 if rec1 field is greater than rec2
@retval -1 if rec1 field is less than rec2
@retval 0 if rec1 field equals to rec2 */
static __attribute__((nonnull, warn_unused_result))
int
cmp_rec_rec_simple_field(
/*=====================*/
	const rec_t*		rec1,	/*!< in: physical record */
	const rec_t*		rec2,	/*!< in: physical record */
	const ulint*		offsets1,/*!< in: rec_get_offsets(rec1, ...) */
	const ulint*		offsets2,/*!< in: rec_get_offsets(rec2, ...) */
	const dict_index_t*	index,	/*!< in: data dictionary index */
	ulint			n)	/*!< in: field to compare */
{
	const byte*	rec1_b_ptr;
	const byte*	rec2_b_ptr;
	ulint		rec1_f_len;
	ulint		rec2_f_len;
	const dict_col_t*	col	= dict_index_get_nth_col(index, n);

	ut_ad(!rec_offs_nth_extern(offsets1, n));
	ut_ad(!rec_offs_nth_extern(offsets2, n));

	rec1_b_ptr = rec_get_nth_field(rec1, offsets1, n, &rec1_f_len);
	rec2_b_ptr = rec_get_nth_field(rec2, offsets2, n, &rec2_f_len);

	return(cmp_data(col->mtype, col->prtype,
			rec1_b_ptr, rec1_f_len, rec2_b_ptr, rec2_f_len));
}

/*************************************************************//**
Compare two physical records that contain the same number of columns,
none of which are stored externally.
@retval 1 if rec1 (including non-ordering columns) is greater than rec2
@retval -1 if rec1 (including non-ordering columns) is less than rec2
@retval 0 if rec1 is a duplicate of rec2 */

int
cmp_rec_rec_simple(
/*===============*/
	const rec_t*		rec1,	/*!< in: physical record */
	const rec_t*		rec2,	/*!< in: physical record */
	const ulint*		offsets1,/*!< in: rec_get_offsets(rec1, ...) */
	const ulint*		offsets2,/*!< in: rec_get_offsets(rec2, ...) */
	const dict_index_t*	index,	/*!< in: data dictionary index */
	struct TABLE*		table)	/*!< in: MySQL table, for reporting
					duplicate key value if applicable,
					or NULL */
{
	ulint		n;
	ulint		n_uniq	= dict_index_get_n_unique(index);
	bool		null_eq	= false;

	ut_ad(rec_offs_n_fields(offsets1) >= n_uniq);
	ut_ad(rec_offs_n_fields(offsets2) == rec_offs_n_fields(offsets2));

	ut_ad(rec_offs_comp(offsets1) == rec_offs_comp(offsets2));

	for (n = 0; n < n_uniq; n++) {
		int cmp = cmp_rec_rec_simple_field(
			rec1, rec2, offsets1, offsets2, index, n);

		if (cmp) {
			return(cmp);
		}

		/* If the fields are internally equal, they must both
		be NULL or non-NULL. */
		ut_ad(rec_offs_nth_sql_null(offsets1, n)
		      == rec_offs_nth_sql_null(offsets2, n));

		if (rec_offs_nth_sql_null(offsets1, n)) {
			ut_ad(!(dict_index_get_nth_col(index, n)->prtype
				& DATA_NOT_NULL));
			null_eq = true;
		}
	}

	/* If we ran out of fields, the ordering columns of rec1 were
	equal to rec2. Issue a duplicate key error if needed. */

	if (!null_eq && table && dict_index_is_unique(index)) {
		/* Report erroneous row using new version of table. */
		innobase_rec_to_mysql(table, rec1, index, offsets1);
		return(0);
	}

	/* Else, keep comparing so that we have the full internal
	order. */
	for (; n < dict_index_get_n_fields(index); n++) {
		int cmp = cmp_rec_rec_simple_field(
			rec1, rec2, offsets1, offsets2, index, n);

		if (cmp) {
			return(cmp);
		}

		/* If the fields are internally equal, they must both
		be NULL or non-NULL. */
		ut_ad(rec_offs_nth_sql_null(offsets1, n)
		      == rec_offs_nth_sql_null(offsets2, n));
	}

	/* This should never be reached. Internally, an index must
	never contain duplicate entries. */
	ut_ad(0);
	return(0);
}

/** Compare two B-tree records.
Only the common first fields are compared, and externally stored field
are treated as equal.
@param[in]	rec1		B-tree record
@param[in]	rec2		B-tree record
@param[in]	offsets1	rec_get_offsets(rec1, index)
@param[in]	offsets2	rec_get_offsets(rec2, index)
@param[in]	index		B-tree index
@param[in]	nulls_unequal	true if this is for index cardinality
statistics estimation, and innodb_stats_method=nulls_unequal
or innodb_stats_method=nulls_ignored
@param[out]	matched_fields	number of completely matched fields
@return 1, 0 , -1 if rec1 is greater, equal, less, respectively, than
rec2; only the common first fields are compared */

int
cmp_rec_rec_with_match(
	const rec_t*		rec1,
	const rec_t*		rec2,
	const ulint*		offsets1,
	const ulint*		offsets2,
	const dict_index_t*	index,
	bool			nulls_unequal,
	ulint*			matched_fields)
{
	ulint		rec1_n_fields;	/* the number of fields in rec */
	ulint		rec1_f_len;	/* length of current field in rec */
	const byte*	rec1_b_ptr;	/* pointer to the current byte
					in rec field */
	ulint		rec2_n_fields;	/* the number of fields in rec */
	ulint		rec2_f_len;	/* length of current field in rec */
	const byte*	rec2_b_ptr;	/* pointer to the current byte
					in rec field */
	ulint		cur_field;	/* current field number */
	int		ret = 0;	/* return value */
	ulint		comp;

	ut_ad(rec1 && rec2 && index);
	ut_ad(rec_offs_validate(rec1, index, offsets1));
	ut_ad(rec_offs_validate(rec2, index, offsets2));
	ut_ad(rec_offs_comp(offsets1) == rec_offs_comp(offsets2));

	comp = rec_offs_comp(offsets1);
	rec1_n_fields = rec_offs_n_fields(offsets1);
	rec2_n_fields = rec_offs_n_fields(offsets2);

	/* Test if rec is the predefined minimum record */
	if (UNIV_UNLIKELY(rec_get_info_bits(rec1, comp)
			  & REC_INFO_MIN_REC_FLAG)) {
		/* There should only be one such record. */
		ut_ad(!(rec_get_info_bits(rec2, comp)
			& REC_INFO_MIN_REC_FLAG));
		ret = -1;
		goto order_resolved;
	} else if (UNIV_UNLIKELY
		   (rec_get_info_bits(rec2, comp)
		    & REC_INFO_MIN_REC_FLAG)) {
		ret = 1;
		goto order_resolved;
	}

	/* Match fields in a loop */

	for (cur_field = 0;
	     cur_field < rec1_n_fields && cur_field < rec2_n_fields;
	     cur_field++) {

		ulint	mtype;
		ulint	prtype;

		if (dict_index_is_univ(index)) {
			/* This is for the insert buffer B-tree. */
			mtype = DATA_BINARY;
			prtype = 0;
		} else {
			const dict_col_t*	col
				= dict_index_get_nth_col(index, cur_field);

			mtype = col->mtype;
			prtype = col->prtype;
		}

		/* We should never encounter an externally stored field.
		Externally stored fields only exist in clustered index
		leaf page records. These fields should already differ
		in the primary key columns already, before DB_TRX_ID,
		DB_ROLL_PTR, and any externally stored columns. */
		ut_ad(!rec_offs_nth_extern(offsets1, cur_field));
		ut_ad(!rec_offs_nth_extern(offsets2, cur_field));

		rec1_b_ptr = rec_get_nth_field(rec1, offsets1,
					       cur_field, &rec1_f_len);
		rec2_b_ptr = rec_get_nth_field(rec2, offsets2,
					       cur_field, &rec2_f_len);

		if (nulls_unequal
		    && rec1_f_len == UNIV_SQL_NULL
		    && rec2_f_len == UNIV_SQL_NULL) {
			ret = -1;
			goto order_resolved;
		}

		ret = cmp_data(mtype, prtype,
			       rec1_b_ptr, rec1_f_len,
			       rec2_b_ptr, rec2_f_len);
		if (ret) {
			goto order_resolved;
		}
	}

	/* If we ran out of fields, rec1 was equal to rec2 up
	to the common fields */
	ut_ad(ret == 0);
order_resolved:

	ut_ad((ret >= - 1) && (ret <= 1));

	*matched_fields = cur_field;
	return(ret);
}

#ifdef UNIV_DEBUG
/*************************************************************//**
Used in debug checking of cmp_dtuple_... .
This function is used to compare a data tuple to a physical record. If
dtuple has n fields then rec must have either m >= n fields, or it must
differ from dtuple in some of the m fields rec has. If encounters an
externally stored field, returns 0.
@return 1, 0, -1, if dtuple is greater, equal, less than rec,
respectively, when only the common first fields are compared */
static
int
cmp_debug_dtuple_rec_with_match(
/*============================*/
	const dtuple_t*	dtuple,	/*!< in: data tuple */
	const rec_t*	rec,	/*!< in: physical record which differs from
				dtuple in some of the common fields, or which
				has an equal number or more fields than
				dtuple */
	const ulint*	offsets,/*!< in: array returned by rec_get_offsets();
				may be NULL for ROW_FORMAT=REDUNDANT */
	ulint		n_cmp,	/*!< in: number of fields to compare */
	ulint*		matched_fields) /*!< in/out: number of already
				completely matched fields; when function
				returns, contains the value for current
				comparison */
{
	const dfield_t*	dtuple_field;	/* current field in logical record */
	ulint		dtuple_f_len;	/* the length of the current field
					in the logical record */
	const byte*	dtuple_f_data;	/* pointer to the current logical
					field data */
	ulint		rec_f_len;	/* length of current field in rec */
	const byte*	rec_f_data;	/* pointer to the current rec field */
	int		ret;		/* return value */
	ulint		cur_field;	/* current field number */

	ut_ad(dtuple && rec && matched_fields);
	ut_ad(dtuple_check_typed(dtuple));
	ut_ad(!offsets || rec_offs_validate(rec, NULL, offsets));

	ut_ad(n_cmp > 0);
	ut_ad(n_cmp <= dtuple_get_n_fields(dtuple));
	ut_ad(*matched_fields <= n_cmp);
	ut_ad(*matched_fields <= (offsets
				  ? rec_offs_n_fields(offsets)
				  : rec_get_n_fields_old(rec)));

	cur_field = *matched_fields;

	if (cur_field == 0) {
		if (UNIV_UNLIKELY
		    (rec_get_info_bits(rec, offsets && rec_offs_comp(offsets))
		     & REC_INFO_MIN_REC_FLAG)) {

			ret = !(dtuple_get_info_bits(dtuple)
				& REC_INFO_MIN_REC_FLAG);

			goto order_resolved;
		}

		if (UNIV_UNLIKELY
		    (dtuple_get_info_bits(dtuple) & REC_INFO_MIN_REC_FLAG)) {
			ret = -1;

			goto order_resolved;
		}
	}

	/* Match fields in a loop; stop if we run out of fields in dtuple */

	while (cur_field < n_cmp) {

		ulint	mtype;
		ulint	prtype;

		dtuple_field = dtuple_get_nth_field(dtuple, cur_field);
		{
			const dtype_t*	type
				= dfield_get_type(dtuple_field);

			mtype = type->mtype;
			prtype = type->prtype;
		}

		dtuple_f_data = static_cast<const byte*>(
			dfield_get_data(dtuple_field));

		dtuple_f_len = dfield_get_len(dtuple_field);

		if (rec_get_nth_field_ext(rec, offsets, cur_field,
					  rec_f_data, rec_f_len)) {
			/* We do not compare to an externally stored field */
			ret = 0;
			goto order_resolved;
		}

		ret = cmp_data_data(mtype, prtype, dtuple_f_data, dtuple_f_len,
				    rec_f_data, rec_f_len);
		if (ret != 0) {
			goto order_resolved;
		}

		cur_field++;
	}

	ret = 0;	/* If we ran out of fields, dtuple was equal to rec
			up to the common fields */
order_resolved:
	ut_ad((ret >= - 1) && (ret <= 1));

	*matched_fields = cur_field;

	return(ret);
}
#endif /* UNIV_DEBUG */
