/*-------
 * Module:			bind.c
 *
 * Description:		This module contains routines related to binding
 *					columns and parameters.
 *
 * Classes:			BindInfoClass, ParameterInfoClass
 *
 * API functions:	SQLBindParameter, SQLBindCol, SQLDescribeParam, SQLNumParams,
 *					SQLParamOptions
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */

#include <stdlib.h>
#include <string.h>
#include "bind.h"

#include "environ.h"
#include "statement.h"
#include "descriptor.h"
#include "qresult.h"
#include "pgtypes.h"
#include "multibyte.h"

#include "pgapifunc.h"


/*		Bind parameters on a statement handle */
RETCODE		SQL_API
PGAPI_BindParameter(
					HSTMT hstmt,
					SQLUSMALLINT ipar,
					SQLSMALLINT fParamType,
					SQLSMALLINT fCType,
					SQLSMALLINT fSqlType,
					SQLULEN cbColDef,
					SQLSMALLINT ibScale,
					PTR rgbValue,
					SQLLEN cbValueMax,
					SQLLEN FAR * pcbValue)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	CSTR func = "PGAPI_BindParameter";
	APDFields	*apdopts;
	IPDFields	*ipdopts;
	PutDataInfo	*pdata_info;

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	SC_clear_error(stmt);

	apdopts = SC_get_APDF(stmt);
	if (apdopts->allocated < ipar)
		extend_parameter_bindings(apdopts, ipar);
	ipdopts = SC_get_IPDF(stmt);
	if (ipdopts->allocated < ipar)
		extend_iparameter_bindings(ipdopts, ipar);
	pdata_info = SC_get_PDTI(stmt);
	if (pdata_info->allocated < ipar)
		extend_putdata_info(pdata_info, ipar, FALSE);

	/* use zero based column numbers for the below part */
	ipar--;

	/* store the given info */
	apdopts->parameters[ipar].buflen = cbValueMax;
	apdopts->parameters[ipar].buffer = rgbValue;
	apdopts->parameters[ipar].used = pcbValue;
	apdopts->parameters[ipar].CType = fCType;
	ipdopts->parameters[ipar].SQLType = fSqlType;
	ipdopts->parameters[ipar].paramType = fParamType;
	ipdopts->parameters[ipar].column_size = cbColDef;
	ipdopts->parameters[ipar].decimal_digits = ibScale;
	ipdopts->parameters[ipar].precision = 0;
	ipdopts->parameters[ipar].scale = 0;
	if (0 == ipdopts->parameters[ipar].PGType)
		ipdopts->parameters[ipar].PGType = sqltype_to_pgtype(stmt, fSqlType);
#if (ODBCVER >= 0x0300)
	switch (fCType)
	{
		case SQL_C_NUMERIC:
			if (cbColDef > 0)
				ipdopts->parameters[ipar].precision = (UInt2) cbColDef;
			if (ibScale > 0)
				ipdopts->parameters[ipar].scale = ibScale;
			break;
		case SQL_C_TYPE_TIMESTAMP:
			if (ibScale > 0)
				ipdopts->parameters[ipar].precision = ibScale;
			break;
	}
	apdopts->parameters[ipar].precision = ipdopts->parameters[ipar].precision;
	apdopts->parameters[ipar].scale = ipdopts->parameters[ipar].scale;
#endif /* ODBCVER */

	/*
	 * If rebinding a parameter that had data-at-exec stuff in it, then
	 * free that stuff
	 */
	if (pdata_info->pdata[ipar].EXEC_used)
	{
		free(pdata_info->pdata[ipar].EXEC_used);
		pdata_info->pdata[ipar].EXEC_used = NULL;
	}

	if (pdata_info->pdata[ipar].EXEC_buffer)
	{
		free(pdata_info->pdata[ipar].EXEC_buffer);
		pdata_info->pdata[ipar].EXEC_buffer = NULL;
	}

	if (pcbValue && apdopts->param_offset_ptr)
		pcbValue = LENADDR_SHIFT(pcbValue, *apdopts->param_offset_ptr);
#ifdef	NOT_USED /* evaluation of pcbValue here is dangerous */
	/* Data at exec macro only valid for C char/binary data */
	if (pcbValue && (*pcbValue == SQL_DATA_AT_EXEC ||
					 *pcbValue <= SQL_LEN_DATA_AT_EXEC_OFFSET))
		apdopts->parameters[ipar].data_at_exec = TRUE;
	else
		apdopts->parameters[ipar].data_at_exec = FALSE;
