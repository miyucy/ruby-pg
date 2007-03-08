/************************************************

  pg.c -

  Author: matz 
  created at: Tue May 13 20:07:35 JST 1997

  Author: ematsu
  modified at: Wed Jan 20 16:41:51 1999

  $Author$
  $Date$
************************************************/

#include "ruby.h"
#include "rubyio.h"
#include "st.h"

/* grep '^#define' $(pg_config --includedir)/server/catalog/pg_type.h | grep OID */
#include "type-oids.h"
#include <libpq-fe.h>
#include <libpq/libpq-fs.h>              /* large-object interface */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#ifndef HAVE_PG_ENCODING_TO_CHAR
#define pg_encoding_to_char(x) "SQL_ASCII"
#else
extern char* pg_encoding_to_char(int);
#endif

#ifndef HAVE_PQFREEMEM
#define PQfreemem(ptr) free(ptr)
#endif

#ifndef StringValuePtr
#define StringValuePtr(x) STR2CSTR(x)
#endif

#define AssignCheckedStringValue(cstring, rstring) do { \
    if (!NIL_P(temp = rstring)) { \
        Check_Type(temp, T_STRING); \
        cstring = StringValuePtr(temp); \
    } \
} while (0)

#if RUBY_VERSION_CODE < 180
#define rb_check_string_type(x) rb_check_convert_type(x, T_STRING, "String", "to_str")
#endif

#define rb_check_hash_type(x) rb_check_convert_type(x, T_HASH, "Hash", "to_hash")

#define rb_define_singleton_alias(klass,new,old) rb_define_alias(rb_singleton_class(klass),new,old)

#define Data_Set_Struct(obj,ptr) do { \
    Check_Type(obj, T_DATA); \
    DATA_PTR(obj) = ptr; \
} while (0)

#define RUBY_CLASS(name) rb_const_get(rb_cObject, rb_intern(name))

#define SINGLE_QUOTE '\''

EXTERN VALUE rb_mEnumerable;
EXTERN VALUE rb_mKernel;
EXTERN VALUE rb_cTime;

static VALUE rb_cDate;
static VALUE rb_cDateTime;
static VALUE rb_cBigDecimal;

static VALUE rb_cPGconn;
static VALUE rb_cPGresult;
static VALUE rb_ePGError;
static VALUE rb_cPGlarge; 
static VALUE rb_cPGrow;
static VALUE rb_cPGrowtype;
static VALUE rb_cPGfield;
static VALUE rb_cPGrecord;
static VALUE rb_cPGbytea;

static VALUE pgconn_lastval _((VALUE));
static VALUE pgresult_fields _((VALUE));
static VALUE pgresult_clear _((VALUE));
static VALUE pgresult_result_with_clear _((VALUE));
static VALUE pgresult_new _((PGresult*));

static int translate_results = 1;
static int parse_arrays = 1;

/* Large Object support */
typedef struct pglarge_object
{
    PGconn *pgconn;
    Oid lo_oid;
    int lo_fd;
} PGlarge;

static VALUE pglarge_new _((PGconn*, Oid, int));
/* Large Object support */

static void free_pgconn(PGconn *);

static VALUE
pgconn_alloc(klass)
    VALUE klass;
{
    return Data_Wrap_Struct(klass, 0, free_pgconn, NULL);
}

static int build_key_value_string_i(VALUE key, VALUE value, VALUE result);
static PGconn *get_pgconn(VALUE obj);
static VALUE pgconn_close(VALUE self);

static PGconn *
try_connectdb(arg)
    VALUE arg;
{
    VALUE conninfo;

    if (!NIL_P(conninfo = rb_check_string_type(arg))) {
        /* do nothing */
    }
    else if (!NIL_P(conninfo = rb_check_hash_type(arg))) {
        VALUE key_values = rb_ary_new2(RHASH(conninfo)->tbl->num_entries);
        rb_hash_foreach(conninfo, build_key_value_string_i, key_values);
        conninfo = rb_ary_join(key_values, rb_str_new2(" "));
    }
    else {
        return NULL;
    }

    return PQconnectdb(StringValuePtr(conninfo));
}

static PGconn *
try_setdbLogin(args)
    VALUE args;
{
    VALUE temp;
    char *host, *port, *opt, *tty, *dbname, *login, *pwd;
    host=port=opt=tty=dbname=login=pwd=NULL;

    rb_funcall(args, rb_intern("flatten!"), 0);

    AssignCheckedStringValue(host, rb_ary_entry(args, 0));
    if (!NIL_P(temp = rb_ary_entry(args, 1)) && NUM2INT(temp) != -1) {
        temp = rb_obj_as_string(temp);
        port = StringValuePtr(temp);
    }
    AssignCheckedStringValue(opt, rb_ary_entry(args, 2));
    AssignCheckedStringValue(tty, rb_ary_entry(args, 3));
    AssignCheckedStringValue(dbname, rb_ary_entry(args, 4));
    AssignCheckedStringValue(login, rb_ary_entry(args, 5));
    AssignCheckedStringValue(pwd, rb_ary_entry(args, 6));

    return PQsetdbLogin(host, port, opt, tty, dbname, login, pwd);
}

static VALUE
pgconn_connect(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    VALUE args;
    PGconn *conn = NULL;

    rb_scan_args(argc, argv, "0*", &args); 
    if (RARRAY(args)->len == 1) { 
        conn = try_connectdb(rb_ary_entry(args, 0));
    }
    if (conn == NULL) {
        conn = try_setdbLogin(args);
    }

    if (PQstatus(conn) == CONNECTION_BAD) {
        VALUE message = rb_str_new2(PQerrorMessage(conn));
        PQfinish(conn);
        rb_raise(rb_ePGError, StringValuePtr(message));
    }

    if (PQserverVersion(conn) >= 80100) {
        rb_define_singleton_method(self, "lastval", pgconn_lastval, 0);
    }
    /* setup several instance vars for result translation/formatting */
    rb_iv_set(self, "@array_types", rb_hash_new());
    rb_iv_set(self, "@translators", rb_hash_new());

    Data_Set_Struct(self, conn);
    return self;
}

/*
 * call-seq:
 *   PGconn.translate_results = boolean
 *
 * When true (default), results are translated to appropriate ruby class.
 * When false, results are returned as +Strings+.
 * 
 * There is number of built-in conversion rules, and custom translation
 * can set, see #reg_translate, #reg_array, #parse_arrays=, #load_composite_types.
 * 
 * Custom translation is instance-specific.
 */
static VALUE
pgconn_s_translate_results_set(self, fact)
    VALUE self, fact;
{
    translate_results = (fact == Qfalse || fact == Qnil) ? 0 : 1;
    return fact;
}

/*
 * call-seq:
 *   PGconn.parse_arrays = boolean
 *
 * When translate_results and parse_arrays are true (default), 
 * results are translated to appropriate ruby class.
 * Arrays of types int[], float[], numeric[], text[] are returned as ruby arrays
 * with values of appropriate ruby class.
 *
 * It is possible to define array parsers for other types, using PGconn#reg_array instance method.
 *
 * When parse_arrays is false, results are returned as +Strings+.
 *
 */
static VALUE
pgconn_s_parse_arrays_set(self, fact)
    VALUE self, fact;
{
    parse_arrays = (fact == Qfalse || fact == Qnil) ? 0 : 1;
    return Qnil;
}

static VALUE format_array_element(VALUE self, VALUE obj);
static VALUE pgconn_s_quote(VALUE self, VALUE obj);
static VALUE pgconn_s_escape_bytea(VALUE self, VALUE obj);
static VALUE find_user_reg_formatter(VALUE klass);

static VALUE
pgconn_s_format(self, obj)
    VALUE self;
    VALUE obj;
{
    VALUE result;
    VALUE record_values;
    int tainted;
    long i;
    VALUE user_form = find_user_reg_formatter( CLASS_OF(obj));
    if( user_form != Qnil)
      return rb_funcall(rb_ary_entry(user_form,0), rb_intern("call"), 1, obj);
        
    if( CLASS_OF(obj) == rb_cPGbytea) {
      return  pgconn_s_escape_bytea(self, obj);
    }
    
    switch(TYPE(obj)) {
    case T_STRING:
      return obj;

    case T_TRUE:
    case T_FALSE:
    case T_FIXNUM:
    case T_BIGNUM:
    case T_FLOAT:
      return rb_obj_as_string(obj);

    case T_NIL:
      return rb_str_new2("NULL");

    case T_ARRAY:
      result = rb_str_buf_new2("{");
      tainted = OBJ_TAINTED(obj);
      for (i = 0; i < RARRAY(obj)->len; i++) {
          VALUE element = format_array_element(self, RARRAY(obj)->ptr[i]);
          if (OBJ_TAINTED(RARRAY(obj)->ptr[i])) tainted = Qtrue;
          if (i > 0) rb_str_buf_cat2(result, ",");
          rb_str_buf_append(result, element);
      }
      rb_str_buf_cat2(result, "}");
      if (tainted) OBJ_TAINT(result);
      return result;

    default:
      if (CLASS_OF(obj) == rb_cBigDecimal) {
          return rb_funcall(obj, rb_intern("to_s"), 1, rb_str_new2("F"));
      }
      if (CLASS_OF(obj) == rb_cPGrecord){
        result = rb_str_buf_new2("(");
        tainted = OBJ_TAINTED(obj);
        record_values = rb_iv_get(obj, "@values");
        VALUE prev_obj = Qnil;
        for (i = 0; i < RARRAY(record_values)->len; i++) {
          if (i > 0) rb_str_buf_cat2(result, ",");
          VALUE cur_obj = RARRAY(record_values)->ptr[i];
          if (Qnil != cur_obj) {
              VALUE element = format_array_element(self, cur_obj);
              if (OBJ_TAINTED(cur_obj)) tainted = Qtrue;
              if (CLASS_OF(cur_obj) == rb_cPGrecord || TYPE(cur_obj) == T_ARRAY)
                rb_str_buf_cat2(result, "\"");
              rb_str_buf_append(result, element);
              if (CLASS_OF(cur_obj) == rb_cPGrecord || TYPE(cur_obj) == T_ARRAY)
                rb_str_buf_cat2(result, "\"");
          }
          prev_obj = cur_obj;
        }
        rb_str_buf_cat2(result, ")");
        return result;
      }
      if (CLASS_OF(obj) == rb_cDate || CLASS_OF(obj) == rb_cDateTime || CLASS_OF(obj) == rb_cTime){
        VALUE formatted_time = rb_funcall(obj, rb_intern("strftime"), 1, rb_str_new2("%Y-%m-%d %H:%M:%S"));
        if( CLASS_OF(obj) == rb_cTime ) {
          char usec[8];
          sprintf(usec, ".%06ld", NUM2LONG(rb_funcall(obj, rb_intern("usec"),0)) );
          rb_str_buf_cat(formatted_time, usec, 7);
        }
        return formatted_time;
      }
      if (rb_block_given_p()) {
          return rb_yield(obj);
      } else {
          rb_raise(rb_ePGError, "can't format");
      }
    }
}
static size_t escape_array_string(char * from, char * to, size_t length)
{
	const char *source = from;
	char	   *target = to;
	size_t		remaining = length;
	while (remaining > 0 && *source != '\0')
	{
		char	c = *source;
		/* Apply quoting if needed */
		if (c == '"' )
                        *target++ = '\\';
		if (c == '\\')
			*target++ = c;
		/* Copy the character */
		*target++ = c;
		source++;
		remaining--;
	}
	/* Write the terminating NUL character. */
	*target = '\0';
	return target - to;
}
static VALUE
format_array_element(self, obj)
    VALUE self, obj;
{
    VALUE usr_form ; 
    if (TYPE(obj) == T_STRING ) {
        char * temp = ALLOCA_N(char, RSTRING(obj)->len * 2+3);
        size_t q_len = escape_array_string( RSTRING(obj)->ptr, temp+1, RSTRING(obj)->len);
        temp[0]='"';
        temp[q_len+1]='"';
        temp[q_len+2]='\0';
        return rb_str_new(temp, q_len+2);
    }
    else {
      VALUE temp = pgconn_s_format(self, obj);
      usr_form = find_user_reg_formatter( CLASS_OF(obj));
      if(usr_form != Qnil && rb_ary_entry(usr_form, 1) == Qtrue)
        return format_array_element(self, temp);
      else
        return temp;
    }
}

/*
 * call-seq:
 *    PGconn.quote( obj )
 *    PGconn.quote( obj ) { |obj| ... }
 *    PGconn.format( obj )
 *    PGconn.format( obj ) { |obj| ... }
 *    conn.quote( obj )
 *    conn.quote( obj ) { |obj| ... }
 *    conn.format( obj )
 *    conn.format( obj ) { |obj| ... }
 *
 * If _obj_ is a Number, String, Array, Time, Date, DateTime, PGrecord, 
 * BigDecimal , +nil+, +true+, or +false+ then
 * #quote returns a String representation of that object safe for use in PostgreSQL.
 * 
 * It is possible to define custom format for Ruby class, using PGconn#reg_format
 * class method. Custom formatters have higher priority than built-in.
 *
 * If _obj_ is not one of the above classes and no there is no custom formatter
 * and a block is supplied to #quote, the block is invoked, passing along the object. 
 * The return value from the block is returned as a string.
 *
 * If _obj_ is not one of the recognized classes and no block is supplied,
 * a PGError is raised.
 *
 * This method is available both as class and instance method.
 */
static VALUE
pgconn_s_quote(self, obj)
    VALUE self, obj;
{
    char* quoted;
    int size,error;
    VALUE result;

    if(CLASS_OF(obj) == rb_cPGbytea ) { // special case for bytea
       VALUE buf=rb_str_buf_new2("'");
       rb_str_buf_append(buf, pgconn_s_escape_bytea(self, obj));
       rb_str_buf_cat2(buf, "'");
       return buf;
    }
    
    VALUE usr_form = find_user_reg_formatter( CLASS_OF(obj));
    if (TYPE(obj) == T_STRING ) {
        /* length * 2 because every char could require escaping */
        /* + 2 for the quotes, + 1 for the null terminator */
        quoted = ALLOCA_N(char, RSTRING(obj)->len * 2 + 2 + 1);
#ifdef HAVE_PQESCAPESTRINGCONN
        if( rb_cPGconn == CLASS_OF(self) ) {
          /* to allow call as instance method */
          size = PQescapeStringConn(get_pgconn(self),quoted + 1, RSTRING(obj)->ptr, RSTRING(obj)->len, &error);
        } else {
          size = PQescapeString(quoted + 1, RSTRING(obj)->ptr, RSTRING(obj)->len);
        }
#else
        size = PQescapeString(quoted + 1, RSTRING(obj)->ptr, RSTRING(obj)->len);
#endif        
        *quoted = *(quoted + size + 1) = SINGLE_QUOTE;
        result = rb_str_new(quoted, size + 2);
        OBJ_INFECT(result, obj);
        return result;
    }
    else 
        if (TYPE(obj) == T_ARRAY || CLASS_OF(obj) == rb_cPGrecord || 
            CLASS_OF(obj) == rb_cDate || CLASS_OF(obj) == rb_cDateTime || CLASS_OF(obj) == rb_cTime
            || (Qnil != usr_form && rb_ary_entry(usr_form, 1) == Qtrue) )  {
            return pgconn_s_quote(self, pgconn_s_format(self, obj));
        }
        else {
            return pgconn_s_format(self, obj);
        }
}

