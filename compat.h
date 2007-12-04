
#ifndef __compat_h
#define __compat_h

#include <stdlib.h>

#if RUBY_VERSION_CODE < 180
#define rb_check_string_type(x) rb_check_convert_type(x, T_STRING, "String", "to_str")
#endif /* RUBY_VERSION_CODE < 180 */

#ifndef StringValuePtr
#define StringValuePtr(x) STR2CSTR(x)
#endif /* StringValuePtr */

#ifndef HAVE_PG_ENCODING_TO_CHAR
#define pg_encoding_to_char(x) "SQL_ASCII"
#endif /* HAVE_PG_ENCODING_TO_CHAR */

#ifndef HAVE_PQFREEMEM
#define PQfreemem(ptr) free(ptr)
#endif /* HAVE_PQFREEMEM */

#ifndef HAVE_PQSETCLIENTENCODING
int PQsetClientEncoding(PGconn *conn, const char *encoding)
#endif /* HAVE_PQSETCLIENTENCODING */

#ifndef HAVE_PQESCAPESTRING
size_t PQescapeString(char *to, const char *from, size_t length);
unsigned char * PQescapeBytea(const unsigned char *bintext, size_t binlen, size_t *bytealen);
unsigned char * PQunescapeBytea(const unsigned char *strtext, size_t *retbuflen);
#endif /* HAVE_PQESCAPESTRING */

#ifndef HAVE_PQESCAPESTRINGCONN
size_t PQescapeStringConn(PGconn *conn, char *to, const char *from, 
	size_t length, int *error);
unsigned char *PQescapeByteaConn(PGconn *conn, const unsigned char *from, 
	size_t from_length, size_t *to_length);
#endif /* HAVE_PQESCAPESTRINGCONN */

#ifndef HAVE_PQPREPARE
PGresult * PQprepare(PGconn *conn, const char *stmtName, const char *query,
	int nParams, const Oid *paramTypes);
#endif /* HAVE_PQPREPARE */

#ifndef HAVE_PQEXECPARAMS
PGresult *PQexecParams(PGconn *conn, const char *command, int nParams, 
	const Oid *paramTypes, const char * const * paramValues, const int *paramLengths, 
	const int *paramFormats, int resultFormat);
PGresult *PQexecParams_compat(PGconn *conn, VALUE command, VALUE values);
#endif /* HAVE_PQEXECPARAMS */

#endif /* __compat_h */
