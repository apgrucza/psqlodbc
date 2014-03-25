/*
 * Tests for the existing behaviors of rollback on errors:
 * 0 -> Do nothing and let the application do it
 * 1 -> Rollback the entire transaction
 * 2 -> Rollback only the statement
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"

HSTMT hstmt = SQL_NULL_HSTMT;

void
error_rollback_init(char *options)
{
	SQLRETURN rc;

	/* Error if initialization is already done */
	if (hstmt != SQL_NULL_HSTMT)
	{
		printf("Initialization already done, leaving...\n");
		exit(1);
	}

	test_connect_ext(options);
	rc = SQLAllocStmt(conn, &hstmt);
	if (!SQL_SUCCEEDED(rc))
	{
		print_diag("failed to allocate stmt handle", SQL_HANDLE_DBC, conn);
		exit(1);
	}

	/* Disable autocommit */
	rc = SQLSetConnectAttr(conn,
						   SQL_ATTR_AUTOCOMMIT,
						   (SQLPOINTER)SQL_AUTOCOMMIT_OFF,
						   SQL_IS_UINTEGER);

	/* Create a table to use */
	rc = SQLExecDirect(hstmt,
			   (SQLCHAR *) "CREATE TEMPORARY TABLE errortab (i int4)",
			   SQL_NTS);
	CHECK_STMT_RESULT(rc, "SQLExecDirect failed", hstmt);

	/* And of course commit... */
	rc = SQLEndTran(SQL_HANDLE_DBC, conn, SQL_COMMIT);
	CHECK_STMT_RESULT(rc, "SQLEndTran failed", hstmt);
}

void
error_rollback_clean(void)
{
	SQLRETURN rc;

	/* Leave if trying to clean an empty handle */
	if (hstmt == SQL_NULL_HSTMT)
	{
		printf("Handle is NULL, leaving...\n");
		exit(1);
	}

	/* Clean up everything */
	rc = SQLFreeStmt(hstmt, SQL_CLOSE);
	CHECK_STMT_RESULT(rc, "SQLFreeStmt failed", hstmt);
	test_disconnect();
	hstmt = SQL_NULL_HSTMT;
}

void
error_rollback_exec_success(void)
{
	SQLRETURN rc;

	/* Leave if executing with an empty handle */
	if (hstmt == SQL_NULL_HSTMT)
	{
		printf("Cannot execute query with NULL handle\n");
		exit(1);
	}

	printf("Executing query that will succeed\n");

	/* Now execute the query */
	rc = SQLExecDirect(hstmt,
					   (SQLCHAR *) "INSERT INTO errortab VALUES (1)",
					   SQL_NTS);

	/* Print error if any, but do not exit */
	CHECK_STMT_RESULT(rc, "SQLExecDirect failed", hstmt);
}

void
error_rollback_exec_failure(void)
{
	SQLRETURN rc;

	/* Leave if executing with an empty handle */
	if (hstmt == SQL_NULL_HSTMT)
	{
		printf("Cannot execute query with NULL handle\n");
		exit(1);
	}

	printf("Executing query that will fail\n");

	/* Now execute the query */
	rc = SQLExecDirect(hstmt,
					   (SQLCHAR *) "INSERT INTO errortab VALUES ('foo')",
					   SQL_NTS);
	if (SQL_SUCCEEDED(rc))
	{
		printf("SQLExecDirect should have failed but it succeeded\n");
		exit(1);
	}

	/* Print error, it is expected */
	print_diag("Failed to execute statement", SQL_HANDLE_DBC, conn);
}

void
error_rollback_print(void)
{
	SQLRETURN rc;

	/* Leave if executing with an empty handle */
	if (hstmt == SQL_NULL_HSTMT)
	{
		printf("Cannot execute query with NULL handle\n");
		exit(1);
	}

	/* Create a table to use */
	rc = SQLExecDirect(hstmt,
			   (SQLCHAR *) "SELECT i FROM errortab",
			   SQL_NTS);
	CHECK_STMT_RESULT(rc, "SQLExecDirect failed", hstmt);

	/* Show results */
	print_result(hstmt);
}

int
main(int argc, char **argv)
{
	SQLRETURN rc;

	/*
	 * Test for protocol at 0.
	 * Do nothing when error occurs and let application do necessary
	 * ROLLBACK on error.
	 */
	printf("Test for rollback protocol 0\n");
	error_rollback_init("Protocol=7.4-0");

	/* Insert a row correctly */
	error_rollback_exec_success();

	/* Now trigger an error, the row previously inserted will disappear */
	error_rollback_exec_failure();

	/*
	 * Now rollback the transaction block, it is the responsability of
	 * application.
	 */
	printf("Rolling back with SQLEndTran\n");
	rc = SQLEndTran(SQL_HANDLE_DBC, conn, SQL_ROLLBACK);
	CHECK_STMT_RESULT(rc, "SQLEndTran failed", hstmt);

	/* Insert row correctly now */
	error_rollback_exec_success();

	/* Not yet committed... */
	rc = SQLEndTran(SQL_HANDLE_DBC, conn, SQL_COMMIT);
	CHECK_STMT_RESULT(rc, "SQLEndTran failed", hstmt);

	/* Print result */
	error_rollback_print();

	/* Clean up */
	error_rollback_clean();

	/*
	 * Test for rollback protocol 1
	 * In case of an error rollback the entire transaction.
	 */
	printf("Test for rollback protocol 1\n");
	error_rollback_init("Protocol=7.4-1");

	/*
	 * Insert a row, trigger an error, and re-insert a row. Only one
	 * row should be visible here.
	 */
	error_rollback_exec_success();
	error_rollback_exec_failure();
	error_rollback_exec_success();
	error_rollback_print();

	/* Clean up */
	error_rollback_clean();

	/*
	 * Test for rollback protocol 2
	 * In the case of an error rollback only the latest statement.
	 */
	printf("Test for rollback protocol 2\n");
	error_rollback_init("Protocol=7.4-2");

	/*
	 * Similarly to previous case, do insert, error and insert. This
	 * time two rows should be visible.
	 */
	error_rollback_exec_success();
	error_rollback_exec_failure();
	error_rollback_exec_success();
	error_rollback_print();

	/* Clean up */
	error_rollback_clean();
}