static VALUE
pgconn_s_quote_connstr(string)
    VALUE string;
{
    char *str,*ptr;
    int i,j=0,len;
    VALUE result;

    Check_Type(string, T_STRING);
    
	ptr = RSTRING(string)->ptr;
	len = RSTRING(string)->len;
    str = ALLOCA_N(char, len * 2 + 2 + 1);
	str[j++] = '\'';
	for(i = 0; i < len; i++) {
		if(ptr[i] == '\'' || ptr[i] == '\\')
			str[j++] = '\\';
		str[j++] = ptr[i];	
	}
	str[j++] = '\'';
    result = rb_str_new(str, j);
    OBJ_INFECT(result, string);
    return result;
}

static int
build_key_value_string_i(key, value, result)
    VALUE key, value, result;
{
    VALUE key_value;
    if (key == Qundef) return ST_CONTINUE;
    key_value = (TYPE(key) == T_STRING ? rb_str_dup(key) : rb_obj_as_string(key));
    rb_str_cat(key_value, "=", 1);
    rb_str_concat(key_value, pgconn_s_quote_connstr(value));
    rb_ary_push(result, key_value);
    return ST_CONTINUE;
}

/*
 * call-seq:
 *    PGconn.quote_ident( str )
 *
 * Returns a SQL-safe identifier.
 */
static VALUE
pgconn_s_quote_ident(self, string)
    VALUE self;
    VALUE string;
{
    char *str,*ptr;
    int i,j=0,len;
    VALUE result;

    Check_Type(string, T_STRING);
    
	ptr = RSTRING(string)->ptr;
	len = RSTRING(string)->len;
    str = ALLOCA_N(char, len * 2 + 2 + 1);
	str[j++] = '"';
	for(i = 0; i < len; i++) {
		if(ptr[i] == '"')
			str[j++] = '"';
		else if(ptr[i] == '\0')
			rb_raise(rb_ePGError, "Identifier cannot contain NULL bytes");
		str[j++] = ptr[i];	
	}
	str[j++] = '"';
    result = rb_str_new(str, j);
    OBJ_INFECT(result, string);
    return result;
}

/*
 * call-seq:
 *    PGconn.escape( str )
 *
 * Returns a SQL-safe version of the String _str_. Unlike #quote, does not wrap the String in '...'.
 */
static VALUE
pgconn_s_escape(self, string)
    VALUE self;
    VALUE string;
{
    char *escaped;
    int size;
    VALUE result;

    Check_Type(string, T_STRING);
    
    escaped = ALLOCA_N(char, RSTRING(string)->len * 2 + 1);
    size = PQescapeString(escaped, RSTRING(string)->ptr, RSTRING(string)->len);
    result = rb_str_new(escaped, size);
    OBJ_INFECT(result, string);
    return result;
}

/*
 * call-seq:
 *    conn.escape( str )
 * Returns a SQL-safe version of the String _str_. Unlike #quote, does not wrap the String in '...'.
 * This is instance method, that also tries to use PQescapeStringConn.
 */
static VALUE
pgconn_escape(self, string)
    VALUE self;
    VALUE string;
{
    char *escaped;
    int size,error;
    VALUE result;

    Check_Type(string, T_STRING);
    
    escaped = ALLOCA_N(char, RSTRING(string)->len * 2 + 1);
#ifdef HAVE_PQESCAPESTRINGCONN    
    size = PQescapeStringConn(get_pgconn(self),escaped, RSTRING(string)->ptr, RSTRING(string)->len, &error);
#else
    size = PQescapeString(escaped, RSTRING(string)->ptr, RSTRING(string)->len);
#endif    
    result = rb_str_new(escaped, size);
    OBJ_INFECT(result, string);
    return result;
}

/*
 * call-seq:
 *   PGconn.escape_bytea( obj )
 *   connection.escape_bytea( obj )
 *
 * Escapes binary data for use within an SQL command with the type +bytea+.
 * 
 * Certain byte values must be escaped (but all byte values may be escaped)
 * when used as part of a +bytea+ literal in an SQL statement. In general, to
 * escape a byte, it is converted into the three digit octal number equal to
 * the octet value, and preceded by two backslashes. The single quote (') and
 * backslash (\) characters have special alternative escape sequences.
 * #escape_bytea performs this operation, escaping only the minimally required bytes.
 * 
 * See the PostgreSQL documentation on PQescapeBytea[http://www.postgresql.org/docs/current/interactive/libpq-exec.html#LIBPQ-EXEC-ESCAPE-BYTEA] for more information.
 * 
 * Connection instance method for versions of 8.1 and higher of libpq
 * uses PQescapeByteaConn
 *
 */
static VALUE
pgconn_s_escape_bytea(self, obj)
    VALUE self;
    VALUE obj;
{
    char *from, *to;
    size_t from_len, to_len;
    VALUE ret;
    
    Check_Type(obj, T_STRING);
    from      = RSTRING(obj)->ptr;
    from_len  = RSTRING(obj)->len;
    
#ifdef HAVE_PQESCAPESTRINGCONN   
    if(CLASS_OF(self) == rb_cPGconn) {
        to = (char *)PQescapeByteaConn(get_pgconn(self),from, from_len, &to_len);
    } else {
        to = (char *)PQescapeBytea(from, from_len, &to_len);
    }
#else
    to = (char *)PQescapeBytea(from, from_len, &to_len);
#endif    
    
    ret = rb_str_new(to, to_len - 1);
    OBJ_INFECT(ret, obj);
    
    PQfreemem(to);
    
    return ret;
}


/*
 * call-seq:
 *   PGconn.unescape_bytea( obj )
 *
 * Converts an escaped string representation of binary data into binary data --- the
 * reverse of #escape_bytea. This is needed when retrieving +bytea+ data in text format,
 * but not when retrieving it in binary format.
 *
 * See the PostgreSQL documentation on PQunescapeBytea[http://www.postgresql.org/docs/current/interactive/libpq-exec.html#LIBPQ-EXEC-ESCAPE-BYTEA] for more information.
 */
static VALUE
pgconn_s_unescape_bytea(self, obj)
    VALUE self, obj;
{
    char *from, *to;
    size_t to_len;
    VALUE ret;

    Check_Type(obj, T_STRING);
    from = StringValuePtr(obj);

    to = (char *) PQunescapeBytea(from, &to_len);

    ret = rb_str_new(to, to_len);
    OBJ_INFECT(ret, obj);
    PQfreemem(to);

    return ret;
}

/*
 * Document-method: new
 *
 * call-seq:
 *     PGconn.open(connection_hash) -> PGconn
 *     PGconn.open(connection_string) -> PGconn
 *     PGconn.open(host, port, options, tty, dbname, login, passwd) ->  PGconn
 *
 *  _host_::     server hostname
 *  _port_::     server port number
 *  _options_::  backend options (String)
 *  _tty_::      tty to print backend debug message <i>(ignored in newer versions of PostgreSQL)</i> (String)
 *  _dbname_::     connecting database name
 *  _login_::      login user name
 *  _passwd_::     login password
 *  
 *  On failure, it raises a PGError exception.
 */
#ifndef HAVE_RB_DEFINE_ALLOC_FUNC
static VALUE
pgconn_s_new(argc, argv, klass)
    int argc;
    VALUE *argv;
    VALUE klass;
{
    VALUE obj = rb_obj_alloc(klass);
    rb_obj_call_init(obj, argc, argv);
    return obj;
}
#endif

static VALUE
pgconn_init(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    pgconn_connect(argc, argv, self);
    if (rb_block_given_p()) {
        return rb_ensure(rb_yield, self, pgconn_close, self);
    }
    return self;
}

static PGconn*
get_pgconn(obj)
    VALUE obj;
{
    PGconn *conn;

    Data_Get_Struct(obj, PGconn, conn);
    if (conn == NULL) rb_raise(rb_ePGError, "closed connection");
    return conn;
}

/*
 * call-seq:
 *    conn.close
 *
 * Closes the backend connection.
 */
static VALUE
pgconn_close(self)
    VALUE self;
{
    PQfinish(get_pgconn(self));
    DATA_PTR(self) = NULL;
    return Qnil;
}

/*
 * call-seq:
 *    conn.reset()
 *
 * Resets the backend connection. This method closes the backend  connection and tries to re-connect.
 */
static VALUE
pgconn_reset(obj)
    VALUE obj;
{
    PQreset(get_pgconn(obj));
    return obj;
}

static PGresult*
get_pgresult(obj)
    VALUE obj;
{
    PGresult *result;
    Data_Get_Struct(obj, PGresult, result);
    if (result == NULL) rb_raise(rb_ePGError, "query not performed");
    return result;
}

#ifndef HAVE_PQEXECPARAMS
PGresult *PQexecParams_compat(PGconn *conn, VALUE command, VALUE values);
#endif

/*
 * call-seq:
 *    conn.exec(sql, *bind_values)
 *
 * Sends SQL query request specified by _sql_ to the PostgreSQL.
 * Returns a PGresult instance on success.
 * On failure, it raises a PGErr or exception.
 *
 * +bind_values+ represents values for the PostgreSQL bind parameters found in the +sql+.  PostgreSQL bind parameters are presented as $1, $1, $2, etc.
 */
static VALUE
pgconn_exec(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result = NULL;
    VALUE command, params, c_reslt;
    char *msg;

    rb_scan_args(argc, argv, "1*", &command, &params);

    Check_Type(command, T_STRING);

    if (RARRAY(params)->len <= 0) {
        result = PQexec(conn, StringValuePtr(command));
    }
    else {
        int len = RARRAY(params)->len;
        int i;
#ifdef HAVE_PQEXECPARAMS
        VALUE* ptr = RARRAY(params)->ptr;
        char const** values = ALLOCA_N(char const*, len);
        int* lengths = ALLOCA_N(int, len);
        int* formats = ALLOCA_N(int, len);
        VALUE formatted;
        for (i = 0; i < len; i++, ptr++) {
            if (*ptr == Qnil) {
                values[i] = NULL;
                lengths[i] = 0;
                formats[i] = 0;
            } else if (CLASS_OF(*ptr) == rb_cPGbytea) {
                values[i] = StringValuePtr(*ptr);
                lengths[i] = RSTRING(*ptr)->len;
                formats[i] = 1;
            } else {
                formatted = pgconn_s_format(obj, *ptr);
                values[i] = StringValuePtr(formatted);
                lengths[i] = RSTRING(formatted)->len;
                formats[i] = 0;
            }
        }
        result = PQexecParams(conn, StringValuePtr(command), len, NULL, values, lengths, formats, 0);
#else
        for (i = 0; i < len; i++) {
            rb_ary_store(params, i, pgconn_s_quote(obj, rb_ary_entry(params, i)));
        }
        result = PQexecParams_compat(conn, command, params);
#endif
    }

    if (!result) {
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }

    switch (PQresultStatus(result)) {
    case PGRES_TUPLES_OK:
    case PGRES_COPY_OUT:
    case PGRES_COPY_IN:
    case PGRES_EMPTY_QUERY:
    case PGRES_COMMAND_OK: {
      c_reslt = pgresult_new(result);
      rb_iv_set(c_reslt, "@connection", obj);
      if(rb_block_given_p()) {
        return rb_ensure(rb_yield, c_reslt, pgresult_clear, c_reslt);
      }
      else {
          return c_reslt;
      }
    }

    case PGRES_BAD_RESPONSE:
    case PGRES_FATAL_ERROR:
    case PGRES_NONFATAL_ERROR:
      msg = RSTRING(rb_str_new2(PQresultErrorMessage(result)))->ptr;
      break;
    default:
      msg = "internal error : unknown result status.";
      break;
    }
    PQclear(result);
    rb_raise(rb_ePGError, msg);
}

/*
 * call-seq:
 *    conn.async_exec( sql )
 *
 * Sends an asyncrhonous SQL query request specified by _sql_ to the PostgreSQL.
 * Returns a PGresult instance on success.
 * On failure, it raises a PGError exception.
 */
static VALUE
pgconn_async_exec(obj, str)
    VALUE obj, str;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result;
    char *msg;

    int cs;
    int ret;
    fd_set rset;

    Check_Type(str, T_STRING);
        
    while ((result = PQgetResult(conn)) != NULL) {
        PQclear(result);
    }

    if (!PQsendQuery(conn, RSTRING(str)->ptr)) {
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }

    cs = PQsocket(conn);
    for(;;) {
        FD_ZERO(&rset);
        FD_SET(cs, &rset);
        ret = rb_thread_select(cs + 1, &rset, NULL, NULL, NULL);
        if (ret < 0) {
            rb_sys_fail(0);
        }
                
        if (ret == 0) {
            continue;
        }

        if (PQconsumeInput(conn) == 0) {
            rb_raise(rb_ePGError, PQerrorMessage(conn));
        }

        if (PQisBusy(conn) == 0) {
            break;
        }
    }

    result = PQgetResult(conn);

    if (!result) {
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }

    switch (PQresultStatus(result)) {
    case PGRES_TUPLES_OK:
    case PGRES_COPY_OUT:
    case PGRES_COPY_IN:
    case PGRES_EMPTY_QUERY:
    case PGRES_COMMAND_OK:      /* no data will be received */
      return pgresult_new(result);

    case PGRES_BAD_RESPONSE:
    case PGRES_FATAL_ERROR:
    case PGRES_NONFATAL_ERROR:
      msg = RSTRING(rb_str_new2(PQresultErrorMessage(result)))->ptr;
      break;
    default:
      msg = "internal error : unknown result status.";
      break;
    }
    PQclear(result);
    rb_raise(rb_ePGError, msg);
}

/*
 * call-seq:
 *    conn.query(sql, *bind_values)
 *
 * Sends SQL query request specified by _sql_ to the PostgreSQL.
 * Returns an Array as the resulting tuple on success.
 * On failure, it returns +nil+, and the error details can be obtained by #error.
 *
 * +bind_values+ represents values for the PostgreSQL bind parameters found in the +sql+.  PostgreSQL bind parameters are presented as $1, $1, $2, etc.
 */
static VALUE
pgconn_query(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    return pgresult_result_with_clear(pgconn_exec(argc, argv, obj));
}

/*
 * call-seq:
 *    conn.async_query(sql)
 *
 * Sends an asynchronous SQL query request specified by _sql_ to the PostgreSQL.
 * Returns an Array as the resulting tuple on success.
 * On failure, it returns +nil+, and the error details can be obtained by #error.
 */