#endif /* NOT_USED */

	/* Clear premature result */
	if (stmt->status == STMT_PREMATURE)
		SC_recycle_statement(stmt);

	mylog("%s: ipar=%d, paramType=%d, fCType=%d, fSqlType=%d, cbColDef=%d, ibScale=%d,", func, ipar, fParamType, fCType, fSqlType, cbColDef, ibScale);
	/*** mylog("rgbValue=%x, pcbValue = %x(%d), data_at_exec = %d\n", rgbValue, pcbValue, pcbValue ? *pcbValue : -777, apdopts->parameters[ipar].data_at_exec); ***/

	mylog("rgbValue=%x, pcbValue = %x, data_at_exec = %d\n", rgbValue, pcbValue, apdopts->parameters[ipar].data_at_exec);

	return SQL_SUCCESS;
}


/*	Associate a user-supplied buffer with a database column. */
RETCODE		SQL_API
PGAPI_BindCol(
			  HSTMT hstmt,
			  SQLUSMALLINT icol,
			  SQLSMALLINT fCType,
			  PTR rgbValue,
			  SQLLEN cbValueMax,
			  SQLLEN FAR * pcbValue)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	CSTR func = "PGAPI_BindCol";
	ARDFields	*opts;
	GetDataInfo	*gdata_info;
	BindInfoClass	*bookmark;
	RETCODE		ret = SQL_SUCCESS;

	mylog("%s: entering...\n", func);

	mylog("**** PGAPI_BindCol: stmt = %x, icol = %d\n", stmt, icol);
	mylog("**** : fCType=%d rgb=%x valusMax=%d pcb=%x\n", fCType, rgbValue, cbValueMax, pcbValue);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	opts = SC_get_ARDF(stmt);
	if (stmt->status == STMT_EXECUTING)
	{
		SC_set_error(stmt, STMT_SEQUENCE_ERROR, "Can't bind columns while statement is still executing.", func);
		return SQL_ERROR;
	}

#define	return	DONT_CALL_RETURN_FROM_HERE ???
	SC_clear_error(stmt);
	/* If the bookmark column is being bound, then just save it */
	if (icol == 0)
	{
		bookmark = opts->bookmark;
		if (rgbValue == NULL)
		{
			if (bookmark)
			{
				bookmark->buffer = NULL;
				bookmark->used = NULL;
			}
		}
		else
		{
			/* Make sure it is the bookmark data type */
			switch (fCType)
			{
				case SQL_C_BOOKMARK:
#if (ODBCVER >= 0x0300)
				case SQL_C_VARBOOKMARK:
#endif /* ODBCVER */
					break;
				default:
					SC_set_error(stmt, STMT_PROGRAM_TYPE_OUT_OF_RANGE, "Bind column 0 is not of type SQL_C_BOOKMARK", func);
inolog("Bind column 0 is type %d not of type SQL_C_BOOKMARK", fCType);
					ret = SQL_ERROR;
					goto cleanup;
			}

			bookmark = ARD_AllocBookmark(opts);
			bookmark->buffer = rgbValue;
			bookmark->used = pcbValue;
			bookmark->buflen = cbValueMax;
			bookmark->returntype = fCType;
		}
		goto cleanup;
	}

	/*
	 * Allocate enough bindings if not already done. Most likely,
	 * execution of a statement would have setup the necessary bindings.
	 * But some apps call BindCol before any statement is executed.
	 */
	if (icol > opts->allocated)
		extend_column_bindings(opts, icol);
	gdata_info = SC_get_GDTI(stmt);
	if (icol > gdata_info->allocated)
		extend_getdata_info(gdata_info, icol, FALSE);

	/* check to see if the bindings were allocated */
	if (!opts->bindings)
	{
		SC_set_error(stmt, STMT_NO_MEMORY_ERROR, "Could not allocate memory for bindings.", func);
		ret = SQL_ERROR;
		goto cleanup;
	}

	/* use zero based col numbers from here out */
	icol--;

	/* Reset for SQLGetData */
	gdata_info->gdata[icol].data_left = -1;

	if (rgbValue == NULL)
	{
		/* we have to unbind the column */
		opts->bindings[icol].buflen = 0;
		opts->bindings[icol].buffer = NULL;
		opts->bindings[icol].used = NULL;
		opts->bindings[icol].returntype = SQL_C_CHAR;
		opts->bindings[icol].precision = 0;
		opts->bindings[icol].scale = 0;
		if (gdata_info->gdata[icol].ttlbuf)
			free(gdata_info->gdata[icol].ttlbuf);
		gdata_info->gdata[icol].ttlbuf = NULL;
		gdata_info->gdata[icol].ttlbuflen = 0;
		gdata_info->gdata[icol].ttlbufused = 0;
	}
	else
	{
		/* ok, bind that column */
		opts->bindings[icol].buflen = cbValueMax;
		opts->bindings[icol].buffer = rgbValue;
		opts->bindings[icol].used = pcbValue;
		opts->bindings[icol].returntype = fCType;
#if (ODBCVER >= 0x0300)
		if (SQL_C_NUMERIC == fCType)
			opts->bindings[icol].precision = 32;
		else
#endif /* ODBCVER */
			opts->bindings[icol].precision = 0;
		opts->bindings[icol].scale = 0;

		mylog("       bound buffer[%d] = %x\n", icol, opts->bindings[icol].buffer);
	}

cleanup:
#undef	return
	if (stmt->internal)
		ret = DiscardStatementSvp(stmt, ret, FALSE);
	return ret;
}


/*
 *	Returns the description of a parameter marker.
 *	This function is listed as not being supported by SQLGetFunctions() because it is
 *	used to describe "parameter markers" (not bound parameters), in which case,
 *	the dbms should return info on the markers.  Since Postgres doesn't support that,
 *	it is best to say this function is not supported and let the application assume a
 *	data type (most likely varchar).
 */
RETCODE		SQL_API
PGAPI_DescribeParam(
					HSTMT hstmt,
					SQLUSMALLINT ipar,
					SQLSMALLINT FAR * pfSqlType,
					SQLULEN FAR * pcbParamDef,
					SQLSMALLINT FAR * pibScale,
					SQLSMALLINT FAR * pfNullable)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	CSTR func = "PGAPI_DescribeParam";
	APDFields	*apdopts;
	IPDFields	*ipdopts;
	RETCODE		ret = SQL_SUCCESS;
	int		num_params;

	mylog("%s: entering...%d\n", func, ipar);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	SC_clear_error(stmt);

	apdopts = SC_get_APDF(stmt);
	ipdopts = SC_get_IPDF(stmt);
	/*if ((ipar < 1) || (ipar > ipdopts->allocated))*/
	num_params = stmt->num_params;
	if (num_params < 0)
	{
		SQLSMALLINT	num_p;

		PGAPI_NumParams(stmt, &num_p);
		num_params = num_p;
	}
	if ((ipar < 1) || (ipar > num_params))
	{
inolog("num_params=%d\n", stmt->num_params);
		SC_set_error(stmt, STMT_BAD_PARAMETER_NUMBER_ERROR, "Invalid parameter number for PGAPI_DescribeParam.", func);
		return SQL_ERROR;
	}
	extend_iparameter_bindings(ipdopts, stmt->num_params);

#define	return	DONT_CALL_RETURN_FROM_HERE???
	/* StartRollbackState(stmt); */
	if (NOT_YET_PREPARED == stmt->prepared)
	{
		decideHowToPrepare(stmt);
inolog("howTo=%d\n", SC_get_prepare_method(stmt));
		switch (SC_get_prepare_method(stmt))
		{
			case USING_PARSE_REQUEST:
			case USING_UNNAMED_PARSE_REQUEST:
				if (ret = prepareParameters(stmt), SQL_ERROR == ret)
					goto cleanup;
		}
	}

	ipar--;
	/*
	 * This implementation is not very good, since it is supposed to
	 * describe
	 */
	/* parameter markers, not bound parameters.  */
	if (pfSqlType)
	{
inolog("[%d].SQLType=%d .PGType=%d\n", ipar, ipdopts->parameters[ipar].SQLType,
ipdopts->parameters[ipar].PGType);
		if (ipdopts->parameters[ipar].SQLType)
			*pfSqlType = ipdopts->parameters[ipar].SQLType;
		else if (ipdopts->parameters[ipar].PGType)
			*pfSqlType = pgtype_to_concise_type(stmt, ipdopts->parameters[ipar].PGType, PG_STATIC);
	}

	if (pcbParamDef)
	{
		*pcbParamDef = 0;
		if (ipdopts->parameters[ipar].SQLType)
			*pcbParamDef = ipdopts->parameters[ipar].column_size;
		if (0 == *pcbParamDef && ipdopts->parameters[ipar].PGType)
			*pcbParamDef = pgtype_column_size(stmt, ipdopts->parameters[ipar].PGType, PG_STATIC, PG_STATIC);
	}

	if (pibScale)
	{
		*pibScale = 0;
		if (ipdopts->parameters[ipar].SQLType)
			*pibScale = ipdopts->parameters[ipar].decimal_digits;
		else if (ipdopts->parameters[ipar].PGType)
			*pibScale = pgtype_scale(stmt, ipdopts->parameters[ipar].PGType, -1);
	}

	if (pfNullable)
		*pfNullable = pgtype_nullable(stmt, ipdopts->parameters[ipar].paramType);