static VALUE
pgconn_async_query(obj, str)
    VALUE obj, str;
{
    return pgresult_result_with_clear(pgconn_async_exec(obj, str));
}

/*
 * call-seq:
 *    conn.get_notify()
 *
 * Returns an array of the unprocessed notifiers.
 * If there is no unprocessed notifier, it returns +nil+.
 */
static VALUE
pgconn_get_notify(obj)
    VALUE obj;
{
    PGconn* conn = get_pgconn(obj);
    PGnotify *notify;
    VALUE ary;

    if (PQconsumeInput(conn) == 0) {
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }
    /* gets notify and builds result */
    notify = PQnotifies(conn);
    if (notify == NULL) {
        /* there are no unhandled notifications */
        return Qnil;
    }
    ary = rb_ary_new3(2, rb_tainted_str_new2(notify->relname), INT2NUM(notify->be_pid));
    PQfreemem(notify);

    /* returns result */
    return ary;
}

static VALUE pg_escape_regex;
static VALUE pg_escape_str;
static ID    pg_gsub_bang_id;

static void
free_pgconn(ptr)
    PGconn *ptr;
{
    PQfinish(ptr);
}

/*
 * call-seq:
 *    conn.insert_table( table, values )
 *
 * Inserts contents of the _values_ Array into the _table_.
 */
static VALUE
pgconn_insert_table(obj, table, values)
    VALUE obj, table, values;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result;
    VALUE s, buffer;
    int i, j;
    int res = 0;

    Check_Type(table, T_STRING);
    Check_Type(values, T_ARRAY);
    i = RARRAY(values)->len;
    while (i--) {
        if (TYPE(RARRAY(RARRAY(values)->ptr[i])) != T_ARRAY) {
            rb_raise(rb_ePGError, "second arg must contain some kind of arrays.");
        }
    }
    
    buffer = rb_str_new(0, RSTRING(table)->len + 17 + 1);
    /* starts query */
    snprintf(RSTRING(buffer)->ptr, RSTRING(buffer)->len, "copy %s from stdin ", StringValuePtr(table));
    
    result = PQexec(conn, StringValuePtr(buffer));
    if (!result){
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }
    PQclear(result);

    for (i = 0; i < RARRAY(values)->len; i++) {
        struct RArray *row = RARRAY(RARRAY(values)->ptr[i]);
        buffer = rb_tainted_str_new(0,0);
        for (j = 0; j < row->len; j++) {
            if (j > 0) rb_str_cat(buffer, "\t", 1);
            if (NIL_P(row->ptr[j])) {
                rb_str_cat(buffer, "\\N",2);
            } else {
                s = rb_obj_as_string(row->ptr[j]);
                rb_funcall(s,pg_gsub_bang_id,2,pg_escape_regex,pg_escape_str);
                rb_str_cat(buffer, StringValuePtr(s), RSTRING(s)->len);
            }
        }
        rb_str_cat(buffer, "\n\0", 2);
        /* sends data */
        PQputline(conn, StringValuePtr(buffer));
    }
    PQputline(conn, "\\.\n");
    res = PQendcopy(conn);

    return obj;
}

/*
 * call-seq:
 *    conn.putline()
 *
 * Sends the string to the backend server.
 * Users must send a single "." to denote the end of data transmission.
 */
static VALUE
pgconn_putline(obj, str)
    VALUE obj, str;
{
    Check_Type(str, T_STRING);
    PQputline(get_pgconn(obj), StringValuePtr(str));
    return obj;
}

/*
 * call-seq:
 *    conn.getline()
 *
 * Reads a line from the backend server into internal buffer.
 * Returns +nil+ for EOF, +0+ for success, +1+ for buffer overflowed.
 * You need to ensure single "." from backend to confirm  transmission completion.
 * The sample program <tt>psql.rb</tt> (see source for postgres) treats this copy protocol right.
 */
static VALUE
pgconn_getline(obj)
    VALUE obj;
{
    PGconn *conn = get_pgconn(obj);
    VALUE str;
    long size = BUFSIZ;
    long bytes = 0;
    int  ret;
    
    str = rb_tainted_str_new(0, size);

    for (;;) {
        ret = PQgetline(conn, RSTRING(str)->ptr + bytes, size - bytes);
        switch (ret) {
        case EOF:
          return Qnil;
        case 0:
          rb_str_resize(str, strlen(StringValuePtr(str)));
          return str;
        }
        bytes += BUFSIZ;
        size += BUFSIZ;
        rb_str_resize(str, size);
    }
    return Qnil;
}

/*
 * call-seq:
 *    conn.endcopy()
 *
 * Waits until the backend completes the copying.
 * You should call this method after #putline or #getline.
 * Returns +nil+ on success; raises an exception otherwise.
 */
static VALUE
pgconn_endcopy(obj)
    VALUE obj;
{
    if (PQendcopy(get_pgconn(obj)) == 1) {
        rb_raise(rb_ePGError, "cannot complete copying");
    }
    return Qnil;
}

static void
notice_proxy(self, message)
    VALUE self;
    const char *message;
{
    VALUE block;
    if ((block = rb_iv_get(self, "@on_notice")) != Qnil) {
        rb_funcall(block, rb_intern("call"), 1, rb_str_new2(message));
    }
}

/*
 * call-seq:
 *   conn.on_notice {|message| ... }
 *
 * Notice and warning messages generated by the server are not returned
 * by the query execution functions, since they do not imply failure of
 * the query. Instead they are passed to a notice handling function, and
 * execution continues normally after the handler returns. The default
 * notice handling function prints the message on <tt>stderr</tt>, but the
 * application can override this behavior by supplying its own handling
 * function.
 */
static VALUE
pgconn_on_notice(self)
    VALUE self;
{
    VALUE block = rb_block_proc();
    PGconn *conn = get_pgconn(self);
    if (PQsetNoticeProcessor(conn, NULL, NULL) != notice_proxy) {
        PQsetNoticeProcessor(conn, notice_proxy, (void *) self);
    }
    rb_iv_set(self, "@on_notice", block);
    return self;
}

/*
 * call-seq:
 *    conn.host()
 *
 * Returns the connected server name.
 */
static VALUE
pgconn_host(obj)
    VALUE obj;
{
    char *host = PQhost(get_pgconn(obj));
    if (!host) return Qnil;
    return rb_tainted_str_new2(host);
}

/*
 * call-seq:
 *    conn.port()
 *
 * Returns the connected server port number.
 */
static VALUE
pgconn_port(obj)
    VALUE obj;
{
    char* port = PQport(get_pgconn(obj));
    return INT2NUM(atol(port));
}

/*
 * call-seq:
 *    conn.db()
 *
 * Returns the connected database name.
 */
static VALUE
pgconn_db(obj)
    VALUE obj;
{
    char *db = PQdb(get_pgconn(obj));
    if (!db) return Qnil;
    return rb_tainted_str_new2(db);
}

/*
 * call-seq:
 *    conn.options()
 *
 * Returns backend option string.
 */
static VALUE
pgconn_options(obj)
    VALUE obj;
{
    char *options = PQoptions(get_pgconn(obj));
    if (!options) return Qnil;
    return rb_tainted_str_new2(options);
}

/*
 * call-seq:
 *    conn.tty()
 *
 * Returns the connected pgtty.
 */
static VALUE
pgconn_tty(obj)
    VALUE obj;
{
    char *tty = PQtty(get_pgconn(obj));
    if (!tty) return Qnil;
    return rb_tainted_str_new2(tty);
}

/*
 * call-seq:
 *    conn.user()
 *
 * Returns the authenticated user name.
 */
static VALUE
pgconn_user(obj)
    VALUE obj;
{
    char *user = PQuser(get_pgconn(obj));
    if (!user) return Qnil;
    return rb_tainted_str_new2(user);
}

/*
 * call-seq:
 *    conn.status()
 *
 * Returns status of connection : CONNECTION_OK or CONNECTION_BAD
 */
static VALUE
pgconn_status(obj)
    VALUE obj;
{
    return INT2NUM(PQstatus(get_pgconn(obj)));
}

/*
 * call-seq:
 *    conn.error()
 *
 * Returns the error message about connection.
 */
static VALUE
pgconn_error(obj)
    VALUE obj;
{
    char *error = PQerrorMessage(get_pgconn(obj));
    if (!error) return Qnil;
    return rb_tainted_str_new2(error);
}

/*
 * call-seq:
 *    conn.trace( port )
 * 
 * Enables tracing message passing between backend.
 * The trace message will be written to the _port_ object,
 * which is an instance of the class +File+.
 */
static VALUE
pgconn_trace(obj, port)
    VALUE obj, port;
{
    OpenFile* fp;

    Check_Type(port, T_FILE);
    GetOpenFile(port, fp);

    PQtrace(get_pgconn(obj), fp->f2?fp->f2:fp->f);

    return obj;
}

/*
 * call-seq:
 *    conn.untrace()
 * 
 * Disables the message tracing.
 */
static VALUE
pgconn_untrace(obj)
    VALUE obj;
{
    PQuntrace(get_pgconn(obj));
    return obj;
}

/*
 * call-seq:
 *    conn.transaction_status()
 *
 * returns one of the following statuses:
 *   PQTRANS_IDLE    = 0 (connection idle)
 *   PQTRANS_ACTIVE  = 1 (command in progress)
 *   PQTRANS_INTRANS = 2 (idle, within transaction block)
 *   PQTRANS_INERROR = 3 (idle, within failed transaction)
 *   PQTRANS_UNKNOWN = 4 (cannot determine status)
 *
 * See the PostgreSQL documentation on PQtransactionStatus[http://www.postgresql.org/docs/current/interactive/libpq-status.html#AEN24919] for more information.
 */
static VALUE
pgconn_transaction_status(obj)
    VALUE obj;
{
    return INT2NUM(PQtransactionStatus(get_pgconn(obj)));
}

#ifdef HAVE_PQSETCLIENTENCODING

/*
 * call-seq:
 *  conn.protocol_version -> Integer
 *
 * The 3.0 protocol will normally be used when communicating with PostgreSQL 7.4 or later servers; pre-7.4 servers support only protocol 2.0. (Protocol 1.0 is obsolete and not supported by libpq.)
 */
static VALUE
pgconn_protocol_version(obj)
    VALUE obj;
{
    return INT2NUM(PQprotocolVersion(get_pgconn(obj)));
}

/*
 * call-seq:
 *   conn.server_version -> Integer
 *
 * The number is formed by converting the major, minor, and revision numbers into two-decimal-digit numbers and appending them together. For example, version 7.4.2 will be returned as 70402, and version 8.1 will be returned as 80100 (leading zeroes are not shown). Zero is returned if the connection is bad.
 */
static VALUE
pgconn_server_version(obj)
    VALUE obj;
{
    return INT2NUM(PQserverVersion(get_pgconn(obj)));
}

/*
 * call-seq:
 *   conn.lastval -> Integer
 *
 * Returns the sequence value returned by the last call to the PostgreSQL function <tt>nextval(sequence_name)</tt>. Equivalent to <tt>conn.query('select lastval()').first.first</tt>.
 *
 * This functionality is only available with PostgreSQL 8.1 and newer.
 * See the PostgreSQL documentation on lastval[http://www.postgresql.org/docs/current/interactive/functions-sequence.html] for more information.
 */
static VALUE
pgconn_lastval(obj)
    VALUE obj;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result;
    VALUE lastval, error;

    result = PQexec(conn, "select lastval()");
    if (!result) rb_raise(rb_ePGError, PQerrorMessage(conn));

    switch (PQresultStatus(result)) {
    case PGRES_TUPLES_OK:
      lastval = rb_cstr2inum(PQgetvalue(result, 0, 0), 10);
      PQclear(result);
      return lastval;

    case PGRES_BAD_RESPONSE:
    case PGRES_FATAL_ERROR:
    case PGRES_NONFATAL_ERROR:
      error = rb_str_new2(PQresultErrorMessage(result));
      PQclear(result);
      rb_raise(rb_ePGError, StringValuePtr(error));

    default:
      PQclear(result);
      rb_raise(rb_ePGError, "unknown lastval");
    }
}

/*
 * call-seq:
 *    conn.client_encoding() -> String
 * 
 * Returns the client encoding as a String.
 */
static VALUE
pgconn_client_encoding(obj)
    VALUE obj;
{
    char *encoding = (char *)pg_encoding_to_char(PQclientEncoding(get_pgconn(obj)));
    return rb_tainted_str_new2(encoding);
}

/*
 * call-seq:
 *    conn.set_client_encoding( encoding )
 * 
 * Sets the client encoding to the _encoding_ String.
 */
static VALUE
pgconn_set_client_encoding(obj, str)
    VALUE obj, str;
{
    Check_Type(str, T_STRING);
    if ((PQsetClientEncoding(get_pgconn(obj), StringValuePtr(str))) == -1){
        rb_raise(rb_ePGError, "invalid encoding name %s",str);
    }
    return Qnil;
}
#endif

static void
free_pgresult(ptr)
    PGresult *ptr;
{
    PQclear(ptr);
}

#define VARHDRSZ 4
#define SCALE_MASK 0xffff

static int
has_numeric_scale(typmod)
    int typmod;
{
    if (typmod == -1) return 1;
    return (typmod - VARHDRSZ) & SCALE_MASK;
}

#define PARSE(klass, string) rb_funcall(klass, rb_intern("parse"), 1, rb_tainted_str_new2(string));

/* checks if we have cached composite type definitions */
static int
loaded_composite_types(VALUE conn)
{
  return RTEST(rb_iv_get(conn, "@loaded_composite_types"));  
}

/* looks for composite type definition in cache */
static VALUE
find_composite_type(conn, tip)
  VALUE conn, tip;
{
  return rb_hash_aref(rb_iv_get(conn, "@cached_types"), tip);
}

/* looks for user  registered type translator */
static VALUE
find_user_reg_translator(VALUE conn, VALUE tip){
  VALUE trs = rb_iv_get(conn, "@translators");
  if (trs == Qnil) return Qnil;
  return rb_hash_aref(trs, tip);
}
/* looks for user  registered type formatter */
static VALUE
find_user_reg_formatter( VALUE klass){
  VALUE trs = rb_cv_get(rb_cPGconn, "@@formatters");
  if (trs == Qnil) return Qnil;
  return rb_hash_aref(trs, klass);
}
/* looks for user  registered array types */
static VALUE
find_user_reg_array_type(VALUE conn, VALUE oid_s){
  VALUE trs = rb_iv_get(conn, "@array_types");
  if (trs == Qnil) return Qnil;
  return rb_hash_aref(trs, oid_s);
}

static char * raise_error(PGconn * conn, PGresult * result);
static char * type_oid_find_query = "select '%s'::regtype::oid";
static char * array_type_oid_find_query = "select oid, typelem from pg_type where oid = '%s'::regtype::oid";