cleanup:
#undef	return
	if (stmt->internal)
		ret = DiscardStatementSvp(stmt, ret, FALSE);
	return ret;
}


/*	Sets multiple values (arrays) for the set of parameter markers. */
RETCODE		SQL_API
PGAPI_ParamOptions(
				   HSTMT hstmt,
				   SQLULEN crow,
				   SQLULEN FAR * pirow)
{
	CSTR func = "PGAPI_ParamOptions";
	StatementClass *stmt = (StatementClass *) hstmt;
	APDFields	*apdopts;

	mylog("%s: entering... %d %x\n", func, crow, pirow);

	apdopts = SC_get_APDF(stmt);
	apdopts->paramset_size = crow;
	SC_get_IPDF(stmt)->param_processed_ptr = (UInt4 *) pirow;
	return SQL_SUCCESS;
}


/*
 *	This function should really talk to the dbms to determine the number of
 *	"parameter markers" (not bound parameters) in the statement.  But, since
 *	Postgres doesn't support that, the driver should just count the number of markers
 *	and return that.  The reason the driver just can't say this function is unsupported
 *	like it does for SQLDescribeParam is that some applications don't care and try
 *	to call it anyway.
 *	If the statement does not have parameters, it should just return 0.
 */
RETCODE		SQL_API
PGAPI_NumParams(
				HSTMT hstmt,
				SQLSMALLINT FAR * pcpar)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	CSTR func = "PGAPI_NumParams";
	char	literal_quote = LITERAL_QUOTE, identifier_quote = IDENTIFIER_QUOTE, dollar_quote = DOLLAR_QUOTE;

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if (pcpar)
		*pcpar = 0;
	else
	{
		SC_set_error(stmt, STMT_EXEC_ERROR, "parameter count address is null", func);
		return SQL_ERROR;
	}
inolog("num_params=%d,%d\n", stmt->num_params, stmt->proc_return);
	if (stmt->num_params >= 0)
		*pcpar = stmt->num_params;
	else if (!stmt->statement)
	{
		/* no statement has been allocated */
		SC_set_error(stmt, STMT_SEQUENCE_ERROR, "PGAPI_NumParams called with no statement ready.", func);
		return SQL_ERROR;
	}
	else
	{
		const	char *sptr, *tag = NULL;
		ConnectionClass	*conn = SC_get_conn(stmt);
		int	taglen;
		char	tchar, bchar, escape_in_literal;
		char	in_literal = FALSE, in_identifier = FALSE,
			in_dollar_quote = FALSE, in_escape = FALSE,
			multi = FALSE, del_found = FALSE;
		encoded_str	encstr;

		stmt->proc_return = 0;
		make_encoded_str(&encstr, conn, stmt->statement);
		for (sptr = stmt->statement, bchar = '\0'; *sptr; sptr++)
		{
			tchar = encoded_nextchar(&encstr);
			if (ENCODE_STATUS(encstr) != 0) /* multibyte char */
			{
				if ((UCHAR) tchar >= 0x80)
					bchar = tchar;
				continue;
			}
			if (!multi && del_found)
			{
				if (!isspace(tchar))
					multi = TRUE;
			}
			if (in_dollar_quote)
			{
				if (tchar == dollar_quote)
				{
					if (strncmp(sptr, tag, taglen) == 0)
					{
						in_dollar_quote = FALSE;
						tag = NULL;
						sptr += taglen;
						sptr--;
						encoded_position_shift(&encstr, taglen - 1);
					}
				}
			}
			else if (in_literal)
			{
				if (in_escape)
					in_escape = FALSE;
				else if (tchar == escape_in_literal)
					in_escape = TRUE;
				else if (tchar == literal_quote)
					in_literal = FALSE;
			}
			else if (in_identifier)
			{
				if (tchar == identifier_quote)
					in_identifier = FALSE;
			}
			else
			{
				if (tchar == '?')
				{
					if (0 == *pcpar && bchar == '{')
						stmt->proc_return = 1;
					(*pcpar)++;
				}
				else if (tchar == ';')
					del_found = TRUE;
				else if (tchar == dollar_quote)
				{
					char	*dollar_next;

					in_dollar_quote = TRUE;
					tag = sptr;
					taglen = 0; 
					if (dollar_next = strchr(sptr + 1, dollar_quote))
					{
						taglen = dollar_next - sptr + 1;
						sptr = dollar_next;
						encoded_position_shift(&encstr, taglen - 1);
					}
				}
				else if (tchar == literal_quote)
				{
					in_literal = TRUE;
					escape_in_literal = CC_get_escape(conn);
					if (!escape_in_literal)
					{
						if (LITERAL_EXT == sptr[-1])
							escape_in_literal = ESCAPE_IN_LITERAL;
					}
				}
				else if (tchar == identifier_quote)
					in_identifier = TRUE;
				if (!isspace(tchar))
					bchar = tchar;
			}
		}
		stmt->num_params = *pcpar;
		stmt->multi_statement = multi;
	}
inolog("num_params=%d,%d\n", stmt->num_params, stmt->proc_return);
	return SQL_SUCCESS;
}


/*
 *	 Bindings Implementation
 */
static BindInfoClass *
create_empty_bindings(int num_columns)
{
	BindInfoClass *new_bindings;
	int			i;

	new_bindings = (BindInfoClass *) malloc(num_columns * sizeof(BindInfoClass));
	if (!new_bindings)
		return 0;

	for (i = 0; i < num_columns; i++)
	{
		new_bindings[i].buflen = 0;
		new_bindings[i].buffer = NULL;
		new_bindings[i].used = NULL;
	}

	return new_bindings;
}

void
extend_parameter_bindings(APDFields *self, int num_params)
{
	CSTR func = "extend_parameter_bindings";
	ParameterInfoClass *new_bindings;

	mylog("%s: entering ... self=%x, parameters_allocated=%d, num_params=%d\n", func, self, self->allocated, num_params);

	/*
	 * if we have too few, allocate room for more, and copy the old
	 * entries into the new structure
	 */
	if (self->allocated < num_params)
	{
		new_bindings = (ParameterInfoClass *) realloc(self->parameters, sizeof(ParameterInfoClass) * num_params);
		if (!new_bindings)
		{
			mylog("%s: unable to create %d new bindings from %d old bindings\n", func, num_params, self->allocated);

			self->parameters = NULL;
			self->allocated = 0;
			return;
		}
		memset(&new_bindings[self->allocated], 0,
			sizeof(ParameterInfoClass) * (num_params - self->allocated));

		self->parameters = new_bindings;
		self->allocated = num_params;
	}

	mylog("exit extend_parameter_bindings\n");
}

void
extend_iparameter_bindings(IPDFields *self, int num_params)
{
	CSTR func = "extend_iparameter_bindings";
	ParameterImplClass *new_bindings;

	mylog("%s: entering ... self=%x, parameters_allocated=%d, num_params=%d\n", func, self, self->allocated, num_params);

	/*
	 * if we have too few, allocate room for more, and copy the old
	 * entries into the new structure
	 */
	if (self->allocated < num_params)
	{
		new_bindings = (ParameterImplClass *) realloc(self->parameters, sizeof(ParameterImplClass) * num_params);
		if (!new_bindings)
		{
			mylog("%s: unable to create %d new bindings from %d old bindings\n", func, num_params, self->allocated);

			self->parameters = NULL;
			self->allocated = 0;
			return;
		}
		memset(&new_bindings[self->allocated], 0,
			sizeof(ParameterImplClass) * (num_params - self->allocated));

		self->parameters = new_bindings;
		self->allocated = num_params;
	}

	mylog("exit extend_iparameter_bindings\n");
}

void
reset_a_parameter_binding(APDFields *self, int ipar)
{
	CSTR func = "reset_a_parameter_binding";

	mylog("%s: entering ... self=%x, parameters_allocated=%d, ipar=%d\n", func, self, self->allocated, ipar);

	if (ipar < 1 || ipar > self->allocated)
		return;

	ipar--;
	self->parameters[ipar].buflen = 0;
	self->parameters[ipar].buffer = 0;
	self->parameters[ipar].used = 0;
	self->parameters[ipar].CType = 0;
	self->parameters[ipar].data_at_exec = FALSE;
	self->parameters[ipar].precision = 0;
	self->parameters[ipar].scale = 0;
}