static VALUE 
pgconn_find_type_oid (VALUE obj, VALUE name)
{
    PGconn * conn = get_pgconn(obj) ;
    PGresult * result;
    char * msg;
    char * quoted = ALLOCA_N(char, RSTRING(name)->len * 2 + 2 + 1);
    int size = PQescapeString(quoted, RSTRING(name)->ptr, RSTRING(name)->len);
    char * query = ALLOCA_N(char, size+strlen(type_oid_find_query)+1);
    sprintf(query, type_oid_find_query, quoted);
    result=PQexec(conn, query);
    if((msg = raise_error(conn, result)))
    {
      if(result) PQclear(result);
      rb_raise(rb_ePGError, msg);
      return Qnil;
    }
    VALUE oid_int = rb_cstr2inum(PQgetvalue(result, 0, 0), 10);
    PQclear(result);
    return oid_int;
}
/*
 * call-seq:
 *     conn.reg_array(array_type_name)
 *
 * Registers new array type. Base type is resolved from pg_type[http://www.postgresql.org/docs/current/interactive/catalog-pg-type.html] system table 
 * 
 *     conn.reg_array("point[]")
 * */
static VALUE
pgconn_reg_array(self, array_type)
    VALUE self, array_type;
{
    Check_Type(array_type, T_STRING);
    PGconn * conn = get_pgconn(self) ;
    PGresult * result;
    char * msg;
    char * quoted = ALLOCA_N(char, RSTRING(array_type)->len * 2 + 2 + 1);
    int size = PQescapeString(quoted, RSTRING(array_type)->ptr, RSTRING(array_type)->len);
    char * query = ALLOCA_N(char, size+strlen(array_type_oid_find_query)+1);
    sprintf(query, array_type_oid_find_query, quoted);
    result=PQexec(conn, query);
    if((msg = raise_error(conn, result)))
    {
      if(result) PQclear(result);
      rb_raise(rb_ePGError, msg);
      return Qnil;
    }
    if(PQntuples(result) == 1){
      VALUE oid_s = rb_cstr2inum(PQgetvalue(result, 0, 0), 10);
      VALUE oid_base = rb_cstr2inum(PQgetvalue(result, 0, 1), 10);
      VALUE translators = rb_iv_get(self, "@array_types");
      VALUE rec = rb_ary_new2(2);
      PQclear(result);
      rb_ary_push(rec, oid_base);
      rb_ary_push(rec, array_type);
      rb_hash_aset(translators, oid_s, rec);
      return oid_s;
    }
    rb_raise(rb_ePGError, "Type '%s' not found", RSTRING(array_type)->ptr);
}
/*
 * call-seq:
 *     conn.reg_translate(type_name, proc_block)
 *
 * Registers new translator proc. Translator proc must accept string representation 
 * and return Ruby value:
 * 
 *     conn.reg_translate("inet", proc {|s| IPAddr.new(s) })
 * */
static VALUE
pgconn_reg_translate(self, typename, proc_block)
    VALUE self, typename, proc_block;
{
    Check_Type(typename, T_STRING);

    VALUE oid_s = pgconn_find_type_oid(self, typename);
    if(Qnil != oid_s) 
    {
      VALUE translators = rb_iv_get(self, "@translators");
      VALUE rec = rb_ary_new2(2);
      rb_ary_push(rec, typename);
      rb_ary_push(rec, proc_block);
      rb_hash_aset(translators, oid_s, rec);
    }
    return oid_s;
}
/*
 * call-seq:
 *     PGconn.reg_format(klass, proc_block, need_quotes)
 *
 * Registers new formatter proc. Formatter proc must return unquoted properly 
 * formatted string representation of Ruby value:
 * 
 *     PGconn.reg_format(Time, proc {|t| t.strftime("%H:%M:%S") }, true)
 * */
static VALUE
pgconn_reg_format(self, klass, proc_block, need_quotes)
    VALUE self, klass, proc_block, need_quotes;
{
      Check_Type(klass, T_CLASS);
      if(need_quotes != Qtrue && need_quotes != Qfalse) 
        rb_raise(rb_eArgError, "need_quotes must be boolean! got ");
      VALUE formatters = rb_cv_get(self, "@@formatters");
    
      VALUE rec = rb_ary_new2(2);
      rb_ary_push(rec, proc_block);
      rb_ary_push(rec, need_quotes);
      rb_hash_aset(formatters, klass, rec);
      return Qtrue;
}


static VALUE parse_composite(VALUE conn, char* string, VALUE type);

/* parses single string value, converting to appropriate ruby type */    
static VALUE 
std_single_value(conn, string, typeoid, typmod)
    VALUE conn;
    char *string;
    Oid typeoid;
    int typmod;
{
    VALUE compType, oid_s, proc_ary;
    switch (typeoid) {

        case BOOLOID:
          return *string == 't' ? Qtrue : Qfalse;
    
        case BYTEAOID:
          return rb_funcall(rb_cPGbytea, rb_intern("new"), 1, pgconn_s_unescape_bytea(conn, rb_tainted_str_new2(string)));
    
        case NUMERICOID:
          if (has_numeric_scale(typmod)) {
              return rb_funcall(rb_cBigDecimal, rb_intern("new"), 1, rb_str_new2(string));
          }
          /* when scale == 0 return inum */
    
        case INT8OID:
        case INT4OID:
        case INT2OID:
        case OIDOID:
          return rb_cstr2inum(string, 10);
    
        case FLOAT8OID:
        case FLOAT4OID:
          return rb_float_new(rb_cstr_to_dbl(string, Qfalse));
    
        case DATEOID:
          return PARSE(rb_cDate, string);
        case TIMEOID:
        case TIMETZOID:
          return PARSE(rb_cTime, string);
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
          return PARSE(rb_cDateTime, string);
    
        default:
          oid_s = INT2NUM(typeoid);
          proc_ary = find_user_reg_translator(conn, oid_s);
          if(Qnil != proc_ary){
              return rb_funcall(rb_ary_entry(proc_ary, 1), rb_intern("call"), 1, rb_str_new2(string));
          }
          if(loaded_composite_types(conn)) {
              compType = find_composite_type(conn, oid_s);
              if(Qnil != compType) 
                  return parse_composite(conn, string, compType); 
          }
          return rb_tainted_str_new2(string);
    }

}

#include "parse_array.c"
static VALUE
parse_string(conn, string, ftype, fmod)
    VALUE  conn;
    char * string;
    Oid    ftype;
    int    fmod;
{
    VALUE oid_s, rec;
    Oid base_oid;
    if(parse_arrays) {
        switch (ftype) {
            case ARRAYS_INT2OID:
            case ARRAYS_INT4OID:
            case ARRAYS_INT8OID:
              return pg_parse_array(conn, string, INT8OID, -1);
              
            case ARRAYS_FLOAT4OID:
            case ARRAYS_FLOAT8OID:
              return pg_parse_array(conn, string, FLOAT8OID, -1);
              
            case ARRAYS_NUMERICOID:
              return pg_parse_array(conn, string, NUMERICOID, -1);
              
            case ARRAYS_BOOLOID:
              return pg_parse_array(conn, string, BOOLOID, -1);
      
            case ARRAYS_TEXTOID:
            case ARRAYS_CHAROID:
            case ARRAYS_VARCHAROID:
            case ARRAYS_BPCHAROID:
              return pg_parse_array(conn, string, TEXTOID, -1);
            
            case ARRAYS_DATEOID:
              return pg_parse_array(conn, string, DATEOID, -1);

            case ARRAYS_TIMEOID:
            case ARRAYS_TIMETZOID:
              return pg_parse_array(conn, string, TIMEOID, -1);
            
            case ARRAYS_TIMESTAMPOID:
            case ARRAYS_TIMESTAMPTZOID:
              return pg_parse_array(conn, string, TIMESTAMPOID, -1);
         
            default:
              oid_s = INT2NUM(ftype);
              rec = find_user_reg_array_type(conn, oid_s);
              if(Qnil != rec) {
                  base_oid = NUM2INT(rb_ary_entry(rec, 0));
                  return pg_parse_array(conn, string, base_oid,-1);
              }
              return std_single_value(conn, string, ftype, fmod );
        }
    }
    else
        return std_single_value(conn, string, ftype, fmod );
}
/*
 * call-seq:
 *     conn.parse(type, string) -> Object
 *
 * If #translate_results= is set to +true+ parses string using translation 
 * rules for given type, converts it to appropriate
 * ruby class or +String+ if it can't be converted, with respect to all present
 * built-in and custom user-defined translations and composite types.
 *
 * If type is +Fixnum+ it is treated as PostgreSQL type Oid, 
 * if it is +String+, then query to database is performed to find Oid of given type,
 *
 *     conn.parse(23, "1234") # => Fixnum(1234)
 *     conn.parse("float[]", "{1.2, 3.4}") # => Array [1.2, 3.4]
 *
 * Raises PGError if wrong type specified.
 * Raises ArgumentError if second argument is not +String+.
 * Other exceptions can be raised by parse procedures.
 * 
 * */

VALUE pgconn_parse(self, typename, value)
    VALUE self, typename, value;
{
   if( value == Qnil ) return Qnil;
   
   Check_Type(value, T_STRING);
   if( 0 == translate_results )
     return value;
   
   if( TYPE(typename) == T_STRING )
     return parse_string(self, RSTRING(value)->ptr, NUM2INT(pgconn_find_type_oid(self, typename)), -1);
   if( TYPE(typename) == T_FIXNUM )
     return parse_string(self, RSTRING(value)->ptr, NUM2INT(typename), -1);
   rb_raise(rb_eArgError, "expected either String or Fixnum as type id");
}
#include "parse_composite.c"

static VALUE
fetch_pgresult(conn, result, row, column)
    VALUE    conn;
    PGresult *result;
    int row;
    int column;
{
    char* string;
    Oid ftype;
    int fmod;

    if (PQgetisnull(result, row, column)) {
        return Qnil;
    }

    string = PQgetvalue(result, row, column);

    if (!translate_results) {
        return rb_tainted_str_new2(string);
    }

    ftype = PQftype(result, column);
    fmod = PQfmod(result, column);
    
    return parse_string(conn, string, ftype, fmod);
}


static VALUE
pgresult_new(ptr)
    PGresult *ptr;
{
    return Data_Wrap_Struct(rb_cPGresult, 0, free_pgresult, ptr);
}

static VALUE
get_result_conn(VALUE result){
  return rb_iv_get(result, "@connection");
}
/*
 * call-seq:
 *    res.status()
 *
 * Returns the status of the query. The status value is one of:
 * * +EMPTY_QUERY+
 * * +COMMAND_OK+
 * * +TUPLES_OK+
 * * +COPY_OUT+
 * * +COPY_IN+
 */