void
reset_a_iparameter_binding(IPDFields *self, int ipar)
{
	CSTR func = "reset_a_iparameter_binding";

	mylog("%s: entering ... self=%x, parameters_allocated=%d, ipar=%d\n", func, self, self->allocated, ipar);

	if (ipar < 1 || ipar > self->allocated)
		return;

	ipar--;
	NULL_THE_NAME(self->parameters[ipar].paramName);
	self->parameters[ipar].paramType = 0;
	self->parameters[ipar].SQLType = 0;
	self->parameters[ipar].column_size = 0;
	self->parameters[ipar].decimal_digits = 0;
	self->parameters[ipar].precision = 0;
	self->parameters[ipar].scale = 0;
	self->parameters[ipar].PGType = 0;
}

int
CountParameters(const StatementClass *self, Int2 *inputCount, Int2 *ioCount, Int2 *outputCount)
{
	CSTR func = "CountParameters";
	IPDFields	*ipdopts = SC_get_IPDF(self);
	int	i, num_params, valid_count;

	if (inputCount)
		*inputCount = 0;
	if (ioCount)
		*ioCount = 0;
	if (outputCount)
		*outputCount = 0;
	if (!ipdopts)	return -1;
	num_params = self->num_params;
	if (ipdopts->allocated < num_params)
		num_params = ipdopts->allocated;
	for (i = 0, valid_count = 0; i < num_params; i++)
	{
		if (SQL_PARAM_OUTPUT == ipdopts->parameters[i].paramType)
		{
			if (outputCount)
			{
				(*outputCount)++;
				valid_count++;
			}
		}
		else if (SQL_PARAM_INPUT_OUTPUT == ipdopts->parameters[i].paramType)
		{
			if (ioCount)
			{
				(*ioCount)++;
				valid_count++;
			}
		}
		else if (inputCount)
		{
			 (*inputCount)++;
			valid_count++;
		}
	}
	return valid_count;
}

/*
 *	Free parameters and free the memory.
 */
void
APD_free_params(APDFields *apdopts, char option)
{
	mylog("APD_free_params:  ENTER, self=%d\n", apdopts);

	if (!apdopts->parameters)
		return;

	if (option == STMT_FREE_PARAMS_ALL)
	{
		free(apdopts->parameters);
		apdopts->parameters = NULL;
		apdopts->allocated = 0;
	}

	mylog("APD_free_params:  EXIT\n");
}

void
PDATA_free_params(PutDataInfo *pdata, char option)
{
	int			i;

	mylog("PDATA_free_params:  ENTER, self=%x\n", pdata);

	if (!pdata->pdata)
		return;

	for (i = 0; i < pdata->allocated; i++)
	{
		if (pdata->pdata[i].EXEC_used)
		{
			free(pdata->pdata[i].EXEC_used);
			pdata->pdata[i].EXEC_used = NULL;
		}
		if (pdata->pdata[i].EXEC_buffer)
		{
			free(pdata->pdata[i].EXEC_buffer);
			pdata->pdata[i].EXEC_buffer = NULL;
		}
	}

	if (option == STMT_FREE_PARAMS_ALL)
	{
		free(pdata->pdata);
		pdata->pdata = NULL;
		pdata->allocated = 0;
	}

	mylog("PDATA_free_params:  EXIT\n");
}

/*
 *	Free parameters and free the memory.
 */
void
IPD_free_params(IPDFields *ipdopts, char option)
{
	mylog("IPD_free_params:  ENTER, self=%x\n", ipdopts);

	if (!ipdopts->parameters)
		return;
	if (option == STMT_FREE_PARAMS_ALL)
	{
		free(ipdopts->parameters);
		ipdopts->parameters = NULL;
		ipdopts->allocated = 0;
	}

	mylog("IPD_free_params:  EXIT\n");
}

void
extend_column_bindings(ARDFields *self, int num_columns)
{
	CSTR func = "extend_column_bindings";
	BindInfoClass *new_bindings;
	int			i;

	mylog("%s: entering ... self=%x, bindings_allocated=%d, num_columns=%d\n", func, self, self->allocated, num_columns);

	/*
	 * if we have too few, allocate room for more, and copy the old
	 * entries into the new structure
	 */
	if (self->allocated < num_columns)
	{
		new_bindings = create_empty_bindings(num_columns);
		if (!new_bindings)
		{
			mylog("%s: unable to create %d new bindings from %d old bindings\n", func, num_columns, self->allocated);

			if (self->bindings)
			{
				free(self->bindings);
				self->bindings = NULL;
			}
			self->allocated = 0;
			return;
		}

		if (self->bindings)
		{
			for (i = 0; i < self->allocated; i++)
				new_bindings[i] = self->bindings[i];

			free(self->bindings);
		}

		self->bindings = new_bindings;
		self->allocated = num_columns;
	}

	/*
	 * There is no reason to zero out extra bindings if there are more
	 * than needed.  If an app has allocated extra bindings, let it worry
	 * about it by unbinding those columns.
	 */

	/* SQLBindCol(1..) ... SQLBindCol(10...)   # got 10 bindings */
	/* SQLExecDirect(...)  # returns 5 cols */
	/* SQLExecDirect(...)  # returns 10 cols  (now OK) */

	mylog("exit extend_column_bindings\n");
}

void
reset_a_column_binding(ARDFields *self, int icol)
{
	CSTR func = "reset_a_column_binding";
	BindInfoClass	*bookmark;

	mylog("%s: entering ... self=%x, bindings_allocated=%d, icol=%d\n", func, self, self->allocated, icol);

	if (icol > self->allocated)
		return;

	/* use zero based col numbers from here out */
	if (0 == icol)
	{
		if (bookmark = self->bookmark, bookmark != NULL)
		{
			bookmark->buffer = NULL;
			bookmark->used = NULL;
		}
	}
	else
	{
		icol--;

		/* we have to unbind the column */
		self->bindings[icol].buflen = 0;
		self->bindings[icol].buffer = NULL;
		self->bindings[icol].used = NULL;
		self->bindings[icol].returntype = SQL_C_CHAR;
	}
}

void	ARD_unbind_cols(ARDFields *self, BOOL freeall)
{
	Int2	lf;

inolog("ARD_unbind_cols freeall=%d allocated=%d bindings=%x", freeall, self->allocated, self->bindings);
	for (lf = 1; lf <= self->allocated; lf++)
		reset_a_column_binding(self, lf);
	if (freeall)
	{
		if (self->bindings)
			free(self->bindings);
		self->bindings = NULL;
		self->allocated = 0;
	}
}
void	GDATA_unbind_cols(GetDataInfo *self, BOOL freeall)
{
	Int2	lf;

inolog("GDATA_unbind_cols freeall=%d allocated=%d gdata=%x", freeall, self->allocated, self->gdata);
	if (self->fdata.ttlbuf)
	{
		free(self->fdata.ttlbuf);
		self->fdata.ttlbuf = NULL;
	}
	self->fdata.ttlbuflen = self->fdata.ttlbufused = 0;
	self->fdata.data_left = -1;
	for (lf = 1; lf <= self->allocated; lf++)
		reset_a_getdata_info(self, lf);
	if (freeall)
	{
		if (self->gdata)
			free(self->gdata);
		self->gdata = NULL;
		self->allocated = 0;
	}
}

void GetDataInfoInitialize(GetDataInfo *gdata_info)
{
	gdata_info->fdata.data_left = -1;
	gdata_info->fdata.ttlbuf = NULL;
	gdata_info->fdata.ttlbuflen = gdata_info->fdata.ttlbufused = 0;
	gdata_info->allocated = 0;
	gdata_info->gdata = NULL;
}
static GetDataClass *
create_empty_gdata(int num_columns)
{
	GetDataClass	*new_gdata;
	int			i;

	new_gdata = (GetDataClass *) malloc(num_columns * sizeof(GetDataClass));
	if (!new_gdata)
		return NULL;

	for (i = 0; i < num_columns; i++)
	{
		new_gdata[i].data_left = -1;
		new_gdata[i].ttlbuf = NULL;
		new_gdata[i].ttlbuflen = 0;
		new_gdata[i].ttlbufused = 0;
	}

	return new_gdata;
}
void
extend_getdata_info(GetDataInfo *self, int num_columns, BOOL shrink)
{
	CSTR func = "extend_getdata_info";
	GetDataClass	*new_gdata;
	int			i;

	mylog("%s: entering ... self=%x, gdata_allocated=%d, num_columns=%d\n", func, self, self->allocated, num_columns);

	/*
	 * if we have too few, allocate room for more, and copy the old
	 * entries into the new structure
	 */
	if (self->allocated < num_columns)
	{
		new_gdata = create_empty_gdata(num_columns);
		if (!new_gdata)
		{
			mylog("%s: unable to create %d new gdata from %d old gdata\n", func, num_columns, self->allocated);

			if (self->gdata)
			{
				free(self->gdata);
				self->gdata = NULL;
			}
			self->allocated = 0;
			return;
		}
		if (self->gdata)
		{
			for (i = 0; i < self->allocated; i++)
				new_gdata[i] = self->gdata[i];
			free(self->gdata);
		}
		self->gdata = new_gdata;
		self->allocated = num_columns;
	}
	else if (shrink && self->allocated > num_columns)
	{
		for (i = self->allocated; i > num_columns; i--)
			reset_a_getdata_info(self, i);
		self->allocated = num_columns;
		if (0 == num_columns)
		{
			free(self->gdata);
			self->gdata = NULL;
		}
	} 

	/*
	 * There is no reason to zero out extra gdata if there are more
	 * than needed.  If an app has allocated extra gdata, let it worry
	 * about it by unbinding those columns.
	 */

	mylog("exit extend_gdata_info\n");
}
void	reset_a_getdata_info(GetDataInfo *gdata_info, int icol)
{
	if (icol < 1 || icol > gdata_info->allocated)
		return;
	icol--;
	if (gdata_info->gdata[icol].ttlbuf)
	{
		free(gdata_info->gdata[icol].ttlbuf);
		gdata_info->gdata[icol].ttlbuf = NULL;
	}
	gdata_info->gdata[icol].ttlbuflen =
	gdata_info->gdata[icol].ttlbufused = 0;
	gdata_info->gdata[icol].data_left = -1;
}