static VALUE
pgresult_status(obj)
    VALUE obj;
{
    return INT2NUM(PQresultStatus(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.result()
 *
 * Returns an array of tuples (rows, which are themselves arrays) that represent the query result.
 */

static VALUE
fetch_pgrow(self, fields, row_num)
    VALUE self, fields;
    int row_num;
{
    PGresult *result = get_pgresult(self);
    VALUE row = rb_funcall(rb_cPGrow, rb_intern("new"), 1, fields);
    int field_num;
    for (field_num = 0; field_num < RARRAY(fields)->len; field_num++) {
       rb_ary_store(row, field_num, fetch_pgresult(get_result_conn(self), result, row_num, field_num));
    }
    return row;
}

/*
 * call-seq:
 *   conn.select_one(query, *bind_values)
 *
 * Return the first row of the query results.
 * Equivalent to conn.query(query, *bind_values).first
 */
static VALUE
pgconn_select_one(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    VALUE result = pgconn_exec(argc, argv, self);
    VALUE row = fetch_pgrow(result, pgresult_fields(result), 0);
    pgresult_clear(result);
    return row;
}

/*
 * call-seq:
 *   conn.select_value(query, *bind_values)
 *
 * Return the first value of the first row of the query results.
 * Equivalent to conn.query(query, *bind_values).first.first
 */
static VALUE
pgconn_select_value(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    VALUE result = pgconn_exec(argc, argv, self);
    VALUE value = fetch_pgresult(self, get_pgresult(result), 0, 0);
    pgresult_clear(result);
    return value;
}

/*
 * call-seq:
 *   conn.select_values(query, *bind_values)
 *
 * Equivalent to conn.query(query, *bind_values).flatten
 */
static VALUE
pgconn_select_values(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    VALUE pg_result = pgconn_exec(argc, argv, self);
    PGresult * result = get_pgresult(pg_result);
    int ntuples = PQntuples(result);
    int nfields = PQnfields(result);

    VALUE values = rb_ary_new2(ntuples * nfields);
    int row_num, field_num;
    for (row_num = 0; row_num < ntuples; row_num++) {
      for (field_num = 0; field_num < nfields; field_num++) {
        rb_ary_push(values, fetch_pgresult(self, result, row_num, field_num));
      }
    }

    pgresult_clear(pg_result);
    return values;
}

/*
 * call-seq:
 *    res.each{ |tuple| ... }
 *
 * Invokes the block for each tuple (row) in the result.
 *
 * Equivalent to <tt>res.result.each{ |tuple| ... }</tt>.
 */
static VALUE
pgresult_each(self)
    VALUE self;
{
    PGresult *result = get_pgresult(self);
    int row_count = PQntuples(result);
    VALUE fields = pgresult_fields(self);

    int row_num;
    for (row_num = 0; row_num < row_count; row_num++) {
        VALUE row = fetch_pgrow(self, fields, row_num);
        rb_yield(row);
    }

    return self;
}

/*
 * call-seq:
 *    res[ n ]
 *
 * Returns the tuple (row) corresponding to _n_. Returns +nil+ if <tt>_n_ >= res.num_tuples</tt>.
 *
 * Equivalent to <tt>res.result[n]</tt>.
 */
static VALUE
pgresult_aref(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    PGresult *result;
    VALUE a1, a2, val;
    int i, j, nf, nt;

    result = get_pgresult(obj);
    nt = PQntuples(result);
    nf = PQnfields(result);
    switch (rb_scan_args(argc, argv, "11", &a1, &a2)) {
    case 1:
      i = NUM2INT(a1);
      if( i >= nt ) return Qnil;

      val = rb_ary_new();
      for (j=0; j<nf; j++) {
          VALUE value = fetch_pgresult(get_result_conn(obj), result, i, j);
          rb_ary_push(val, value);
      }
      return val;

    case 2:
      i = NUM2INT(a1);
      if( i >= nt ) return Qnil;
      j = NUM2INT(a2);
      if( j >= nf ) return Qnil;
      return fetch_pgresult(get_result_conn(obj), result, i, j);

    default:
      return Qnil;            /* not reached */
    }
}

/*
 * call-seq:
 *    res.fields()
 *
 * Returns an array of Strings representing the names of the fields in the result.
 *
 *   res=conn.exec("SELECT foo,bar AS biggles,jim,jam FROM mytable")
 *   res.fields => [ 'foo' , 'biggles' , 'jim' , 'jam' ]
 */
static VALUE
pgresult_fields(obj)
    VALUE obj;
{
    PGresult *result;
    VALUE ary;
    int n, i;

    result = get_pgresult(obj);
    n = PQnfields(result);
    ary = rb_ary_new2(n);
    for (i=0;i<n;i++) {
        rb_ary_push(ary, rb_tainted_str_new2(PQfname(result, i)));
    }
    return ary;
}

/*
 * call-seq:
 *    res.num_tuples()
 *
 * Returns the number of tuples (rows) in the query result.
 *
 * Similar to <tt>res.result.length</tt> (but faster).
 */
static VALUE
pgresult_num_tuples(obj)
    VALUE obj;
{
    int n;

    n = PQntuples(get_pgresult(obj));
    return INT2NUM(n);
}

/*
 * call-seq:
 *    res.num_fields()
 *
 * Returns the number of fields (columns) in the query result.
 *
 * Similar to <tt>res.result[0].length</tt> (but faster).
 */
static VALUE
pgresult_num_fields(obj)
    VALUE obj;
{
    int n;

    n = PQnfields(get_pgresult(obj));
    return INT2NUM(n);
}

/*
 * call-seq:
 *    res.fieldname( index )
 *
 * Returns the name of the field (column) corresponding to the index.
 *
 *   res=conn.exec("SELECT foo,bar AS biggles,jim,jam FROM mytable")
 *   puts res.fieldname(2) => 'jim'
 *   puts res.fieldname(1) => 'biggles'
 *
 * Equivalent to <tt>res.fields[_index_]</tt>.
 */
static VALUE
pgresult_fieldname(obj, index)
    VALUE obj, index;
{
    PGresult *result;
    int i = NUM2INT(index);
    char *name;

    result = get_pgresult(obj);
    if (i < 0 || i >= PQnfields(result)) {
        rb_raise(rb_eArgError,"invalid field number %d", i);
    }
    name = PQfname(result, i);
    return rb_tainted_str_new2(name);
}

/*
 * call-seq:
 *    res.fieldnum( name )
 *
 * Returns the index of the field specified by the string _name_.
 *
 *   res=conn.exec("SELECT foo,bar AS biggles,jim,jam FROM mytable")
 *   puts res.fieldnum('foo') => 0
 *
 * Raises an ArgumentError if the specified _name_ isn't one of the field names;
 * raises a TypeError if _name_ is not a String.
 */
static VALUE
pgresult_fieldnum(obj, name)
    VALUE obj, name;
{
    int n;
    
    Check_Type(name, T_STRING);
    
    n = PQfnumber(get_pgresult(obj), StringValuePtr(name));
    if (n == -1) {
        rb_raise(rb_eArgError,"Unknown field: %s", StringValuePtr(name));
    }
    return INT2NUM(n);
}

/*
 * call-seq:
 *    res.type( index )
 *
 * Returns the data type associated with the given column number.
 *
 * The integer returned is the internal +OID+ number (in PostgreSQL) of the type.
 * If you have the PostgreSQL source available, you can see the OIDs for every column type in the file <tt>src/include/catalog/pg_type.h</tt>.
 */
static VALUE
pgresult_type(obj, index)
    VALUE obj, index;
{
    PGresult* result = get_pgresult(obj);
    int i = NUM2INT(index);
    if (i < 0 || i >= PQnfields(result)) {
        rb_raise(rb_eArgError, "invalid field number %d", i);
    }
    return INT2NUM(PQftype(result, i));
}

/*
 * call-seq:
 *    res.size( index )
 *
 * Returns the size of the field type in bytes.  Returns <tt>-1</tt> if the field is variable sized.
 *
 *   res = conn.exec("SELECT myInt, myVarChar50 FROM foo")
 *   res.size(0) => 4
 *   res.size(1) => -1
 */
static VALUE
pgresult_size(obj, index)
    VALUE obj, index;
{
    PGresult *result;
    int i = NUM2INT(index);
    int size;

    result = get_pgresult(obj);
    if (i < 0 || i >= PQnfields(result)) {
        rb_raise(rb_eArgError,"invalid field number %d", i);
    }
    size = PQfsize(result, i);
    return INT2NUM(size);
}

/*
 * call-seq:
 *    res.value( tup_num, field_num )
 *
 * Returns the value in tuple number <i>tup_num</i>, field number <i>field_num</i>. (Row <i>tup_num</i>, column <i>field_num</i>.)
 *
 * Equivalent to <tt>res.result[<i>tup_num</i>][<i>field_num</i>]</tt> (but faster).
 */
static VALUE
pgresult_getvalue(obj, tup_num, field_num)
    VALUE obj, tup_num, field_num;
{
    PGresult *result;
    int i = NUM2INT(tup_num);
    int j = NUM2INT(field_num);

    result = get_pgresult(obj);
    if (i < 0 || i >= PQntuples(result)) {
        rb_raise(rb_eArgError,"invalid tuple number %d", i);
    }
    if (j < 0 || j >= PQnfields(result)) {
        rb_raise(rb_eArgError,"invalid field number %d", j);
    }

    return fetch_pgresult(get_result_conn(obj), result, i, j);
}


/*
 * call-seq:
 *    res.value_byname( tup_num, field_name )
 *
 * Returns the value in tuple number <i>tup_num</i>, for the field named <i>field_name</i>.
 *
 * Equivalent to (but faster than) either of:
 *    res.result[<i>tup_num</i>][ res.fieldnum(<i>field_name</i>) ]
 *    res.value( <i>tup_num</i>, res.fieldnum(<i>field_name</i>) )
 *
 * <i>(This method internally calls #value as like the second example above; it is slower than using the field index directly.)</i>
 */
static VALUE
pgresult_getvalue_byname(obj, tup_num, field_name)
    VALUE obj, tup_num, field_name;
{
    return pgresult_getvalue(obj, tup_num, pgresult_fieldnum(obj, field_name));
}


/*
 * call-seq:
 *    res.getlength( tup_num, field_num )
 *
 * Returns the (String) length of the field in bytes.
 *
 * Equivalent to <tt>res.value(<i>tup_num</i>,<i>field_num</i>).length</tt>.
 */
static VALUE
pgresult_getlength(obj, tup_num, field_num)
    VALUE obj, tup_num, field_num;
{
    PGresult *result;
    int i = NUM2INT(tup_num);
    int j = NUM2INT(field_num);

    result = get_pgresult(obj);
    if (i < 0 || i >= PQntuples(result)) {
        rb_raise(rb_eArgError,"invalid tuple number %d", i);
    }
    if (j < 0 || j >= PQnfields(result)) {
        rb_raise(rb_eArgError,"invalid field number %d", j);
    }
    return INT2FIX(PQgetlength(result, i, j));
}

/*
 * call-seq:
 *    res.getisnull(tuple_position, field_position) -> boolean
 *
 * Returns +true+ if the specified value is +nil+; +false+ otherwise.
 *
 * Equivalent to <tt>res.value(<i>tup_num</i>,<i>field_num</i>)==+nil+</tt>.
 */
static VALUE
pgresult_getisnull(obj, tup_num, field_num)
    VALUE obj, tup_num, field_num;
{
    PGresult *result;
    int i = NUM2INT(tup_num);
    int j = NUM2INT(field_num);

    result = get_pgresult(obj);
    if (i < 0 || i >= PQntuples(result)) {
        rb_raise(rb_eArgError,"invalid tuple number %d", i);
    }
    if (j < 0 || j >= PQnfields(result)) {
        rb_raise(rb_eArgError,"invalid field number %d", j);
    }
    return PQgetisnull(result, i, j) ? Qtrue : Qfalse;
}

/*
 * call-seq:
 *    res.print( file, opt )
 *
 * MISSING: Documentation
 */
static VALUE
pgresult_print(obj, file, opt)
    VALUE obj, file, opt;
{
    VALUE value;
    ID mem;
    OpenFile* fp;
    PQprintOpt po;

    Check_Type(file, T_FILE);
    Check_Type(opt,  T_STRUCT);
    GetOpenFile(file, fp);

    memset(&po, 0, sizeof(po));

    mem = rb_intern("header");
    value = rb_struct_getmember(opt, mem);
    po.header = value == Qtrue ? 1 : 0;

    mem = rb_intern("align");
    value = rb_struct_getmember(opt, mem);
    po.align = value == Qtrue ? 1 : 0;

    mem = rb_intern("standard");
    value = rb_struct_getmember(opt, mem);
    po.standard = value == Qtrue ? 1 : 0;

    mem = rb_intern("html3");
    value = rb_struct_getmember(opt, mem);
    po.html3 = value == Qtrue ? 1 : 0;

    mem = rb_intern("expanded");
    value = rb_struct_getmember(opt, mem);
    po.expanded = value == Qtrue ? 1 : 0;

    mem = rb_intern("pager");
    value = rb_struct_getmember(opt, mem);
    po.pager = value == Qtrue ? 1 : 0;

    mem = rb_intern("fieldSep");
    value = rb_struct_getmember(opt, mem);
    if (!NIL_P(value)) {
        Check_Type(value, T_STRING);
        po.fieldSep = StringValuePtr(value);
    }

    mem = rb_intern("tableOpt");
    value = rb_struct_getmember(opt, mem);
    if (!NIL_P(value)) {
        Check_Type(value, T_STRING);
        po.tableOpt = StringValuePtr(value);
    }

    mem = rb_intern("caption");
    value = rb_struct_getmember(opt, mem);
    if (!NIL_P(value)) {
        Check_Type(value, T_STRING);
        po.caption = StringValuePtr(value);
    }

    PQprint(fp->f2?fp->f2:fp->f, get_pgresult(obj), &po);
    return obj;
}

/*
 * call-seq:
 *    res.cmdtuples()
 *
 * Returns the number of tuples (rows) affected by the SQL command.
 *
 * If the SQL command that generated the PGresult was not one of +INSERT+, +UPDATE+, +DELETE+, +MOVE+, or +FETCH+, or if no tuples (rows) were affected, <tt>0</tt> is returned.
 */
static VALUE
pgresult_cmdtuples(obj)
    VALUE obj;
{
    long n;
    n = strtol(PQcmdTuples(get_pgresult(obj)),NULL, 10);
    return INT2NUM(n);
}
/*
 * call-seq:
 *    res.cmdstatus()
 *
 * Returns the status string of the last query command.
 */
static VALUE
pgresult_cmdstatus(obj)
    VALUE obj;
{
    return rb_tainted_str_new2(PQcmdStatus(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.oid()
 *
 * Returns the +oid+.
 */
static VALUE
pgresult_oid(obj)
    VALUE obj;
{
    Oid n = PQoidValue(get_pgresult(obj));
    if (n == InvalidOid)
        return Qnil;
    else
        return INT2NUM(n);
}

/*
 * call-seq:
 *    res.clear()
 *
 * Clears the PGresult object as the result of the query.
 */
static VALUE
pgresult_clear(obj)
    VALUE obj;
{
    PQclear(get_pgresult(obj));
    DATA_PTR(obj) = 0;

    return Qnil;
}

static VALUE
pgresult_result_with_clear(self)
    VALUE self;
{
    VALUE rows = rb_funcall(self, rb_intern("rows"), 0);
    pgresult_clear(self);
    return rows;
}

/* Large Object support */
static PGlarge*
get_pglarge(obj)
    VALUE obj;
{
    PGlarge *pglarge;
    Data_Get_Struct(obj, PGlarge, pglarge);
    if (pglarge == NULL) rb_raise(rb_ePGError, "invalid large object");
    return pglarge;
}

/*
 * call-seq:
 *    conn.lo_import(file) -> PGlarge
 *
 * Import a file to a large object. Returns a PGlarge instance on success. On failure, it raises a PGError exception.
 */
static VALUE
pgconn_loimport(obj, filename)
    VALUE obj, filename;
{
    Oid lo_oid;

    PGconn *conn = get_pgconn(obj);

    Check_Type(filename, T_STRING);

    lo_oid = lo_import(conn, StringValuePtr(filename));
    if (lo_oid == 0) {
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }
    return pglarge_new(conn, lo_oid, -1);
}

/*
 * call-seq:
 *    conn.lo_export( oid, file )
 *
 * Saves a large object of _oid_ to a _file_.
 */
static VALUE
pgconn_loexport(obj, lo_oid,filename)
    VALUE obj, lo_oid, filename;
{
    PGconn *conn = get_pgconn(obj);
    int oid;
    Check_Type(filename, T_STRING);

    oid = NUM2INT(lo_oid);
    if (oid < 0) {
        rb_raise(rb_ePGError, "invalid large object oid %d",oid);
    }

    if (!lo_export(conn, oid, StringValuePtr(filename))) {
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }
    return Qnil;
}

/*
 * call-seq:
 *    conn.lo_create( [mode] ) -> PGlarge
 *
 * Returns a PGlarge instance on success. On failure, it raises PGError exception.
 * <i>(See #lo_open for information on _mode_.)</i>
 */
static VALUE
pgconn_locreate(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    Oid lo_oid;
    int mode;
    VALUE nmode;
    PGconn *conn;
    
    if (rb_scan_args(argc, argv, "01", &nmode) == 0) {
        mode = INV_READ;
    }
    else {
        mode = FIX2INT(nmode);
    }
  
    conn = get_pgconn(obj);
    lo_oid = lo_creat(conn, mode);
    if (lo_oid == 0){
        rb_raise(rb_ePGError, "can't creat large object");
    }

    return pglarge_new(conn, lo_oid, -1);
}

/*
 * call-seq:
 *    conn.lo_open( oid, [mode] ) -> PGlarge
 *
 * Open a large object of _oid_. Returns a PGlarge instance on success.
 * The _mode_ argument specifies the mode for the opened large object,
 * which is either +INV_READ+, or +INV_WRITE+.
 * * If _mode_ On failure, it raises a PGError exception.
 * * If _mode_ is omitted, the default is +INV_READ+.
 */
static VALUE
pgconn_loopen(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    Oid lo_oid;
    int fd, mode;
    VALUE nmode, objid;
    PGconn *conn = get_pgconn(obj);

    switch (rb_scan_args(argc, argv, "02", &objid, &nmode)) {
    case 1:
      lo_oid = NUM2INT(objid);
      mode = INV_READ;
      break;
    case 2:
      lo_oid = NUM2INT(objid);
      mode = FIX2INT(nmode);
      break;
    default:
      mode = INV_READ;
      lo_oid = lo_creat(conn, mode);
      if (lo_oid == 0){
          rb_raise(rb_ePGError, "can't creat large object");
      }
    }
    if((fd = lo_open(conn, lo_oid, mode)) < 0) {
        rb_raise(rb_ePGError, "can't open large object");
    }
    return pglarge_new(conn, lo_oid, fd);
}

/*
 * call-seq:
 *    conn.lo_unlink( oid )
 *
 * Unlinks (deletes) the postgres large object of _oid_.
 */
static VALUE
pgconn_lounlink(obj, lo_oid)
    VALUE obj, lo_oid;
{
    PGconn *conn;
    int oid = NUM2INT(lo_oid);
    int result;
    
    if (oid < 0){
        rb_raise(rb_ePGError, "invalid oid %d",oid);
    }
    conn = get_pgconn(obj);
    result = lo_unlink(conn,oid);

    return Qnil;
}

static void
free_pglarge(ptr)
    PGlarge *ptr;
{
    if ((ptr->lo_fd) > 0) {
        lo_close(ptr->pgconn,ptr->lo_fd);
    }
    free(ptr);
}

static VALUE
pglarge_new(conn, lo_oid ,lo_fd)
    PGconn *conn;
    Oid lo_oid;
    int lo_fd;
{
    VALUE obj;
    PGlarge *pglarge;

    obj = Data_Make_Struct(rb_cPGlarge, PGlarge, 0, free_pglarge, pglarge);
    pglarge->pgconn = conn;
    pglarge->lo_oid = lo_oid;
    pglarge->lo_fd = lo_fd;

    return obj;
}

/*
 * call-seq:
 *    lrg.oid()
 *
 * Returns the large object's +oid+.
 */
static VALUE
pglarge_oid(obj)
    VALUE obj;
{
    PGlarge *pglarge = get_pglarge(obj);

    return INT2NUM(pglarge->lo_oid);
}

/*
 * call-seq:
 *    lrg.open( [mode] )
 *
 * Opens a large object.
 * The _mode_ argument specifies the mode for the opened large object,
 * which is either +INV_READ+ or +INV_WRITE+.
 */
static VALUE
pglarge_open(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    PGlarge *pglarge = get_pglarge(obj);
    VALUE nmode;
    int fd;
    int mode;

    if (rb_scan_args(argc, argv, "01", &nmode) == 0) {
        mode = INV_READ;
    }
    else {
        mode = FIX2INT(nmode);
    }
  
    if((fd = lo_open(pglarge->pgconn, pglarge->lo_oid, mode)) < 0) {
        rb_raise(rb_ePGError, "can't open large object");
    }
    pglarge->lo_fd = fd;

    return INT2FIX(pglarge->lo_fd);
}

/*
 * call-seq:
 *    lrg.close()
 *
 * Closes a large object. Closed when they are garbage-collected.
 */
static VALUE
pglarge_close(obj)
    VALUE obj;
{
    PGlarge *pglarge = get_pglarge(obj);

    if((lo_close(pglarge->pgconn, pglarge->lo_fd)) < 0) {
        rb_raise(rb_ePGError, "can't closed large object");
    }
    DATA_PTR(obj) = 0;
  
    return Qnil;
}

/*
 * call-seq:
 *    lrg.tell()
 *
 * Returns the current position of the large object pointer.
 */
static VALUE
pglarge_tell(obj)
    VALUE obj;
{
    int start;
    PGlarge *pglarge = get_pglarge(obj);

    if ((start = lo_tell(pglarge->pgconn,pglarge->lo_fd)) == -1) {
        rb_raise(rb_ePGError, "error while getting position");
    }
    return INT2NUM(start);
}

static VALUE
loread_all(obj)
    VALUE obj;
{
    PGlarge *pglarge = get_pglarge(obj);
    VALUE str;
    long siz = BUFSIZ;
    long bytes = 0;
    int n;

    str = rb_tainted_str_new(0,siz);
    for (;;) {
        n = lo_read(pglarge->pgconn, pglarge->lo_fd, RSTRING(str)->ptr + bytes,siz - bytes);
        if (n == 0 && bytes == 0) return Qnil;
        bytes += n;
        if (bytes < siz ) break;
        siz += BUFSIZ;
        rb_str_resize(str,siz);
    }
    if (bytes == 0) return rb_tainted_str_new(0,0);
    if (bytes != siz) rb_str_resize(str, bytes);
    return str;
}

/*
 * call-seq:
 *    lrg.read( [length] )
 *
 * Attempts to read _length_ bytes from large object.
 * If no _length_ is given, reads all data.
 */
static VALUE
pglarge_read(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    int len;
    PGlarge *pglarge = get_pglarge(obj);
    VALUE str;
    VALUE length;
    
    rb_scan_args(argc, argv, "01", &length);
    if (NIL_P(length)) {
        return loread_all(obj);
    }
    
    len = NUM2INT(length);
    if (len < 0){
        rb_raise(rb_ePGError,"nagative length %d given", len);
    }
    str = rb_tainted_str_new(0,len);

    if((len = lo_read(pglarge->pgconn, pglarge->lo_fd, StringValuePtr(str), len)) < 0) {
        rb_raise(rb_ePGError, "error while reading");
    }
    if (len == 0) return Qnil;
    RSTRING(str)->len = len;
    return str;
}

/*
 * call-seq:
 *    lrg.write( str )
 *
 * Writes the string _str_ to the large object.
 * Returns the number of bytes written.
 */
static VALUE
pglarge_write(obj, buffer)
    VALUE obj, buffer;
{
    int n;
    PGlarge *pglarge = get_pglarge(obj);

    Check_Type(buffer, T_STRING);

    if( RSTRING(buffer)->len < 0) {
        rb_raise(rb_ePGError, "write buffer zero string");
    }
    if((n = lo_write(pglarge->pgconn, pglarge->lo_fd, StringValuePtr(buffer), RSTRING(buffer)->len)) == -1) {
        rb_raise(rb_ePGError, "buffer truncated during write");
    }
  
    return INT2FIX(n);
}

/*
 * call-seq:
 *    lrg.seek( offset, whence )
 *
 * Move the large object pointer to the _offset_.
 * Valid values for _whence_ are +SEEK_SET+, +SEEK_CUR+, and +SEEK_END+.
 * (Or 0, 1, or 2.)
 */
static VALUE
pglarge_seek(obj, offset, whence)
    VALUE obj, offset, whence;
{
    PGlarge *pglarge = get_pglarge(obj);
    int ret;
    
    if((ret = lo_lseek(pglarge->pgconn, pglarge->lo_fd, NUM2INT(offset), NUM2INT(whence))) == -1) {
        rb_raise(rb_ePGError, "error while moving cursor");
    }

    return INT2NUM(ret);
}

/*
 * call-seq:
 *    lrg.size()
 *
 * Returns the size of the large object.
 */
static VALUE
pglarge_size(obj)
    VALUE obj;
{
    PGlarge *pglarge = get_pglarge(obj);
    int start, end;

    if ((start = lo_tell(pglarge->pgconn,pglarge->lo_fd)) == -1) {
        rb_raise(rb_ePGError, "error while getting position");
    }

    if ((end = lo_lseek(pglarge->pgconn, pglarge->lo_fd, 0, SEEK_END)) == -1) {
        rb_raise(rb_ePGError, "error while moving cursor");
    }

    if ((start = lo_lseek(pglarge->pgconn, pglarge->lo_fd,start, SEEK_SET)) == -1) {
        rb_raise(rb_ePGError, "error while moving back to posiion");
    }
        
    return INT2NUM(end);
}
    
/*
 * call-seq:
 *    lrg.export( file )
 *
 * Saves the large object to a file.
 */
static VALUE
pglarge_export(obj, filename)
    VALUE obj, filename;
{
    PGlarge *pglarge = get_pglarge(obj);

    Check_Type(filename, T_STRING);

    if (!lo_export(pglarge->pgconn, pglarge->lo_oid, StringValuePtr(filename))){
        rb_raise(rb_ePGError, PQerrorMessage(pglarge->pgconn));
    }

    return Qnil;
}

/*
 * call-seq:
 *    lrg.unlink()
 *
 * Deletes the large object.
 */
static VALUE
pglarge_unlink(obj)
    VALUE obj;
{
    PGlarge *pglarge = get_pglarge(obj);

    if (!lo_unlink(pglarge->pgconn,pglarge->lo_oid)) {
        rb_raise(rb_ePGError, PQerrorMessage(pglarge->pgconn));
    }
    DATA_PTR(obj) = 0;

    return Qnil;
}

static VALUE
pgrow_init(self, keys)
    VALUE self, keys;
{
    VALUE args[1] = { LONG2NUM(RARRAY(keys)->len) };
    rb_call_super(1, args);
    rb_iv_set(self, "@keys", keys);
    return self;
}

/*
 * call-seq:
 *   row.keys -> Array
 *
 * Column names.
 */
static VALUE
pgrow_keys(self)
    VALUE self;
{
    return rb_iv_get(self, "@keys");
}

/*
 * call-seq:
 *   row.values -> row
 */
static VALUE
pgrow_values(self)
    VALUE self;
{
    return self;
}

/*
 * call-seq:
 *   row[position] -> value
 *   row[name] -> value
 *
 * Access elements of this row by column position or name.
 * 
 * Raises an ArgumentError if the specified _name_ isn't one of the keys of row.
 */
static VALUE
pgrow_aref(argc, argv, self)
    int argc;
    VALUE * argv;
    VALUE self;
{
    if (TYPE(argv[0]) == T_STRING) {
        VALUE keys = pgrow_keys(self);
        VALUE index = rb_funcall(keys, rb_intern("index"), 1, argv[0]);
        if (index == Qnil) {
            rb_raise(rb_ePGError, "%s: field not found", StringValuePtr(argv[0]));
            return Qnil; /* to reduce compiler complains */
        }
        else {
            return rb_ary_entry(self, NUM2INT(index));
        }
    }
    else {
        return rb_call_super(argc, argv);
    }
}

/*
 * call-seq:
 *   row.each_value { |value| block } -> row
 *
 * Iterate with values.
 */
static VALUE
pgrow_each_value(self)
    VALUE self;
{
    rb_ary_each(self);
    return self;
}

/*
 * call-seq:
 *   row.each_pair { |column_value_array| block } -> row
 *
 * Iterate with column,value pairs.
 */
static VALUE
pgrow_each_pair(self)
    VALUE self;
{
    VALUE keys = pgrow_keys(self);
    int i;
    for (i = 0; i < RARRAY(keys)->len; ++i) {
        rb_yield(rb_assoc_new(rb_ary_entry(keys, i), rb_ary_entry(self, i)));
    }
    return self;
}

/*
 * call-seq:
 *   row.each { |column, value| block } -> row
 *   row.each { |value| block } -> row
 *
 * Iterate with values or column,value pairs.
 */
static VALUE
pgrow_each(self)
    VALUE self;
{
    int arity = NUM2INT(rb_funcall(rb_block_proc(), rb_intern("arity"), 0));
    if (arity == 2) {
        pgrow_each_pair(self);
    }
    else {
        pgrow_each_value(self);
    }
    return self;
}

/*
 * call-seq:
 *   row.each_key { |column| block } -> row
 *
 * Iterate with column names.
 */
static VALUE
pgrow_each_key(self)
    VALUE self;
{
    rb_ary_each(pgrow_keys(self));
    return self;
}

/*
 * call-seq:
 *   row.to_hash -> Hash
 *
 * Returns a +Hash+ of the row's values indexed by column name.
 * Equivalent to <tt>Hash [*row.keys.zip(row).flatten]</tt>
 */
static VALUE
pgrow_to_hash(self)
    VALUE self;
{
    VALUE result = rb_hash_new();
    VALUE keys = pgrow_keys(self);
    int i;
    for (i = 0; i < RARRAY(self)->len; ++i) {
        rb_hash_aset(result, rb_ary_entry(keys, i), rb_ary_entry(self, i));
    }
    return result;
}

/*
 * checks if result is not null and result status is valid.
 * returns NULL if OK, message string, if errors.
 * */
static char *
raise_error(PGconn * conn, PGresult * result)
{
  char *msg;
  if(result == NULL){
    return PQerrorMessage(conn);
  }
  switch (PQresultStatus(result)) {
    case PGRES_TUPLES_OK:
    case PGRES_COPY_OUT:
    case PGRES_COPY_IN:
    case PGRES_EMPTY_QUERY:
    case PGRES_COMMAND_OK:      
        return NULL;    
    case PGRES_BAD_RESPONSE:
    case PGRES_FATAL_ERROR:
    case PGRES_NONFATAL_ERROR:
        msg = RSTRING(rb_str_new2(PQresultErrorMessage(result)))->ptr;
        break;
    default:
        msg = "internal error : unknown result status.";
        break;
  }
  return msg;
}
#ifdef HAVE_PQEXECPARAMS
/*
 * call-seq: 
 *     conn.prepare(stmt_name, query)
 *
 * Creates prepared named  SQL statement. Raises PGerror exception on failure.
 * Prepared statements can be removed via SQL DEALLOCATE command.
 */
static VALUE 
pgconn_prepare(self, name, stmt)
  VALUE self, name, stmt;
{
  PGconn *conn;
  PGresult * result;
  char * msg;
  conn = get_pgconn(self);
  result = PQprepare(conn, StringValuePtr(name), StringValuePtr(stmt), 0, NULL);
  if( (msg = raise_error(conn, result)) ) {
    if(result) PQclear(result);
    rb_raise(rb_ePGError, msg);
    return Qnil;
  }
  PQclear(result);
  return Qtrue;
}
/*
 * call-seq:
 *    conn.exec_prepared(statement_name, *bind_values)
 *
 * Execute prepared named statement specified by _statement_name_.
 * Returns a PGresult instance on success.
 * On failure, it raises a PGError exception.
 *
 * +bind_values+ represents values for the PostgreSQL bind parameters found in the prepared statement.  
 * PostgreSQL bind parameters are presented as $1, $1, $2, etc.
 */
static VALUE
pgconn_exec_prepared(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result = NULL;
    VALUE command, params, c_reslt;
    char * msg;

    rb_scan_args(argc, argv, "1*", &command, &params);

    Check_Type(command, T_STRING);

    if (RARRAY(params)->len <= 0) {
        result = PQexecPrepared(conn, StringValuePtr(command), 0, NULL, NULL, NULL, 0);
    }
    else {
        int len = RARRAY(params)->len;
        int i;
        VALUE* ptr = RARRAY(params)->ptr;
        const char ** values = ALLOCA_N(const char *, len);
        int  * p_lengths = ALLOCA_N( int , len);
        int  * p_formats   = ALLOCA_N( int , len);
        VALUE formatted;
        for (i = 0; i < len; i++, ptr++) {
            if (*ptr == Qnil) {
                values[i] = NULL;
                p_lengths[i] = 0;
                p_formats[i] = 0;
            } else if (CLASS_OF(*ptr) == rb_cPGbytea) {
                values[i] = StringValuePtr(*ptr);
                p_lengths[i] = RSTRING(*ptr)->len;
                p_formats[i] = 1;
            } else {
                formatted = pgconn_s_format(obj, *ptr);
                values[i] = StringValuePtr(formatted);
                p_lengths[i] = RSTRING(formatted)->len;
                p_formats[i] = 0;
            }
        }
        result = PQexecPrepared(conn, StringValuePtr(command), len,  values, p_lengths, p_formats, 0);
   }

    if((msg = raise_error(conn, result))) {
      if(result) PQclear(result);
      rb_raise(rb_ePGError, msg);
      return Qnil;
    }
    c_reslt =pgresult_new(result );
    rb_iv_set(c_reslt, "@connection", obj);
    return c_reslt;
}

#endif
/* --------------------------*/
/*  COMPOSITE TYPES SUPPORT  */
/* --------------------------*/

/*
 * call-seq:
 *     PGfield.new(name, type_oid, type_mod, type_name)
 *
 */
static VALUE
pg_attribute_init( self,  attname,  typeoid,  typmod,  typname)
   VALUE self, attname, typeoid, typmod, typname;
{
  rb_iv_set(self, "@name", attname);
  rb_iv_set(self, "@type_oid", typeoid);
  rb_iv_set(self, "@type_mod", typmod);
  rb_iv_set(self, "@type_name", typname);
  return self;
}

static char * attribute_query = "select attname, atttypid::int, atttypmod, format_type(atttypid, atttypmod) as type_name "
    "from pg_attribute where attrelid = "
    "(select typrelid from pg_type where oid = %d ) and not attisdropped and attnum > 0 "
    "order by attnum";

/*
 * call-seq: 
 *     PGrowtype.new(conn, type_oid, type_name)
 *
 * Creates new instance of PGrowtype, loading data from database.
 * */    
static VALUE 
pg_composite_init(self, obj, tip, type_name)
  VALUE self, obj, tip, type_name;
{
   VALUE attrs, attnames;
   int i;
   PGresult * result;
   PGconn * conn;
   char * msg;
   conn = get_pgconn(obj);
   char * attribute_patched_query = ALLOCA_N(char, strlen(attribute_query)+20);
   sprintf(attribute_patched_query, attribute_query, NUM2INT(tip));
   result = PQexec(conn, attribute_patched_query);
   if( (msg = raise_error(conn, result)) )
   {
      if(result) PQclear(result);
      rb_raise(rb_ePGError, msg);
      return self;
   }
   int ntuples = PQntuples(result);
   attrs = rb_ary_new2(ntuples);
   attnames = rb_ary_new2(ntuples);
   for(i = 0;i< ntuples; i++) 
   {  //attname,  typeoid,  typmod,  typname)
      VALUE param2[4] = { 
          rb_str_new2(PQgetvalue(result, i, 0)),     // attname
          rb_cstr2inum(PQgetvalue(result,i, 1), 10), // typeoid
          rb_cstr2inum(PQgetvalue(result,i, 2), 10), // typmod
          rb_str_new2(PQgetvalue(result, i, 3)) };   // type_name
      rb_ary_push(attrs, rb_class_new_instance(4, param2, rb_cPGfield) );
      rb_ary_push(attnames, param2[0]);
   }
   PQclear(result);
   rb_iv_set(self, "@attributes", attrs);
   rb_iv_set(self, "@attnames", attnames);
   rb_iv_set(self, "@name", type_name);
   rb_iv_set(self, "@oid", tip);
   return self;
}

/*
 * call-seq:
 *     composite_type.index(field_name)
 * 
 * Returns index of given field in record.
 */
static VALUE
pg_composite_index(self, obj)
  VALUE self, obj;
{
  return rb_funcall(rb_iv_get(self, "@attnames"), rb_intern("index"),1, obj);
}


/*
 * call-seq:
 *     conn.load_composite_types
 *
 * Creates cache of PGrowtype objects - definitions of composite PostgreSQL types.
 * 
 * Cached types are retrieved from @cached_types attribute by OID.
 */
static VALUE 
pgconn_load_composite_types(self) 
    VALUE self;
{
  VALUE cached_types = rb_hash_new();
  int i;
  PGresult * result;
  PGconn * conn = get_pgconn(self);
  char * msg;

  rb_iv_set(self, "@loaded_composite_types", Qfalse);
  rb_iv_set(self, "@cached_types", cached_types);
  result = PQexec(conn, "select t.oid, format_type(t.oid, typtypmod)   from pg_type t where typtype = 'c'");
  if( (msg = raise_error(conn, result)) ) {
     if (result) PQclear(result);
     rb_raise(rb_ePGError, msg);
     return Qnil;
  }
  int ntuples = PQntuples(result);
  VALUE ** prepared_params = ALLOCA_N(VALUE *, ntuples+1);
  for(i = 0; i < ntuples ; i++) {
      VALUE * params2 = ALLOCA_N(VALUE, 4);
      params2[0] = self;  // conn
      params2[1] = rb_cstr2inum(PQgetvalue(result, i, 0), 10); // type OID
      params2[2] = rb_str_new2(PQgetvalue(result, i, 1)); // type name
      prepared_params[i] = params2;
  }
  PQclear(result);
  for(i = 0; i < ntuples; i++) {
      VALUE c_type = rb_class_new_instance(3, prepared_params[i], rb_cPGrowtype);
      rb_hash_aset(cached_types, prepared_params[i][1], c_type );
  }
  rb_iv_set(self, "@loaded_composite_types", Qtrue);
  rb_obj_freeze(cached_types);
  return Qnil;
}

/* 
 * call-seq:
 *     PGrecord.new( record_type, field_values)
 *
 */
static VALUE 
pg_composite_value_init(self, c_type, values)
    VALUE self, c_type, values;
{
   rb_iv_set(self, "@record_type", c_type);
   rb_iv_set(self, "@values", values);
   return self;
}
/* 
 * call-seq:
 *     conn.compose(type_name, values) -> PGrecord
 *
 * Returns instance of PGrecord, for given PostgreSQL typename,
 * and initializes field from given values. Values parameter can be 
 * anything, that support [] method.
 *
 * Raises PGError if there is no such type in DB or type is not in cache.
 *
 *     item = conn.compose("inventory_item2", { :name => "fuzzy dice", :suppliers => [42, 43], :prices => [1.99, 1.75]} )
 */
static VALUE
pgconn_compose(self, typename, values)
    VALUE self, typename, values;
{
  Check_Type(typename, T_STRING);
  VALUE oid_s =  pgconn_find_type_oid(self, typename);
  if(Qnil == oid_s)
    rb_raise(rb_ePGError, "Type '%s' not found database! ", StringValuePtr(typename));
  VALUE comp_type = find_composite_type( self, oid_s);
  if(Qnil == comp_type)
    rb_raise(rb_ePGError, "Type '%s' not found in cache! ", StringValuePtr(typename));
  VALUE attnames = rb_iv_get(comp_type, "@attnames");
  int n = RARRAY(attnames)->len;
  int i;
  VALUE new_values = rb_ary_new2(n);
  for(i = 0; i < n ; i++) {
     VALUE val = rb_funcall(values, rb_intern("[]"), 1, rb_ary_entry(attnames, i)); 
     if(Qnil == val) // trying to resolve as symbol
        val =  rb_funcall(values, rb_intern("[]"), 1, rb_str_intern(rb_ary_entry(attnames, i))); 
     rb_ary_push(new_values, val);
  }
  VALUE params[2] =  {comp_type, new_values};
  return rb_class_new_instance(2, params, rb_cPGrecord);
}    
/*
 * call-seq:
 *     record.names()
 *
 * Returns names of attributes.
 * */
static VALUE 
pg_composite_value_names(self)
  VALUE self;
{
   return rb_iv_get(rb_iv_get(self, "@record_type"), "@attnames");
}

/*
 * call-seq:
 *     record.index(attname)
 *
 * returns index of given attribute name.
 * */
static long
pgcv_get_index(obj, name)
    VALUE obj, name;
{
  return NUM2INT(rb_funcall(rb_iv_get(obj, "@record_type"), rb_intern("index"), 1, name));
}

/*
 * call-seq:
 *     record[name] -> attribute value
 *
 * Accessor method - returns value of record field, eg.
 *     name['first'] # => 'John'
 * */
static VALUE 
pg_composite_value_aref(self, name)
    VALUE self, name;
{
  return rb_ary_entry(rb_iv_get(self, "@values"), pgcv_get_index(self, name) );
}

/*
 * call-seq:
 *     record[name]= new_value
 *
 * Mutator method - sets record field to new value. 
 *
 *     name['first'] = 'Jack'
 * */
static VALUE 
pg_composite_value_aset(self, name, value)
  VALUE self, name, value;
{
  return rb_funcall(rb_iv_get(self, "@values"), rb_intern("[]="), 2, INT2NUM(pgcv_get_index(self, name)), value);
}

/*
 * call-seq:
 *    record1 == record2
 *
 * Compares for equality two composite records.
 * Records are considered equal if they have the same record type and
 * all their fields are equal.
 * */
static VALUE
pg_composite_value_equal(self, obj)
  VALUE self, obj;
{
  if(! rb_equal(rb_iv_get(self, "@record_type"), rb_iv_get(obj, "@record_type")) )
    return Qfalse;
  if( rb_equal(rb_iv_get(self, "@values"), rb_iv_get(obj, "@values")) )
    return Qtrue;
  else
    return Qfalse;
}

/*
 * overloaded to allow more record-like access to components of composite record,
 * if method begins with '_' (underscore) and there is such field, accessor and
 * mutator methods are emulated:
 *
 *    # having composite type EMAIL_ADDRESS(account VARCHAR(200), network VARCHAR(200))
 *    # accessor methods
 *    email._account # => 'username'
 *    # mutator
 *    email._network  = 'example.com'
 * */
static VALUE
pg_composite_value_method_missing(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
   VALUE idx;
   char * method_name = rb_id2name(rb_to_id(argv[0])) ;
   VALUE mname = rb_str_new2(method_name);
   if( *method_name == '_') {
      ++method_name;
      if (argc == 1) {
         mname = rb_str_new2(method_name);
         idx = pg_composite_index(rb_iv_get(self, "@record_type"), mname);
         if (idx != Qnil) {
           return pg_composite_value_aref(self, mname);
         }
      }
      if (argc == 2) {
         int len = strlen(method_name);
         if( method_name[len-1] == '=' ) {
            mname = rb_str_new(method_name, len-1);
            idx = pg_composite_index(rb_iv_get(self, "@record_type"), mname );
            if (idx != Qnil) {
               return pg_composite_value_aset(self, mname, argv[1]);
            }
         }
      }
   }
   rb_raise(rb_eNoMethodError, method_name);
   return Qnil;
}
/********************************************************************
 * 
 * Document-class: PGconn
 *
 * The class to access PostgreSQL database, based on libpq[http://www.postgresql.org/docs/current/interactive/libpq.html]
 * interface, provides convenient OO methods to query database and means for
 * transparent translation of query results (including PostgreSQL arrays and composite types) to 
 * appropriate ruby class values and vice versa.
 *
 * For example, to send query to the database on the localhost:
 *    require 'pg'
 *    conn = PGconn.open('dbname' => 'test1')
 *    res  = conn.exec('select * from a')
 *
 * See the PGresult class for information on working with the results of a query.
 *
 * ------------------------
 * ==Functions overview
 *
 * 1. Connection Control functions:
 *    #new (aliases: #connect , #open, #setdb, #setdblogin)
 *    #close (alias: #finish )
 *    #reset
 *    
 *    #trace
 *    #untrace
 *    #set_client_encoding
 *
 * 2. Connection Info Methods:  
 *    #db
 *    #host
 *    #user
 *    #options
 *    #port
 *    #tty
 *    #protocol_version  
 *    #server_version
 *    
 *    #status
 *    #error
 *    #transaction_status
 *    #client_encoding
 *
 * 3. Query functions:
 *    #exec
 *    #query
 *    #lastval
 *    
 *    #async_exec
 *    #async_query
 *    
 *    #get_notify
 *    #on_notice
 *    
 *    #insert_table
 *    #putline
 *    #getline
 *    #endcopy
 *    
 *    classes: PGresult, PGrow
 *    
 * 4. Prepared statements:
 *    #prepare
 *    #exec_prepared
 *
 * 5. Value translation and formatting:
 *    * Generic:
 *      #escape, #quote (alias: #format), #parse 
 *      
 *    * BYTEA support
 *      #escape_bytea, #unescape_bytea, 
 *
 *      class PGbytea
 *      
 *    * Value translation:
 *      #translate_results=
 *      #parse_arrays=
 * 
 *    * Custom value translation and formatting:
 *      #reg_translate
 *      #reg_array
 *      #reg_format
 *
 *    * Composite Types support:     
 *      #load_composite_types
 *      #get_composite_type
 *      #compose
 *      
 *      classes: PGrowtype, PGfield, PGrecord
 *
 * 6. Large Object support:
 *    #lo_import
 *    #lo_create
 *    #lo_open 
 *    #lo_export
 *    #lo_unlink 
 *    class PGlarge
 */


/********************************************************************
 * 
 * Document-class: PGresult
 *
 * The class to represent the query result tuples (rows). 
 * An instance of this class is created as the result of every query.
 * You may need to invoke the #clear method of the instance when finished with
 * the result for better memory performance.
 */


/********************************************************************
 * 
 * Document-class: PGrow
 *
 * Array subclass that provides hash-like behavior.
 */


/********************************************************************
 * 
 * Document-class: PGlarge
 *
 * The class to access large objects.
 * An instance of this class is created as the  result of
 * PGconn#lo_import, PGconn#lo_create, and PGconn#lo_open.
 */

/********************************************************************
 * 
 * Document-class: PGfield
 *
 * The class to store information about composite type field
 * Instances of this class are created during creation of PGrowtype instance.
 */

/********************************************************************
 * 
 * Document-class: PGrowtype
 *
 * The class to store information about composite type structure.
 * A composite type describes the structure of a row or record; 
 * it is in essence just a list of field names and their data types. 
 * PostgreSQL allows values of composite types to be used in many of 
 * the same ways that simple types can be used. For example, a column 
 * of a table can be declared to be of a composite type.
 * See [http://www.postgresql.org/docs/current/interactive/rowtypes.html] for details
 * 
 * Instances of this class are created during PGconn.load_composite_types
 */

/********************************************************************
 * 
 * Document-class: PGrecord
 *
 * The class to store value of PostgreSQL composite type (record types).
 * Instances of this class are created automatically when translating results
 * and there is type definition (PGrowtype) in cache (loaded during PGconn.load_composite_types),
 * or via PGconn.compose method.
 */

/********************************************************************
 *
 * Document-class: PGbytea
 *
 * XXX: The class to store binary value of BYTEA field. Essentially, PGbytea is method-less ancestor
 * of String, but introducing special class is useful to avoid escaping when 
 * using query with parameters.
 *
 */
void
Init_pg()
{
    pg_gsub_bang_id = rb_intern("gsub!");
    pg_escape_regex = rb_reg_new("([\\t\\n\\\\])", 10, 0);
    rb_global_variable(&pg_escape_regex);
    pg_escape_str = rb_str_new("\\\\\\1", 4);
    rb_global_variable(&pg_escape_str);

    rb_require("bigdecimal");
    rb_require("date");
    rb_require("time");
    rb_cBigDecimal = RUBY_CLASS("BigDecimal");
    rb_cDate = RUBY_CLASS("Date");
    rb_cDateTime = RUBY_CLASS("DateTime");

    rb_ePGError = rb_define_class("PGError", rb_eStandardError);
    rb_define_alias(rb_ePGError, "error", "message");

    rb_cPGconn = rb_define_class("PGconn", rb_cObject);
#ifdef HAVE_RB_DEFINE_ALLOC_FUNC
    rb_define_alloc_func(rb_cPGconn, pgconn_alloc);
#else
    rb_define_singleton_method(rb_cPGconn, "new", pgconn_s_new, -1);
#endif  
    rb_define_singleton_alias(rb_cPGconn, "connect", "new");
    rb_define_singleton_alias(rb_cPGconn, "open", "connect");
    rb_define_singleton_alias(rb_cPGconn, "setdb", "connect");
    rb_define_singleton_alias(rb_cPGconn, "setdblogin", "connect");
    rb_define_singleton_alias(rb_cPGconn, "open", "connect");
    rb_define_singleton_method(rb_cPGconn, "escape", pgconn_s_escape, 1);
    rb_define_singleton_method(rb_cPGconn, "quote", pgconn_s_quote, 1);
    rb_define_singleton_alias(rb_cPGconn, "format", "quote");
    rb_define_singleton_method(rb_cPGconn, "escape_bytea", pgconn_s_escape_bytea, 1);
    rb_define_singleton_method(rb_cPGconn, "unescape_bytea", pgconn_s_unescape_bytea, 1);
    rb_define_singleton_method(rb_cPGconn, "translate_results=", pgconn_s_translate_results_set, 1);
    rb_define_singleton_method(rb_cPGconn, "quote_ident", pgconn_s_quote_ident, 1);
    rb_define_singleton_method(rb_cPGconn, "parse_arrays=", pgconn_s_parse_arrays_set, 1);
    rb_define_singleton_method(rb_cPGconn, "reg_format", pgconn_reg_format, 3);
    rb_define_class_variable(rb_cPGconn, "@@formatters", rb_hash_new());

    rb_define_const(rb_cPGconn, "CONNECTION_OK", INT2FIX(CONNECTION_OK));
    rb_define_const(rb_cPGconn, "CONNECTION_BAD", INT2FIX(CONNECTION_BAD));

    rb_define_method(rb_cPGconn, "initialize", pgconn_init, -1);
    rb_define_method(rb_cPGconn, "db", pgconn_db, 0);
    rb_define_method(rb_cPGconn, "host", pgconn_host, 0);
    rb_define_method(rb_cPGconn, "options", pgconn_options, 0);
    rb_define_method(rb_cPGconn, "port", pgconn_port, 0);
    rb_define_method(rb_cPGconn, "tty", pgconn_tty, 0);
    rb_define_method(rb_cPGconn, "status", pgconn_status, 0);
    rb_define_method(rb_cPGconn, "error", pgconn_error, 0);
    rb_define_method(rb_cPGconn, "close", pgconn_close, 0);
    rb_define_alias(rb_cPGconn, "finish", "close");
    rb_define_method(rb_cPGconn, "reset", pgconn_reset, 0);
    rb_define_method(rb_cPGconn, "user", pgconn_user, 0);
    rb_define_method(rb_cPGconn, "trace", pgconn_trace, 1);
    rb_define_method(rb_cPGconn, "untrace", pgconn_untrace, 0);
    rb_define_method(rb_cPGconn, "exec", pgconn_exec, -1);
    rb_define_method(rb_cPGconn, "query", pgconn_query, -1);
    rb_define_method(rb_cPGconn, "select_one", pgconn_select_one, -1);
    rb_define_method(rb_cPGconn, "select_value", pgconn_select_value, -1);
    rb_define_method(rb_cPGconn, "select_values", pgconn_select_values, -1);
    rb_define_method(rb_cPGconn, "async_exec", pgconn_async_exec, 1);
    rb_define_method(rb_cPGconn, "async_query", pgconn_async_query, 1);
    rb_define_method(rb_cPGconn, "get_notify", pgconn_get_notify, 0);
    rb_define_method(rb_cPGconn, "insert_table", pgconn_insert_table, 2);
    rb_define_method(rb_cPGconn, "putline", pgconn_putline, 1);
    rb_define_method(rb_cPGconn, "getline", pgconn_getline, 0);
    rb_define_method(rb_cPGconn, "endcopy", pgconn_endcopy, 0);
    rb_define_method(rb_cPGconn, "on_notice", pgconn_on_notice, 0);
    rb_define_method(rb_cPGconn, "transaction_status", pgconn_transaction_status, 0);
    rb_define_method(rb_cPGconn, "protocol_version", pgconn_protocol_version, 0);
    rb_define_method(rb_cPGconn, "server_version", pgconn_server_version, 0);
    rb_define_method(rb_cPGconn, "escape", pgconn_escape, 1);
    rb_define_method(rb_cPGconn, "escape_bytea", pgconn_s_escape_bytea, 1);
    rb_define_method(rb_cPGconn, "unescape_bytea", pgconn_s_unescape_bytea, 1);
    rb_define_method(rb_cPGconn, "quote", pgconn_s_quote, 1);
    rb_define_method(rb_cPGconn, "quote_ident", pgconn_s_quote_ident, 1);
    rb_define_alias(rb_cPGconn, "format", "quote");

    rb_define_method(rb_cPGconn, "load_composite_types", pgconn_load_composite_types, 0);
    rb_define_method(rb_cPGconn, "get_composite_type", find_composite_type, 1);
    rb_define_method(rb_cPGconn, "reg_translate", pgconn_reg_translate, 2);
    rb_define_method(rb_cPGconn, "reg_array", pgconn_reg_array, 1);
    rb_define_method(rb_cPGconn, "compose", pgconn_compose, 2);
    rb_define_method(rb_cPGconn, "parse", pgconn_parse, 2);
#ifdef HAVE_PQEXECPARAMS
    rb_define_method(rb_cPGconn, "prepare", pgconn_prepare, 2);
    rb_define_method(rb_cPGconn, "exec_prepared", pgconn_exec_prepared, -1);
 
#endif
    rb_define_attr(rb_cPGconn, "cached_types", 1, 0);
    /* following line is for rdoc */
    /* rb_define_method(rb_cPGconn, "lastval", pgconn_lastval, 0); */

#ifdef HAVE_PQSETCLIENTENCODING
    rb_define_method(rb_cPGconn, "client_encoding", pgconn_client_encoding, 0);
    rb_define_method(rb_cPGconn, "set_client_encoding", pgconn_set_client_encoding, 1);
#endif

    /* Large Object support */
    rb_define_method(rb_cPGconn, "lo_import", pgconn_loimport, 1);
    rb_define_alias(rb_cPGconn, "loimport", "lo_import");
    rb_define_method(rb_cPGconn, "lo_create", pgconn_locreate, -1);
    rb_define_alias(rb_cPGconn, "locreate", "lo_create");
    rb_define_method(rb_cPGconn, "lo_open", pgconn_loopen, -1);
    rb_define_alias(rb_cPGconn, "loopen", "lo_open");
    rb_define_method(rb_cPGconn, "lo_export", pgconn_loexport, 2);
    rb_define_alias(rb_cPGconn, "loexport", "lo_export");
    rb_define_method(rb_cPGconn, "lo_unlink", pgconn_lounlink, 1);
    rb_define_alias(rb_cPGconn, "lounlink", "lo_unlink");
    
    rb_cPGlarge = rb_define_class("PGlarge", rb_cObject);
    rb_define_method(rb_cPGlarge, "oid",pglarge_oid, 0);
    rb_define_method(rb_cPGlarge, "open",pglarge_open, -1);
    rb_define_method(rb_cPGlarge, "close",pglarge_close, 0);
    rb_define_method(rb_cPGlarge, "read",pglarge_read, -1);
    rb_define_method(rb_cPGlarge, "write",pglarge_write, 1);
    rb_define_method(rb_cPGlarge, "seek",pglarge_seek, 2);
    rb_define_method(rb_cPGlarge, "tell",pglarge_tell, 0);
    rb_define_method(rb_cPGlarge, "size",pglarge_size, 0);
    rb_define_method(rb_cPGlarge, "export",pglarge_export, 1);
    rb_define_method(rb_cPGlarge, "unlink",pglarge_unlink, 0);

    rb_define_const(rb_cPGlarge, "INV_WRITE", INT2FIX(INV_WRITE));
    rb_define_const(rb_cPGlarge, "INV_READ", INT2FIX(INV_READ));
    rb_define_const(rb_cPGlarge, "SEEK_SET", INT2FIX(SEEK_SET));
    rb_define_const(rb_cPGlarge, "SEEK_CUR", INT2FIX(SEEK_CUR));
    rb_define_const(rb_cPGlarge, "SEEK_END", INT2FIX(SEEK_END));
    /* Large Object support */
    
    rb_cPGresult = rb_define_class("PGresult", rb_cObject);
    rb_include_module(rb_cPGresult, rb_mEnumerable);

    rb_define_const(rb_cPGresult, "EMPTY_QUERY", INT2FIX(PGRES_EMPTY_QUERY));
    rb_define_const(rb_cPGresult, "COMMAND_OK", INT2FIX(PGRES_COMMAND_OK));
    rb_define_const(rb_cPGresult, "TUPLES_OK", INT2FIX(PGRES_TUPLES_OK));
    rb_define_const(rb_cPGresult, "COPY_OUT", INT2FIX(PGRES_COPY_OUT));
    rb_define_const(rb_cPGresult, "COPY_IN", INT2FIX(PGRES_COPY_IN));
    rb_define_const(rb_cPGresult, "BAD_RESPONSE", INT2FIX(PGRES_BAD_RESPONSE));
    rb_define_const(rb_cPGresult, "NONFATAL_ERROR",INT2FIX(PGRES_NONFATAL_ERROR));
    rb_define_const(rb_cPGresult, "FATAL_ERROR", INT2FIX(PGRES_FATAL_ERROR));

    rb_define_method(rb_cPGresult, "status", pgresult_status, 0);
    rb_define_alias(rb_cPGresult, "result", "entries");
    rb_define_alias(rb_cPGresult, "rows", "entries");
    rb_define_method(rb_cPGresult, "each", pgresult_each, 0);
    rb_define_method(rb_cPGresult, "[]", pgresult_aref, -1);
    rb_define_method(rb_cPGresult, "fields", pgresult_fields, 0);
    rb_define_method(rb_cPGresult, "num_tuples", pgresult_num_tuples, 0);
    rb_define_method(rb_cPGresult, "num_fields", pgresult_num_fields, 0);
    rb_define_method(rb_cPGresult, "fieldname", pgresult_fieldname, 1);
    rb_define_method(rb_cPGresult, "fieldnum", pgresult_fieldnum, 1);
    rb_define_method(rb_cPGresult, "type", pgresult_type, 1);
    rb_define_method(rb_cPGresult, "size", pgresult_size, 1);
    rb_define_method(rb_cPGresult, "getvalue", pgresult_getvalue, 2);
    rb_define_method(rb_cPGresult, "getvalue_byname", pgresult_getvalue_byname, 2);
    rb_define_method(rb_cPGresult, "getlength", pgresult_getlength, 2);
    rb_define_method(rb_cPGresult, "getisnull", pgresult_getisnull, 2);
    rb_define_method(rb_cPGresult, "cmdtuples", pgresult_cmdtuples, 0);
    rb_define_method(rb_cPGresult, "cmdstatus", pgresult_cmdstatus, 0);
    rb_define_method(rb_cPGresult, "oid", pgresult_oid, 0);
    rb_define_method(rb_cPGresult, "print", pgresult_print, 2);
    rb_define_method(rb_cPGresult, "clear", pgresult_clear, 0);
    rb_define_alias(rb_cPGresult, "close", "clear");

    rb_cPGrow = rb_define_class("PGrow", rb_cArray);
    rb_define_method(rb_cPGrow, "initialize", pgrow_init, 1);
    rb_define_method(rb_cPGrow, "[]", pgrow_aref, -1);
    rb_define_method(rb_cPGrow, "keys", pgrow_keys, 0);
    rb_define_method(rb_cPGrow, "values", pgrow_values, 0);
    rb_define_method(rb_cPGrow, "each", pgrow_each, 0);
    rb_define_method(rb_cPGrow, "each_pair", pgrow_each_pair, 0);
    rb_define_method(rb_cPGrow, "each_key", pgrow_each_key, 0);
    rb_define_method(rb_cPGrow, "each_value", pgrow_each_value, 0);
    rb_define_method(rb_cPGrow, "to_hash", pgrow_to_hash, 0); 

    rb_cPGfield  = rb_define_class("PGfield", rb_cObject);
    rb_define_method(rb_cPGfield, "initialize", pg_attribute_init, 4);
    rb_define_attr(rb_cPGfield, "name", 1, 0);
    /* type OID */
    rb_define_attr(rb_cPGfield, "type_oid", 1, 0);
    /* type modifier */
    rb_define_attr(rb_cPGfield, "type_mod", 1, 0);
    /* formatted type string*/
    rb_define_attr(rb_cPGfield, "type_name", 1, 0);

    rb_cPGrowtype  = rb_define_class("PGrowtype", rb_cObject);
    rb_define_method(rb_cPGrowtype, "initialize", pg_composite_init, 3);
    rb_define_method(rb_cPGrowtype, "index", pg_composite_index, 1);
    /* array of field definitions (PGfield) */
    rb_define_attr(rb_cPGrowtype, "attributes", 1, 0);
    /* composite type OID */
    rb_define_attr(rb_cPGrowtype, "oid", 1, 0);
    rb_define_attr(rb_cPGrowtype, "name", 1, 0);

    rb_cPGrecord  = rb_define_class("PGrecord", rb_cObject);
    rb_define_method(rb_cPGrecord, "initialize", pg_composite_value_init, 2);
    rb_define_method(rb_cPGrecord, "names", pg_composite_value_names, 0);
    rb_define_method(rb_cPGrecord, "[]", pg_composite_value_aref, 1);
    rb_define_method(rb_cPGrecord, "==", pg_composite_value_equal, 1);
    rb_define_method(rb_cPGrecord, "[]=", pg_composite_value_aset, 2);
    rb_define_method(rb_cPGrecord, "method_missing", pg_composite_value_method_missing, -1);
    /* record type (PGrowtype) */
    rb_define_attr(rb_cPGrecord, "record_type", 1, 0);

    rb_cPGbytea  = rb_define_class("PGbytea", rb_cString);

}