void PutDataInfoInitialize(PutDataInfo *pdata_info)
{
	pdata_info->allocated = 0;
	pdata_info->pdata = NULL;
}
void
extend_putdata_info(PutDataInfo *self, int num_params, BOOL shrink)
{
	CSTR func = "extend_putdata_info";
	PutDataClass	*new_pdata;

	mylog("%s: entering ... self=%x, parameters_allocated=%d, num_params=%d\n", func, self, self->allocated, num_params);

	/*
	 * if we have too few, allocate room for more, and copy the old
	 * entries into the new structure
	 */
	if (self->allocated < num_params)
	{
		if (self->allocated <= 0 && self->pdata)
		{
			mylog("??? pdata is not null while allocated == 0\n");
			self->pdata = NULL; 
		}
		new_pdata = (PutDataClass *) realloc(self->pdata, sizeof(PutDataClass) * num_params);
		if (!new_pdata)
		{
			mylog("%s: unable to create %d new pdata from %d old pdata\n", func, num_params, self->allocated);

			self->pdata = NULL;
			self->allocated = 0;
			return;
		}
		memset(&new_pdata[self->allocated], 0,
			sizeof(PutDataClass) * (num_params - self->allocated));

		self->pdata = new_pdata;
		self->allocated = num_params;
	}
	else if (shrink && self->allocated > num_params)
	{
		int	i;

		for (i = self->allocated; i > num_params; i--)
			reset_a_putdata_info(self, i);
		self->allocated = num_params;
		if (0 == num_params)
		{
			free(self->pdata);
			self->pdata = NULL;
		}
	}

	mylog("exit extend_putdata_info\n");
}
void	reset_a_putdata_info(PutDataInfo *pdata_info, int ipar)
{
	if (ipar < 1 || ipar > pdata_info->allocated)
		return;
	ipar--;
	if (pdata_info->pdata[ipar].EXEC_used)
	{
		free(pdata_info->pdata[ipar].EXEC_used);
		pdata_info->pdata[ipar].EXEC_used = NULL;
	}
	if (pdata_info->pdata[ipar].EXEC_buffer)
	{
		free(pdata_info->pdata[ipar].EXEC_buffer);
		pdata_info->pdata[ipar].EXEC_buffer = NULL;
	}
	pdata_info->pdata[ipar].lobj_oid = 0;
}

void SC_param_next(const StatementClass *stmt, int *param_number, ParameterInfoClass **apara, ParameterImplClass **ipara)
{
	int	next;
	IPDFields	*ipdopts = SC_get_IPDF(stmt);

	if (*param_number < 0)
		next = stmt->proc_return;
	else
		next = *param_number + 1;
	if (stmt->discard_output_params)
	{
		for (;next < ipdopts->allocated && SQL_PARAM_OUTPUT == ipdopts->parameters[next].paramType; next++) ;
	}
	*param_number = next;
	if (ipara)
	{
		if (next < ipdopts->allocated)
			*ipara = ipdopts->parameters + next;
		else
			*ipara = NULL;
	}
	if (apara)
	{
		APDFields	*apdopts = SC_get_APDF(stmt);
		if (next < apdopts->allocated)
			*apara = apdopts->parameters + next;
		else
			*apara = NULL;
	}
}
