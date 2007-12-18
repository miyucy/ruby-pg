/************************************************

  pg.c -

  Author: matz 
  created at: Tue May 13 20:07:35 JST 1997

  Author: ematsu
  modified at: Wed Jan 20 16:41:51 1999

  $Author$
  $Date$
************************************************/

#include "pg.h"

#define AssignCheckedStringValue(cstring, rstring) do { \
    if (!NIL_P(temp = rstring)) { \
        Check_Type(temp, T_STRING); \
        cstring = StringValuePtr(temp); \
    } \
} while (0)

#define rb_check_hash_type(x) rb_check_convert_type(x, T_HASH, "Hash", "to_hash")

#define rb_define_singleton_alias(klass,new,old) rb_define_alias(rb_singleton_class(klass),new,old)

#define Data_Set_Struct(obj,ptr) do { \
    Check_Type(obj, T_DATA); \
    DATA_PTR(obj) = ptr; \
} while (0)

static VALUE rb_cPGconn;
static VALUE rb_cPGresult;
static VALUE rb_ePGError;

/***************************************************************************
 * UTILITY FUNCTIONS
 **************************************************************************/

static void free_pgconn(PGconn *);
static void pgresult_check(VALUE, VALUE);

static int build_key_value_string_i(VALUE key, VALUE value, VALUE result);
static PGconn *get_pgconn(VALUE obj);
static VALUE pgconn_finish(VALUE self);

static VALUE pg_escape_regex;
static VALUE pg_escape_str;
static ID    pg_gsub_bang_id;

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

static void
free_pgconn(ptr)
    PGconn *ptr;
{
    PQfinish(ptr);
}

static VALUE
pgconn_alloc(klass)
    VALUE klass;
{
    return Data_Wrap_Struct(klass, 0, free_pgconn, NULL);
}

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

static PGconn*
get_pgconn(obj)
    VALUE obj;
{
    PGconn *conn;

    Data_Get_Struct(obj, PGconn, conn);
    if (conn == NULL) rb_raise(rb_ePGError, "closed connection");
    return conn;
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

static void
free_pgresult(ptr)
    PGresult *ptr;
{
    PQclear(ptr);
}

static VALUE
pgresult_new(ptr)
    PGresult *ptr;
{
    return Data_Wrap_Struct(rb_cPGresult, 0, free_pgresult, ptr);
}

/*
 * Raises appropriate exception if PGresult is
 * in a bad state.
 */
static void
pgresult_check(VALUE rb_pgconn, VALUE rb_pgresult)
{
	VALUE error;
	PGconn *conn = get_pgconn(rb_pgconn);
	PGresult *result = get_pgresult(rb_pgresult);

	if(result == NULL)
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
	switch (PQresultStatus(result)) {
	case PGRES_TUPLES_OK:
	case PGRES_COPY_OUT:
	case PGRES_COPY_IN:
	case PGRES_EMPTY_QUERY:
	case PGRES_COMMAND_OK:      
		return;
	case PGRES_BAD_RESPONSE:
	case PGRES_FATAL_ERROR:
	case PGRES_NONFATAL_ERROR:
		error = rb_exc_new2(rb_ePGError, PQresultErrorMessage(result));
		break;
	default:
		error = rb_exc_new2(rb_ePGError, 
			"internal error : unknown result status.");
	}
	
	rb_iv_set(error, "@connection", rb_pgconn);
	rb_iv_set(error, "@result", rb_pgresult);
	rb_exc_raise(error);
	return;
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
 *    #pass
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
 *    
 *    
 *    #get_notify
 *    #on_notice
 *    
 *    #putline
 *    #getline
 *    #endcopy
 *    
 *    classes: PGresult
 *    
 * 4. Prepared statements:
 *    #prepare
 *    #exec_prepared
 *
 */

/**************************************************************************
 * PGconn SINGLETON METHODS
 **************************************************************************/

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

    Data_Set_Struct(self, conn);
    return self;
}

//TODO PGconn.conndefaults

static VALUE
pgconn_init(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    pgconn_connect(argc, argv, self);
    if (rb_block_given_p()) {
        return rb_ensure(rb_yield, self, pgconn_finish, self);
    }
    return self;
}

/*
 * call-seq:
 *    PGconn.encrypt_password( password, username ) -> String
 *
 * This function is intended to be used by client applications that
 * send commands like: +ALTER USER joe PASSWORD 'pwd'+.
 * The arguments are the cleartext password, and the SQL name 
 * of the user it is for.
 *
 * Return value is the encrypted password.
 */
static VALUE
pgconn_s_encrypt_password(obj, password, username)
	VALUE obj, password, username;
{
	Check_Type(password, T_STRING);
	Check_Type(username, T_STRING);
	char *ret = PQencryptPassword(StringValuePtr(password),
		StringValuePtr(username));
	return rb_tainted_str_new2(ret);
}

/*
 * call-seq:
 *    PGconn.isthreadsafe() -> Boolean
 *
 * Returns +true+ if libpq is thread safe, +false+ otherwise.
 */
static VALUE
pgconn_s_isthreadsafe(obj)
	VALUE obj;
{
	return PQisthreadsafe() ? Qtrue : Qfalse;
}

/**************************************************************************
 * PGconn INSTANCE METHODS
 **************************************************************************/

/*
 * call-seq:
 *    conn.finish()
 *
 * Closes the backend connection.
 */
static VALUE
pgconn_finish(self)
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
 * Resets the backend connection. This method closes the backend connection and tries to re-connect.
 */
static VALUE
pgconn_reset(obj)
    VALUE obj;
{
    PQreset(get_pgconn(obj));
    return obj;
}

//TODO conn.reset_start

//TODO conn.reset_poll

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
 *    conn.pass()
 *
 * Returns the authenticated user name.
 */
static VALUE
pgconn_pass(obj)
    VALUE obj;
{
    char *user = PQpass(get_pgconn(obj));
    if (!user) return Qnil;
    return rb_tainted_str_new2(user);
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
 *    conn.transaction_status()
 *
 * returns one of the following statuses:
 *   PQTRANS_IDLE    = 0 (connection idle)
 *   PQTRANS_ACTIVE  = 1 (command in progress)
 *   PQTRANS_INTRANS = 2 (idle, within transaction block)
 *   PQTRANS_INERROR = 3 (idle, within failed transaction)
 *   PQTRANS_UNKNOWN = 4 (cannot determine status)
 */
static VALUE
pgconn_transaction_status(obj)
    VALUE obj;
{
    return INT2NUM(PQtransactionStatus(get_pgconn(obj)));
}

/*
 * call-seq:
 *    conn.parameter_status( param_name ) -> String
 *
 * Returns the setting of parameter _param_name_, where
 * _param_name_ is one of
 * * +server_version+
 * * +server_encoding+
 * * +client_encoding+ 
 * * +is_superuser+
 * * +session_authorization+
 * * +DateStyle+
 * * +TimeZone+
 * * +integer_datetimes+
 * * +standard_conforming_strings+
 * 
 * Returns nil if the value of the parameter is not known.
 */
static VALUE
pgconn_parameter_status(obj, param_name)
	VALUE obj, param_name;
{
	const char *ret = PQparameterStatus(get_pgconn(obj), 
			StringValuePtr(param_name));
	if(ret == NULL)
		return Qnil;
	else
		return rb_tainted_str_new2(ret);
}

/*
 * call-seq:
 *  conn.protocol_version -> Integer
 *
 * The 3.0 protocol will normally be used when communicating with PostgreSQL 7.4 
 * or later servers; pre-7.4 servers support only protocol 2.0. (Protocol 1.0 is 
 * obsolete and not supported by libpq.)
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
 *    conn.error() -> String
 *
 * Returns the error message about connection.
 */
static VALUE
pgconn_error_message(obj)
    VALUE obj;
{
    char *error = PQerrorMessage(get_pgconn(obj));
    if (!error) return Qnil;
    return rb_tainted_str_new2(error);
}

//TODO PQsocket
/*
 * call-seq:
 *    conn.socket() -> TCPSocket
 *
 * Returns the socket file descriptor of this
 * connection.
 */


/*
 * call-seq:
 *    conn.backed_pid() -> Fixnum
 *
 * Returns the process ID of the backend server
 * process for this connection.
 * Note that this is a PID on database server host.
 */
static VALUE
pgconn_backend_pid(self)
	VALUE self;
{
	return INT2NUM(PQbackendPID(get_pgconn(self)));
}

/*
 * call-seq:
 *    conn.connection_used_password() -> Boolean
 *
 * Returns +true+ if the authentication required a password,
 * +false+ otherwise.
 */
static VALUE
pgconn_connection_used_password(self)
	VALUE self;
{
	return PQconnectionUsedPassword(get_pgconn(self)) ? Qtrue : Qfalse;
}


//TODO get_ssl


/*
 * call-seq:
 *    conn.exec(sql) -> PGresult
 *
 * Sends SQL query request specified by _sql_ to the PostgreSQL.
 * Returns a PGresult instance on success.
 * On failure, it raises a PGError exception.
 *
 */
static VALUE
pgconn_exec(obj, in_command)
    VALUE obj, in_command;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result = NULL;
	VALUE rb_pgresult;
	VALUE command;

	if(TYPE(in_command) == T_STRING)
		command = in_command;
	else
		command = rb_funcall(in_command, rb_intern("to_s"), 0);
	Check_Type(command, T_STRING);

	result = PQexec(conn, StringValuePtr(command));

	rb_pgresult = pgresult_new(result);
	pgresult_check(obj, rb_pgresult);

	return rb_pgresult;
}

/*
 * call-seq:
 *    conn.exec_params(sql, params, result_format) -> PGresult
 *
 * Sends SQL query request specified by _sql_ to the PostgreSQL.
 * Returns a PGresult instance on success.
 * On failure, it raises a PGErr or exception.
 *
 * +params+ is an array of the bind parameters for the SQL query.
 * Each element of the +params+ array may be either:
 *   a hash of the form:
 *     {:value  => String (value of bind parameter)
 *      :type   => Fixnum (oid of type of bind parameter)
 *      :format => Fixnum (0 for text, 1 for binary)
 *     }
 *   or, it may be a String. If it is a string, that is equivalent to:
 *     { :value => <string value>, :type => 0, :format => 0 }
 * 
 * PostgreSQL bind parameters are represented as $1, $1, $2, etc.,
 * inside the SQL query. The 0th element of the +params+ array is bound
 * to $1, the 1st element is bound to $2, etc.
 * 
 * If the types are not specified, they will be inferred by PostgreSQL.
 * Instead of specifying type oids, it's recommended to simply add
 * explicit casts in the query to ensure that the right type is used.
 *
 * For example: "SELECT $1::int"
 *
 * The optional +result_format+ should be 0 for text results, 1
 * for binary.
 */
static VALUE
pgconn_exec_params(argc, argv, obj)
    int argc;
	VALUE *argv;
	VALUE obj;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result = NULL;
	VALUE rb_pgresult;
	VALUE command, params, in_res_fmt;
	VALUE param, param_type, param_value, param_format;
	VALUE param_value_tmp;
	VALUE sym_type, sym_value, sym_format;
	int i=0;

	int nParams;
	Oid *paramTypes;
	char ** paramValues;
	int *paramLengths;
	int *paramFormats;
	int resultFormat;


    rb_scan_args(argc, argv, "12", &command, &params, &in_res_fmt);

    Check_Type(command, T_STRING);

	if(NIL_P(params)) {
		params = rb_ary_new2(0);
		resultFormat = 0;
	}
	else {
		Check_Type(params, T_ARRAY);
	}

	if(NIL_P(in_res_fmt)) {
		resultFormat = 0;
	}
	else {
		resultFormat = NUM2INT(in_res_fmt);
	}

	sym_type = ID2SYM(rb_intern("type"));
	sym_value = ID2SYM(rb_intern("value"));
	sym_format = ID2SYM(rb_intern("format"));

	nParams = RARRAY(params)->len;
	paramTypes = ALLOC_N(Oid, nParams); 
	paramValues = ALLOC_N(char *, nParams);
	paramLengths = ALLOC_N(int, nParams);
	paramFormats = ALLOC_N(int, nParams);
	for(i = 0; i < nParams; i++) {
		param = rb_ary_entry(params, i);
		if (TYPE(param) == T_HASH) {
			param_type = rb_hash_aref(param, sym_type);
			param_value_tmp = rb_hash_aref(param, sym_value);
			if(TYPE(param_value_tmp) == T_STRING)
				param_value = param_value_tmp;
			else
				param_value = rb_funcall(param_value_tmp, rb_intern("to_s"), 0);
			param_format = rb_hash_aref(param, sym_format);
		}
		else {
			param_type = INT2NUM(0);
			if(TYPE(param) == T_STRING)
				param_value = param;
			else
				param_value = rb_funcall(param, rb_intern("to_s"), 0);
			param_format = INT2NUM(0);
		}
		Check_Type(param_value, T_STRING);
		paramTypes[i] = NUM2INT(param_type);
		paramValues[i] = RSTRING(param_value)->ptr;
		paramLengths[i] = RSTRING(param_value)->len + 1;
		paramFormats[i] = NUM2INT(param_format);
	}
	
	result = PQexecParams(conn, StringValuePtr(command), nParams, paramTypes, 
		(const char * const *)paramValues, paramLengths, paramFormats, resultFormat);

	free(paramTypes);
	free(paramValues);
	free(paramLengths);
	free(paramFormats);

	rb_pgresult = pgresult_new(result);
	pgresult_check(obj, rb_pgresult);

	return rb_pgresult;

}

/*
 * call-seq:
 *    conn.prepare(sql, stmt_name, param_types) -> PGresult
 *
 * Prepares statement _sql_ with name _name_ to be executed later.
 * Returns a PGresult instance on success.
 * On failure, it raises a PGError exception.
 *
 * +param_types+ is an optional parameter to specify the Oids of the 
 * types of the parameters.
 *
 * If the types are not specified, they will be inferred by PostgreSQL.
 * Instead of specifying type oids, it's recommended to simply add
 * explicit casts in the query to ensure that the right type is used.
 *
 * For example: "SELECT $1::int"
 * 
 * PostgreSQL bind parameters are represented as $1, $1, $2, etc.,
 * inside the SQL query.
 */
static VALUE
pgconn_prepare(argc, argv, obj)
    int argc;
	VALUE *argv;
	VALUE obj;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result = NULL;
	VALUE rb_pgresult;
	VALUE name, command, in_paramtypes;
	VALUE param;
	int i = 0;

	int nParams = 0;
	Oid *paramTypes = NULL;

    rb_scan_args(argc, argv, "21", &name, &command, &in_paramtypes);

	Check_Type(name, T_STRING);
    Check_Type(command, T_STRING);

	if(! NIL_P(in_paramtypes)) {
		Check_Type(in_paramtypes, T_ARRAY);
		nParams = RARRAY(in_paramtypes)->len;
		paramTypes = ALLOC_N(Oid, nParams); 
		for(i = 0; i < nParams; i++) {
			param = rb_ary_entry(in_paramtypes, i);
			Check_Type(param, T_FIXNUM);
			paramTypes[i] = NUM2INT(param);
		}
	}
	result = PQprepare(conn, StringValuePtr(name), StringValuePtr(command),
			nParams, paramTypes);

	free(paramTypes);

	rb_pgresult = pgresult_new(result);
	pgresult_check(obj, rb_pgresult);

	return rb_pgresult;

}

/*
 * call-seq:
 *    conn.exec_prepared(statement_name, params, result_format)
 *
 * Execute prepared named statement specified by _statement_name_.
 * Returns a PGresult instance on success.
 * On failure, it raises a PGError exception.
 *
 * +params+ is an array of the optional bind parameters for the 
 * SQL query. Each element of the +params+ array may be either:
 *   a hash of the form:
 *     {:value  => String (value of bind parameter)
 *      :format => Fixnum (0 for text, 1 for binary)
 *     }
 *   or, it may be a String. If it is a string, that is equivalent to:
 *     { :value => <string value>, :format => 0 }
 * 
 * PostgreSQL bind parameters are represented as $1, $1, $2, etc.,
 * inside the SQL query. The 0th element of the +params+ array is bound
 * to $1, the 1st element is bound to $2, etc.
 *
 * The optional +result_format+ should be 0 for text results, 1
 * for binary.
 */
static VALUE
pgconn_exec_prepared(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    PGconn *conn = get_pgconn(obj);
    PGresult *result = NULL;
	VALUE rb_pgresult;
	VALUE name, params, in_res_fmt;
	VALUE param, param_value, param_format;
	VALUE param_value_tmp;
	VALUE sym_value, sym_format;
	int i = 0;

	int nParams;
	char ** paramValues;
	int *paramLengths;
	int *paramFormats;
	int resultFormat;


    rb_scan_args(argc, argv, "12", &name, &params, &in_res_fmt);

	Check_Type(name, T_STRING);

	if(NIL_P(params)) {
		params = rb_ary_new2(0);
		resultFormat = 0;
	}
	else {
		Check_Type(params, T_ARRAY);
	}

	if(NIL_P(in_res_fmt)) {
		resultFormat = 0;
	}
	else {
		resultFormat = NUM2INT(in_res_fmt);
	}

	sym_value = ID2SYM(rb_intern("value"));
	sym_format = ID2SYM(rb_intern("format"));

	nParams = RARRAY(params)->len;
	paramValues = ALLOC_N(char *, nParams);
	paramLengths = ALLOC_N(int, nParams);
	paramFormats = ALLOC_N(int, nParams);
	for(i = 0; i < nParams; i++) {
		param = rb_ary_entry(params, i);
		if (TYPE(param) == T_HASH) {
			param_value_tmp = rb_hash_aref(param, sym_value);
			if(TYPE(param_value_tmp) == T_STRING)
				param_value = param_value_tmp;
			else
				param_value = rb_funcall(param_value_tmp, rb_intern("to_s"), 0);
			param_format = rb_hash_aref(param, sym_format);
		}
		else {
			if(TYPE(param) == T_STRING)
				param_value = param;
			else
				param_value = rb_funcall(param, rb_intern("to_s"), 0);
			param_format = INT2NUM(0);
		}
		Check_Type(param_value, T_STRING);
		paramValues[i] = RSTRING(param_value)->ptr;
		paramLengths[i] = RSTRING(param_value)->len + 1;
		paramFormats[i] = NUM2INT(param_format);
	}
	
	result = PQexecPrepared(conn, StringValuePtr(name), nParams, 
		(const char * const *)paramValues, paramLengths, paramFormats, 
		resultFormat);

	free(paramValues);
	free(paramLengths);
	free(paramFormats);

	rb_pgresult = pgresult_new(result);
	pgresult_check(obj, rb_pgresult);

	return rb_pgresult;
}

/*
 * call-seq:
 *    conn.describe_prepared( statement_name ) -> PGresult
 *
 * Retrieve information about the prepared statement
 * _statement_name_.
 */
static VALUE
pgconn_describe_prepared(self, stmt_name)
	VALUE self, stmt_name;
{
	PGconn *conn = get_pgconn(self);
	PGresult *result;
	VALUE rb_pgresult;
	char *stmt;
	if(stmt_name == Qnil) {
		stmt = NULL;
	}
	else {
		Check_Type(stmt_name, T_STRING);
		stmt = StringValuePtr(stmt_name);
	}
	result = PQdescribePrepared(conn, stmt);
	rb_pgresult = pgresult_new(result);
	pgresult_check(self, rb_pgresult);
	return rb_pgresult;
}


/*
 * call-seq:
 *    conn.describe_portal( portal_name ) -> PGresult
 *
 * Retrieve information about the portal _portal_name_.
 */
static VALUE
pgconn_describe_portal(self, stmt_name)
	VALUE self, stmt_name;
{
	PGconn *conn = get_pgconn(self);
	PGresult *result;
	VALUE rb_pgresult;
	char *stmt;
	if(stmt_name == Qnil) {
		stmt = NULL;
	}
	else {
		Check_Type(stmt_name, T_STRING);
		stmt = StringValuePtr(stmt_name);
	}
	result = PQdescribePortal(conn, stmt);
	rb_pgresult = pgresult_new(result);
	pgresult_check(self, rb_pgresult);
	return rb_pgresult;
}


// TODO make_empty_pgresult


/*
 * call-seq:
 *    conn.escape_string( str ) -> String
 *    PGconn.escape_string( str ) -> String  # DEPRECATED
 *
 * Connection instance method for versions of 8.1 and higher of libpq
 * uses PQescapeStringConn, which is safer. Avoid calling as a class method,
 * the class method uses the deprecated PQescapeString() API function.
 * 
 * Returns a SQL-safe version of the String _str_.
 * This is the preferred way to make strings safe for inclusion in SQL queries.
 * 
 * Consider using exec_params, which avoids the need for passing values inside of 
 * SQL commands.
 */
static VALUE
pgconn_s_escape(self, string)
    VALUE self;
    VALUE string;
{
    char *escaped;
    int size,error;
    VALUE result;

    Check_Type(string, T_STRING);
    
    escaped = ALLOCA_N(char, RSTRING(string)->len * 2 + 1);
    if(CLASS_OF(self) == rb_cPGconn) {
    	size = PQescapeStringConn(get_pgconn(self),escaped, RSTRING(string)->ptr,
			RSTRING(string)->len, &error);
		if(error) {
			rb_raise(rb_ePGError, PQerrorMessage(get_pgconn(self)));
		}
    } else {
    	size = PQescapeString(escaped, RSTRING(string)->ptr,
			RSTRING(string)->len);
    }
    result = rb_str_new(escaped, size);
    OBJ_INFECT(result, string);
    return result;
}

/*
 * call-seq:
 *   conn.escape_bytea( obj ) -> String 
 *   PGconn.escape_bytea( obj ) -> String # DEPRECATED
 *
 * Connection instance method for versions of 8.1 and higher of libpq
 * uses PQescapeByteaConn, which is safer. Avoid calling as a class method,
 * the class method uses the deprecated PQescapeBytea() API function.
 *
 * Use the instance method version of this function, it is safer than the
 * class method.
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
 * Consider using exec_params, which avoids the need for passing values inside of 
 * SQL commands.
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
    
    if(CLASS_OF(self) == rb_cPGconn) {
        to = (char *)PQescapeByteaConn(get_pgconn(self),(unsigned char*)from, from_len, &to_len);
    } else {
        to = (char *)PQescapeBytea( (unsigned char*)from, from_len, &to_len);
    }
    
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

    to = (char *) PQunescapeBytea( (unsigned char*) from, &to_len);

    ret = rb_str_new(to, to_len);
    OBJ_INFECT(ret, obj);
    PQfreemem(to);

    return ret;
}

/*
 * call-seq:
 *    conn.send_query( command ) -> nil
 *
 * Asynchronously send _command_ to the server. Does not block. 
 * Use in combination with +conn.get_result+.
 */
static VALUE
pgconn_send_query(obj, command)
	VALUE obj, command;
{
	VALUE error;
	PGconn *conn = get_pgconn(obj);
	/* returns 0 on failure */
	if(PQsendQuery(conn,StringValuePtr(command)) == 0) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
	}
	return Qnil;
}


//TODO send_query_params

/*TODO
 * call-seq:
 *    conn.send_prepare( command ) -> nil
 *
 * Asynchronously send _command_ to the server. Does not block. 
 * Use in combination with +conn.get_result+.
 */
static VALUE
pgconn_send_prepare(obj, command)
	VALUE obj, command;
{
	VALUE error;
	PGconn *conn = get_pgconn(obj);
	/* returns 0 on failure */
	if(PQsendQuery(conn,StringValuePtr(command)) == 0) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
	}
	return Qnil;
}


//TODO send_query_prepared

/*
 * call-seq:
 *    conn.send_describe_prepared( statement_name ) -> nil
 *
 * Asynchronously send _command_ to the server. Does not block. 
 * Use in combination with +conn.get_result+.
 */
static VALUE
pgconn_send_describe_prepared(obj, stmt_name)
	VALUE obj, stmt_name;
{
	VALUE error;
	PGconn *conn = get_pgconn(obj);
	/* returns 0 on failure */
	if(PQsendDescribePrepared(conn,StringValuePtr(stmt_name)) == 0) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
	}
	return Qnil;
}


/*
 * call-seq:
 *    conn.send_describe_portal( portal_name ) -> nil
 *
 * Asynchronously send _command_ to the server. Does not block. 
 * Use in combination with +conn.get_result+.
 */
static VALUE
pgconn_send_describe_portal(obj, portal)
	VALUE obj, portal;
{
	VALUE error;
	PGconn *conn = get_pgconn(obj);
	/* returns 0 on failure */
	if(PQsendDescribePortal(conn,StringValuePtr(portal)) == 0) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
	}
	return Qnil;
}


/*
 * call-seq:
 *    conn.get_result() -> PGresult
 *
 * Asynchronously send _command_ to the server. Does not block. 
 * Use in combination with +conn.get_result+.
 */
static VALUE
pgconn_get_result(obj)
	VALUE obj;
{
	PGresult *result;
	VALUE rb_pgresult;

	result = PQgetResult(get_pgconn(obj));
	if(result == NULL)
		return Qnil;
	
	rb_pgresult = pgresult_new(result);
	pgresult_check(obj, rb_pgresult);

	return rb_pgresult;
}

/*
 * call-seq:
 *    conn.consume_input()
 *
 * If input is available from the server, consume it.
 * After calling +consume_input+, you can check +is_busy+
 * or *notifies* to see if the state has changed.
 */
static VALUE
pgconn_consume_input(obj)
	VALUE obj;
{
	VALUE error;
	PGconn *conn = get_pgconn(obj);
	/* returns 0 on error */
	if(PQconsumeInput(conn) == 0) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
	}
	return Qnil;
}

/*
 * call-seq:
 *    conn.is_busy() -> Boolean
 *
 * Returns +true+ if a command is busy, that is, if
 * PQgetResult would block. Otherwise returns +false+.
 */
static VALUE
pgconn_is_busy(obj)
	VALUE obj;
{
	return PQisBusy(get_pgconn(obj)) ? Qtrue : Qfalse;
}

/*
 * call-seq:
 *    conn.setnonblocking() -> Boolean
 *
 * Returns +true+ if a command is busy, that is, if
 * PQgetResult would block. Otherwise returns +false+.
 */
static VALUE
pgconn_setnonblocking(obj, state)
	VALUE obj, state;
{
	int arg;
	VALUE error;
	PGconn *conn = get_pgconn(obj);
	if(state == Qtrue)
		arg = 1;
	else if (state == Qfalse)
		arg = 0;
	else
		rb_raise(rb_eArgError, "Boolean value expected");

	if(PQsetnonblocking(conn, arg) == -1) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
	}
	return Qnil;
}


/*
 * call-seq:
 *    conn.isnonblocking() -> Boolean
 *
 * Returns +true+ if a command is busy, that is, if
 * PQgetResult would block. Otherwise returns +false+.
 */
static VALUE
pgconn_isnonblocking(obj)
	VALUE obj;
{
	return PQisnonblocking(get_pgconn(obj)) ? Qtrue : Qfalse;
}

/*TODO
 * call-seq:
 *    conn.flush() -> Boolean
 *
 * Returns +true+ if a command is busy, that is, if
 * PQgetResult would block. Otherwise returns +false+.
 */
static VALUE
pgconn_flush(obj)
	VALUE obj;
{
	//if(PQflush(get_pgconn(obj))) 
	return Qnil;
}

//TODO get_cancel

//TODO free_cancel

//TODO cancel

//TODO fn

/*
 * call-seq:
 *    conn.notifies()
 *
 * Returns an array of the unprocessed notifiers.
 * If there is no unprocessed notifier, it returns +nil+.
 */
static VALUE
pgconn_notifies(obj)
    VALUE obj;
{
    PGconn* conn = get_pgconn(obj);
    PGnotify *notify;
    VALUE hash;
	VALUE sym_relname, sym_be_pid, sym_extra;
	VALUE relname, be_pid, extra;
	VALUE error;

    if (PQconsumeInput(conn) == 0) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
    }

	sym_relname = ID2SYM(rb_intern("relname"));
	sym_be_pid = ID2SYM(rb_intern("be_pid"));
	sym_extra = ID2SYM(rb_intern("extra"));

    /* gets notify and builds result */
    notify = PQnotifies(conn);
    if (notify == NULL) {
        /* there are no unhandled notifications */
        return Qnil;
    }
	
	hash = rb_hash_new();
	relname = rb_tainted_str_new2(notify->relname);
	be_pid = INT2NUM(notify->be_pid);
	extra = rb_tainted_str_new2(notify->extra);
	
    rb_hash_aset(hash, sym_relname, relname);
	rb_hash_aset(hash, sym_be_pid, be_pid);
	rb_hash_aset(hash, sym_extra, extra);

    PQfreemem(notify);

    /* returns result */
    return hash;
}


/*
 * call-seq:
 *    conn.put_copy_data( buffer ) -> Boolean
 *
 * Transmits _buffer_ as copy data to the server.
 * Returns true if the data was sent, false if it was
 * not sent (false is only possible if the connection
 * is in nonblocking mode, and this command would block).
 *
 * Raises an exception if an error occurs.
 */
static VALUE
pgconn_put_copy_data(obj, buffer)
	VALUE obj, buffer;
{
	int ret;
	VALUE error;
	PGconn *conn = get_pgconn(obj);
	Check_Type(buffer, T_STRING);

	ret = PQputCopyData(conn, RSTRING(buffer)->ptr,
			RSTRING(buffer)->len);
	if(ret == -1) {
		error = rb_exc_new2(rb_ePGError, PQerrorMessage(conn));
		rb_iv_set(error, "@connection", obj);
		rb_raise(error, PQerrorMessage(conn));
	}
	return (ret) ? Qtrue : Qfalse;
}

//TODO put_copy_end

//TODO get_copy_data

//TODO set_error_verbosity

/*TODO
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

//TODO set_notice_receiver

//TODO set_notice_processor

/*TODO
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

/*TODO
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

/**** TODO ?????????? ******/


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
pgconn_set_notice_processor(self)
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



/**************************************************************************
 * LARGE OBJECT SUPPORT
 **************************************************************************/

/*
 * call-seq:
 *    conn.lo_creat( [mode] ) -> Fixnum
 *
 * Creates a large object with mode _mode_. Returns a large object Oid.
 * On failure, it raises PGError exception.
 */
static VALUE
pgconn_locreat(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    Oid lo_oid;
    int mode;
    VALUE nmode;
    PGconn *conn = get_pgconn(obj);
    
    if (rb_scan_args(argc, argv, "01", &nmode) == 0)
        mode = INV_READ;
    else
        mode = NUM2INT(nmode);
  
    lo_oid = lo_creat(conn, mode);
    if (lo_oid == 0)
        rb_raise(rb_ePGError, "lo_creat failed");

    return INT2FIX(lo_oid);
}

/*
 * call-seq:
 *    conn.lo_create( oid ) -> Fixnum
 *
 * Creates a large object with oid _oid_. Returns the large object Oid.
 * On failure, it raises PGError exception.
 */
static VALUE
pgconn_locreate(obj, in_lo_oid)
    VALUE obj, in_lo_oid;
{
    Oid ret, lo_oid;
    PGconn *conn = get_pgconn(obj);
	lo_oid = NUM2INT(in_lo_oid);
    
    ret = lo_create(conn, in_lo_oid);
    if (ret == InvalidOid)
        rb_raise(rb_ePGError, "lo_create failed");

    return INT2FIX(ret);
}

/*
 * call-seq:
 *    conn.lo_import(file) -> Fixnum
 *
 * Import a file to a large object. Returns a large object Oid.
 *
 * On failure, it raises a PGError exception.
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
    return INT2FIX(lo_oid);
}

/*
 * call-seq:
 *    conn.lo_export( oid, file ) -> nil
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

    if (lo_export(conn, oid, StringValuePtr(filename)) < 0) {
        rb_raise(rb_ePGError, PQerrorMessage(conn));
    }
    return Qnil;
}

/*
 * call-seq:
 *    conn.lo_open( oid, [mode] ) -> Fixnum
 *
 * Open a large object of _oid_. Returns a large object descriptor 
 * instance on success. The _mode_ argument specifies the mode for
 * the opened large object,which is either +INV_READ+, or +INV_WRITE+.
 *
 * If _mode_ is omitted, the default is +INV_READ+.
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

    rb_scan_args(argc, argv, "11", &objid, &nmode);
    lo_oid = NUM2INT(objid);
	if(NIL_P(nmode))
    	mode = INV_READ;
    else
		mode = NUM2INT(nmode);

    if((fd = lo_open(conn, lo_oid, mode)) < 0) {
        rb_raise(rb_ePGError, "can't open large object");
    }
    return INT2FIX(fd);
}

/*
 * call-seq:
 *    conn.lo_write( lo_desc, buffer ) -> Fixnum
 *
 * Writes the string _buffer_ to the large object _lo_desc_.
 * Returns the number of bytes written.
 */
static VALUE
pgconn_lowrite(obj, in_lo_desc, buffer)
    VALUE obj, buffer;
{
    int n;
    PGconn *conn = get_pgconn(obj);
	int fd = NUM2INT(in_lo_desc);

    Check_Type(buffer, T_STRING);

    if( RSTRING(buffer)->len < 0) {
        rb_raise(rb_ePGError, "write buffer zero string");
    }
    if((n = lo_write(conn, fd, StringValuePtr(buffer), RSTRING(buffer)->len)) < 0) {
        rb_raise(rb_ePGError, "lo_write failed");
    }
  
    return INT2FIX(n);
}

/*
 * call-seq:
 *    conn.lo_read( lo_desc, len ) -> String
 *
 * Attempts to read _len_ bytes from large object _lo_desc_,
 * returns resulting data.
 */
static VALUE
pgconn_loread(obj, in_lo_desc, in_len)
    VALUE obj, in_lo_desc, in_len;
{
    int ret;
    PGconn *conn = get_pgconn(obj);
    int len = NUM2INT(in_len);
	int lo_desc = NUM2INT(in_lo_desc);
	VALUE str = rb_tainted_str_new(0,len);
    
    if (len < 0){
        rb_raise(rb_ePGError,"nagative length %d given", len);
    }

    if((ret = lo_read(conn, lo_desc, StringValuePtr(str), len)) < 0)
        rb_raise(rb_ePGError, "lo_read failed");

    if (ret == 0)
		return Qnil;

    RSTRING(str)->len = ret;
    return str;
}


/*
 * call-seq:
 *    conn.lo_lseek( lo_desc, offset, whence ) -> Fixnum
 *
 * Move the large object pointer _lo_desc_ to offset _offset_.
 * Valid values for _whence_ are +SEEK_SET+, +SEEK_CUR+, and +SEEK_END+.
 * (Or 0, 1, or 2.)
 */
static VALUE
pgconn_lolseek(obj, in_lo_desc, offset, whence)
    VALUE obj, in_lo_desc, offset, whence;
{
    PGconn *conn = get_pgconn(obj);
	int lo_desc = NUM2INT(in_lo_desc);
    int ret;
    
    if((ret = lo_lseek(conn, lo_desc, NUM2INT(offset), NUM2INT(whence))) < 0) {
        rb_raise(rb_ePGError, "lo_lseek failed");
    }

    return INT2FIX(ret);
}

/*
 * call-seq:
 *    conn.lo_tell( lo_desc ) -> Fixnum
 *
 * Returns the current position of the large object _lo_desc_.
 */
static VALUE
pgconn_lotell(obj,in_lo_desc)
    VALUE obj, in_lo_desc;
{
    int position;
	PGconn *conn = get_pgconn(obj);
	int lo_desc = NUM2INT(in_lo_desc);

	if((position = lo_tell(conn, lo_desc)) < 0)
		rb_raise(rb_ePGError,"lo_tell failed");

    return INT2FIX(position);
}

/*
 * call-seq:
 *    conn.lo_truncate( lo_desc, len ) -> nil
 *
 * Truncates the large object _lo_desc_ to size _len_.
 */
static VALUE
pgconn_lotruncate(obj, in_lo_desc, in_len)
    VALUE obj, in_lo_desc, in_len;
{
    PGconn *conn = get_pgconn(obj);
    int lo_desc = NUM2INT(in_lo_desc);
	size_t len = NUM2INT(in_len);
    
    if(lo_truncate(conn,lo_desc,len) < 0)
		rb_raise(rb_ePGError,"lo_truncate failed");

    return Qnil;
}

/*
 * call-seq:
 *    conn.lo_close( lo_desc ) -> nil
 *
 * Closes the postgres large object of _lo_desc_.
 */
static VALUE
pgconn_loclose(obj, in_lo_desc)
    VALUE obj, in_lo_desc;
{
    PGconn *conn = get_pgconn(obj);
    int lo_desc = NUM2INT(in_lo_desc);
    
    if(lo_unlink(conn,lo_desc) < 0)
		rb_raise(rb_ePGError,"lo_close failed");

    return Qnil;
}

/*
 * call-seq:
 *    conn.lo_unlink( oid ) -> nil
 *
 * Unlinks (deletes) the postgres large object of _oid_.
 */
static VALUE
pgconn_lounlink(obj, in_oid)
    VALUE obj, in_oid;
{
    PGconn *conn = get_pgconn(obj);
    int oid = NUM2INT(in_oid);
    
    if (oid < 0)
        rb_raise(rb_ePGError, "invalid oid %d",oid);

    if(lo_unlink(conn,oid) < 0)
		rb_raise(rb_ePGError,"lo_unlink failed");

    return Qnil;
}

/********************************************************************
 * 
 * Document-class: PGresult
 *
 * The class to represent the query result tuples (rows). 
 * An instance of this class is created as the result of every query.
 * You may need to invoke the #clear method of the instance when finished with
 * the result for better memory performance.
 */

/**************************************************************************
 * PGresult INSTANCE METHODS
 **************************************************************************/

/*
 * call-seq:
 *    res.result_status() -> Fixnum
 *
 * Returns the status of the query. The status value is one of:
 * * +PGRES_EMPTY_QUERY+
 * * +PGRES_COMMAND_OK+
 * * +PGRES_TUPLES_OK+
 * * +PGRES_COPY_OUT+
 * * +PGRES_COPY_IN+
 * * +PGRES_BAD_RESPONSE+
 * * +PGRES_NONFATAL_ERROR+
 * * +PGRES_FATAL_ERROR+
 */
static VALUE
pgresult_result_status(obj)
    VALUE obj;
{
    return INT2FIX(PQresultStatus(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.res_status( status ) -> String
 *
 * Returns the string representation of status +status+.
 *
*/
static VALUE
pgresult_res_status(obj,status)
    VALUE obj,status;
{
    return rb_str_new2(PQresStatus(NUM2INT(status)));
}

/*
 * call-seq:
 *    res.result_error_message() -> String
 *
 * Returns the error message of the command as a string. 
 */
static VALUE
pgresult_result_error_message(obj)
    VALUE obj;
{
    return rb_str_new2(PQresultErrorMessage(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.result_error_field(fieldcode) -> String
 *
 * Returns the individual field of an error.
 *
 * +fieldcode+ is one of:
 * * +PG_DIAG_SEVERITY+
 * * +PG_DIAG_SQLSTATE+
 * * +PG_DIAG_MESSAGE_PRIMARY+
 * * +PG_DIAG_MESSAGE_DETAIL+
 * * +PG_DIAG_MESSAGE_HINT+
 * * +PG_DIAG_STATEMENT_POSITION+
 * * +PG_DIAG_INTERNAL_POSITION+
 * * +PG_DIAG_INTERNAL_QUERY+
 * * +PG_DIAG_CONTEXT+
 * * +PG_DIAG_SOURCE_FILE+
 * * +PG_DIAG_SOURCE_LINE+
 * * +PG_DIAG_SOURCE_FUNCTION+
 */
static VALUE
pgresult_result_error_field(obj)
    VALUE obj;
{
    return rb_str_new2(PQresultErrorMessage(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.clear() -> nil
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

/*
 * call-seq:
 *    res.ntuples() -> Fixnum
 *
 * Returns the number of tuples in the query result.
 */
static VALUE
pgresult_ntuples(obj)
    VALUE obj;
{
    return INT2FIX(PQntuples(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.nfields() -> Fixnum
 *
 * Returns the number of columns in the query result.
 */
static VALUE
pgresult_nfields(obj)
    VALUE obj;
{
    return INT2NUM(PQnfields(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.fname( index ) -> String
 *
 * Returns the name of the column corresponding to _index_.
 */
static VALUE
pgresult_fname(obj, index)
    VALUE obj, index;
{
    PGresult *result;
	int i = NUM2INT(index);

    result = get_pgresult(obj);
    if (i < 0 || i >= PQnfields(result)) {
        rb_raise(rb_eArgError,"invalid field number %d", i);
    }
    return rb_tainted_str_new2(PQfname(result, i));
}

/*
 * call-seq:
 *    res.fnumber( name ) -> Fixnum
 *
 * Returns the index of the field specified by the string _name_.
 *
 * Raises an ArgumentError if the specified _name_ isn't one of the field names;
 * raises a TypeError if _name_ is not a String.
 */
static VALUE
pgresult_fnumber(obj, name)
    VALUE obj, name;
{
    int n;
    
    Check_Type(name, T_STRING);
    
    n = PQfnumber(get_pgresult(obj), StringValuePtr(name));
    if (n == -1) {
        rb_raise(rb_eArgError,"Unknown field: %s", StringValuePtr(name));
    }
    return INT2FIX(n);
}

/*
 * call-seq:
 *    res.ftable( column_number ) -> Fixnum
 *
 * Returns the Oid of the table from which the column _column_number_
 * was fetched.
 *
 * Raises ArgumentError if _column_number_ is out of range or if
 * the Oid is undefined for that column.
 */
static VALUE
pgresult_ftable(obj, column_number)
    VALUE obj, column_number;
{
	Oid n = PQftable(get_pgresult(obj), NUM2INT(column_number));
    if (n == InvalidOid) {
        rb_raise(rb_eArgError,"Oid is undefined for column: %d", 
			NUM2INT(column_number));
    }
    return INT2FIX(n);
}

/*
 * call-seq:
 *    res.ftablecol( column_number ) -> Fixnum
 *
 * Returns the column number (within its table) of the table from 
 * which the column _column_number_ is made up.
 *
 * Raises ArgumentError if _column_number_ is out of range or if
 * the column number from its table is undefined for that column.
 */
static VALUE
pgresult_ftablecol(obj, column_number)
    VALUE obj, column_number;
{
	int n = PQftablecol(get_pgresult(obj), NUM2INT(column_number));
    if (n == 0) {
        rb_raise(rb_eArgError,
			"Column number from table is undefined for column: %d", 
			NUM2INT(column_number));
    }
    return INT2FIX(n);
}

/*
 * call-seq:
 *    res.fformat( column_number ) -> Fixnum
 *
 * Returns the format (0 for text, 1 for binary) of column
 * _column_number_.
 * 
 * Raises ArgumentError if _column_number_ is out of range.
 */
static VALUE
pgresult_fformat(obj, column_number)
    VALUE obj, column_number;
{
	PGresult *result = get_pgresult(obj);
	int fnumber = NUM2INT(column_number);
    if (fnumber >= PQnfields(result)) {
        rb_raise(rb_eArgError, "Column number is out of range: %d", 
			fnumber);
    }
	return INT2FIX(PQfformat(result, fnumber));
}

/*
 * call-seq:
 *    res.ftype( column_number )
 *
 * Returns the data type associated with _column_number_.
 *
 * The integer returned is the internal +OID+ number (in PostgreSQL) of the type.
 */
static VALUE
pgresult_ftype(obj, index)
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
 *    res.fmod( column_number )
 *
 * Returns the type modifier associated with column _column_number_.
 * 
 * Raises ArgumentError if _column_number_ is out of range.
 */
static VALUE
pgresult_fmod(obj, column_number)
    VALUE obj, column_number;
{
	PGresult *result = get_pgresult(obj);
	int fnumber = NUM2INT(column_number);
	int modifier;
    if (fnumber >= PQnfields(result)) {
        rb_raise(rb_eArgError, "Column number is out of range: %d", 
			fnumber);
    }
	if((modifier = PQfmod(result,fnumber)) == -1)
		rb_raise(rb_eArgError, 
			"No modifier information available for column: %d", 
			fnumber);
	return INT2NUM(modifier);
}

/*
 * call-seq:
 *    res.fsize( index )
 *
 * Returns the size of the field type in bytes.  Returns <tt>-1</tt> if the field is variable sized.
 *
 *   res = conn.exec("SELECT myInt, myVarChar50 FROM foo")
 *   res.size(0) => 4
 *   res.size(1) => -1
 */
static VALUE
pgresult_fsize(obj, index)
    VALUE obj, index;
{
    PGresult *result;
    int i = NUM2INT(index);

    result = get_pgresult(obj);
    if (i < 0 || i >= PQnfields(result)) {
        rb_raise(rb_eArgError,"invalid field number %d", i);
    }
    return INT2NUM(PQfsize(result, i));
}

/*
 * call-seq:
 *    res.getvalue( tup_num, field_num )
 *
 * Returns the value in tuple number _tup_num_, field _field_num_. 
 */
static VALUE
pgresult_getvalue(obj, tup_num, field_num)
    VALUE obj, tup_num, field_num;
{
    PGresult *result;
    int i = NUM2INT(tup_num);
    int j = NUM2INT(field_num);

    result = get_pgresult(obj);
	if(i < 0 || i >= PQntuples(result)) {
		rb_raise(rb_eArgError,"invalid tuple number %d", i);
	}
   	if(j < 0 || j >= PQnfields(result)) {
		rb_raise(rb_eArgError,"invalid field number %d", j);
   	}
	return rb_str_new2(PQgetvalue(result, i, j));
}

/*
 * call-seq:
 *    res.getisnull(tuple_position, field_position) -> boolean
 *
 * Returns +true+ if the specified value is +nil+; +false+ otherwise.
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
 *    res.getlength( tup_num, field_num ) -> Fixnum
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
 *    res.nparams() -> Fixnum
 *
 * Returns the number of parameters of a prepared statement.
 * Only useful for the result returned by conn.describePrepared
 */
static VALUE
pgresult_nparams(obj)
    VALUE obj;
{
    PGresult *result;

    result = get_pgresult(obj);
    return INT2FIX(PQnparams(result));
}

/*
 * call-seq:
 *    res.paramtype( param_number ) -> Oid
 *
 * Returns the Oid of the data type of parameter _param_number_.
 * Only useful for the result returned by conn.describePrepared
 */
static VALUE
pgresult_paramtype(obj, param_number)
    VALUE obj, param_number;
{
    PGresult *result;

    result = get_pgresult(obj);
    return INT2FIX(PQparamtype(result,NUM2INT(param_number)));
}

/*
 * call-seq:
 *    res.cmd_status() -> String
 *
 * Returns the status string of the last query command.
 */
static VALUE
pgresult_cmd_status(obj)
    VALUE obj;
{
    return rb_tainted_str_new2(PQcmdStatus(get_pgresult(obj)));
}

/*
 * call-seq:
 *    res.cmd_tuples() -> Fixnum
 *
 * Returns the number of tuples (rows) affected by the SQL command.
 *
 * If the SQL command that generated the PGresult was not one of:
 * * +INSERT+
 * * +UPDATE+
 * * +DELETE+
 * * +MOVE+
 * * +FETCH+
 * or if no tuples were affected, <tt>0</tt> is returned.
 */
static VALUE
pgresult_cmd_tuples(obj)
    VALUE obj;
{
    long n;
    n = strtol(PQcmdTuples(get_pgresult(obj)),NULL, 10);
    return INT2NUM(n);
}

/*
 * call-seq:
 *    res.oid_value() -> Fixnum
 *
 * Returns the +oid+ of the inserted row if applicable,
 * otherwise +nil+.
 */
static VALUE
pgresult_oid_value(obj)
    VALUE obj;
{
    Oid n = PQoidValue(get_pgresult(obj));
    if (n == InvalidOid)
        return Qnil;
    else
        return INT2FIX(n);
}

/* Utility methods not in libpq */

/*
 * call-seq:
 *    res[ n ] -> Hash
 *
 * Returns tuple _n_ as a hash. 
 */
static VALUE
pgresult_aref(obj, index)
	VALUE obj, index;
{
	PGresult *result = get_pgresult(obj);
	int tuple_num = NUM2INT(index);
	int field_num;
	VALUE fname,val;
	VALUE tuple;

	tuple = rb_hash_new();
	for(field_num = 0; field_num < PQnfields(result); field_num++) {
		fname = rb_str_new2(PQfname(result,field_num));
		if(PQgetisnull(result, tuple_num, field_num)) {
			rb_hash_aset(tuple, fname, Qnil);
		}
		else {
			val = rb_tainted_str_new2(PQgetvalue(result, tuple_num, field_num));
			rb_hash_aset(tuple, fname, val);
		}
	}
	return tuple;
}

/*
 * call-seq:
 *    res.each{ |tuple| ... }
 *
 * Invokes block for each tuple in the result set.
 */
static VALUE
pgresult_each(obj)
	VALUE obj;
{
	PGresult *result = get_pgresult(obj);
	int tuple_num;

	for(tuple_num = 0; tuple_num < PQntuples(result); tuple_num++) {
		rb_yield(pgresult_aref(obj, INT2NUM(tuple_num)));
	}
	return obj;
}

/*
 * call-seq:
 *    res.fields() -> Array
 *
 * Returns an array of Strings representing the names of the fields in the result.
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



/**************************************************************************/
 

void
Init_pg()
{
    pg_gsub_bang_id = rb_intern("gsub!");
    pg_escape_regex = rb_reg_new("([\\t\\n\\\\])", 10, 0);
    rb_global_variable(&pg_escape_regex);
    pg_escape_str = rb_str_new("\\\\\\1", 4);
    rb_global_variable(&pg_escape_str);

    rb_ePGError = rb_define_class("PGError", rb_eStandardError);
    rb_cPGconn = rb_define_class("PGconn", rb_cObject);
    rb_cPGresult = rb_define_class("PGresult", rb_cObject);


	
	/*************************
	 *  PGError 
	 *************************/
    rb_define_alias(rb_ePGError, "error", "message");
	rb_define_attr(rb_ePGError, "connection", 1, 0);
	rb_define_attr(rb_ePGError, "result", 1, 0);

	/*************************
	 *  PGconn 
	 *************************/
#ifdef HAVE_RB_DEFINE_ALLOC_FUNC
    rb_define_alloc_func(rb_cPGconn, pgconn_alloc);
#else
    rb_define_singleton_method(rb_cPGconn, "new", pgconn_s_new, -1);
#endif  
    rb_define_singleton_alias(rb_cPGconn, "connect", "new");
    rb_define_singleton_alias(rb_cPGconn, "open", "new");
    rb_define_singleton_alias(rb_cPGconn, "setdb", "new");
    rb_define_singleton_alias(rb_cPGconn, "setdblogin", "new");
    rb_define_singleton_alias(rb_cPGconn, "open", "new");
    rb_define_singleton_method(rb_cPGconn, "escape_string", pgconn_s_escape, 1);
	rb_define_singleton_alias(rb_cPGconn, "escape", "escape_string");
    rb_define_singleton_method(rb_cPGconn, "escape_bytea", pgconn_s_escape_bytea, 1);
    rb_define_singleton_method(rb_cPGconn, "unescape_bytea", pgconn_s_unescape_bytea, 1);
    rb_define_singleton_method(rb_cPGconn, "isthreadsafe", pgconn_s_isthreadsafe, 0);
    rb_define_singleton_method(rb_cPGconn, "encrypt_password", pgconn_s_encrypt_password, 0);

	/******     CONSTANTS      ******/

	/* Connection Status */
    rb_define_const(rb_cPGconn, "CONNECTION_OK", INT2FIX(CONNECTION_OK));
    rb_define_const(rb_cPGconn, "CONNECTION_BAD", INT2FIX(CONNECTION_BAD));

	/* Connection Status of nonblocking connections */
	rb_define_const(rb_cPGconn, "CONNECTION_STARTED", INT2FIX(CONNECTION_STARTED));
	rb_define_const(rb_cPGconn, "CONNECTION_MADE", INT2FIX(CONNECTION_MADE));
	rb_define_const(rb_cPGconn, "CONNECTION_AWAITING_RESPONSE", INT2FIX(CONNECTION_AWAITING_RESPONSE));
	rb_define_const(rb_cPGconn, "CONNECTION_AUTH_OK", INT2FIX(CONNECTION_AUTH_OK));
	rb_define_const(rb_cPGconn, "CONNECTION_SSL_STARTUP", INT2FIX(CONNECTION_SSL_STARTUP));
	rb_define_const(rb_cPGconn, "CONNECTION_SETENV", INT2FIX(CONNECTION_SETENV));
	
	/* Nonblocking connection polling status */
	rb_define_const(rb_cPGconn, "PGRES_POLLING_READING", INT2FIX(PGRES_POLLING_READING));
	rb_define_const(rb_cPGconn, "PGRES_POLLING_WRITING", INT2FIX(PGRES_POLLING_WRITING));
	rb_define_const(rb_cPGconn, "PGRES_POLLING_FAILED", INT2FIX(PGRES_POLLING_FAILED));
	rb_define_const(rb_cPGconn, "PGRES_POLLING_OK", INT2FIX(PGRES_POLLING_OK));

	/* Transaction Status */
	rb_define_const(rb_cPGconn, "PQTRANS_IDLE", INT2FIX(PQTRANS_IDLE));
	rb_define_const(rb_cPGconn, "PQTRANS_ACTIVE", INT2FIX(PQTRANS_ACTIVE));
	rb_define_const(rb_cPGconn, "PQTRANS_INTRANS", INT2FIX(PQTRANS_INTRANS));
	rb_define_const(rb_cPGconn, "PQTRANS_INERROR", INT2FIX(PQTRANS_INERROR));
	rb_define_const(rb_cPGconn, "PQTRANS_UNKNOWN", INT2FIX(PQTRANS_UNKNOWN));

	/* Large Objects */
    rb_define_const(rb_cPGconn, "INV_WRITE", INT2FIX(INV_WRITE));
    rb_define_const(rb_cPGconn, "INV_READ", INT2FIX(INV_READ));
    rb_define_const(rb_cPGconn, "SEEK_SET", INT2FIX(SEEK_SET));
    rb_define_const(rb_cPGconn, "SEEK_CUR", INT2FIX(SEEK_CUR));
    rb_define_const(rb_cPGconn, "SEEK_END", INT2FIX(SEEK_END));
    
	/******     INSTANCE METHODS      ******/

	/* Connection Control */
    rb_define_method(rb_cPGconn, "initialize", pgconn_init, -1);
    rb_define_method(rb_cPGconn, "finish", pgconn_finish, 0);
    rb_define_alias(rb_cPGconn, "close", "finish");
    rb_define_method(rb_cPGconn, "reset", pgconn_reset, 0);

	/* Connection Status Functions */
    rb_define_method(rb_cPGconn, "db", pgconn_db, 0);
    rb_define_method(rb_cPGconn, "user", pgconn_user, 0);
    rb_define_method(rb_cPGconn, "pass", pgconn_pass, 0);
    rb_define_method(rb_cPGconn, "host", pgconn_host, 0);
    rb_define_method(rb_cPGconn, "port", pgconn_port, 0);
    rb_define_method(rb_cPGconn, "tty", pgconn_tty, 0);
    rb_define_method(rb_cPGconn, "options", pgconn_options, 0);
    rb_define_method(rb_cPGconn, "status", pgconn_status, 0);
    rb_define_method(rb_cPGconn, "transaction_status", pgconn_transaction_status, 0);
    rb_define_method(rb_cPGconn, "parameter_status", pgconn_parameter_status, 1);
    rb_define_method(rb_cPGconn, "protocol_version", pgconn_protocol_version, 0);
    rb_define_method(rb_cPGconn, "server_version", pgconn_server_version, 0);
    rb_define_method(rb_cPGconn, "error_message", pgconn_error_message, 0);
    //rb_define_method(rb_cPGconn, "socket", pgconn_socket, 0);
    rb_define_method(rb_cPGconn, "backend_pid", pgconn_backend_pid, 0);
    rb_define_method(rb_cPGconn, "connection_used_password", pgconn_connection_used_password, 0);
    //rb_define_method(rb_cPGconn, "getssl", pgconn_getssl, 0);

	/* Command Execution Functions */
    rb_define_method(rb_cPGconn, "exec", pgconn_exec, 1);
    rb_define_method(rb_cPGconn, "exec_params", pgconn_exec_params, -1);
    rb_define_method(rb_cPGconn, "prepare", pgconn_prepare, -1);
    rb_define_method(rb_cPGconn, "exec_prepared", pgconn_exec_prepared, -1);
    rb_define_method(rb_cPGconn, "describe_prepared", pgconn_prepare, -1);
    rb_define_method(rb_cPGconn, "describe_portal", pgconn_exec_prepared, -1);
    rb_define_method(rb_cPGconn, "escape_string", pgconn_s_escape, 1);
	rb_define_alias(rb_cPGconn, "escape", "escape_string");
    rb_define_method(rb_cPGconn, "escape_bytea", pgconn_s_escape_bytea, 1);
    rb_define_method(rb_cPGconn, "unescape_bytea", pgconn_s_unescape_bytea, 1);
 
	/* Asynchronous Command Processing */
    rb_define_method(rb_cPGconn, "send_query", pgconn_send_query, 0);
    //rb_define_method(rb_cPGconn, "send_query_params", pgconn_send_query_params, 0);
    rb_define_method(rb_cPGconn, "send_prepare", pgconn_send_prepare, 0);
    //rb_define_method(rb_cPGconn, "send_query_prepared", pgconn_send_query_prepared, 0);
    rb_define_method(rb_cPGconn, "send_describe_prepared", pgconn_send_describe_prepared, 0);
    rb_define_method(rb_cPGconn, "send_describe_portal", pgconn_send_describe_portal, 0);
    rb_define_method(rb_cPGconn, "get_result", pgconn_get_result, 0);
    rb_define_method(rb_cPGconn, "consume_input", pgconn_consume_input, 0);
    rb_define_method(rb_cPGconn, "is_busy", pgconn_is_busy, 0);
    rb_define_method(rb_cPGconn, "setnonblocking", pgconn_setnonblocking, 1);
    rb_define_method(rb_cPGconn, "isnonblocking", pgconn_isnonblocking, 0);
    rb_define_method(rb_cPGconn, "flush", pgconn_flush, 0);

	/* Cancelling Queries in Progress */
	//rb_define_method(rb_cPGconn, "get_cancel", pgconn_get_result, 0);
	//rb_define_method(rb_cPGconn, "free_cancel", pgconn_get_result, 0);
	//rb_define_method(rb_cPGconn, "cancel", pgconn_get_result, 0);

	/* Fast-Path Interface */
    //rb_define_method(rb_cPGconn, "fn", pgconn_fn, 0);

	/* NOTIFY */
    rb_define_method(rb_cPGconn, "notifies", pgconn_notifies, 0);

	/* COPY */
    rb_define_method(rb_cPGconn, "put_copy_data", pgconn_put_copy_data, 1);
    //rb_define_method(rb_cPGconn, "put_copy_end", pgconn_put_copy_end, 1);
    //rb_define_method(rb_cPGconn, "get_copy_data", pgconn_get_copy_data, 2);

	/* Control Functions */
    //rb_define_method(rb_cPGconn, "set_error_verbosity", pgconn_set_error_verbosity, 0);
    rb_define_method(rb_cPGconn, "trace", pgconn_trace, 1);
    rb_define_method(rb_cPGconn, "untrace", pgconn_untrace, 0);

	/* Notice Processing */
    //rb_define_method(rb_cPGconn, "set_notice_receiver", pgconn_set_notice_receiver, 0);
    rb_define_method(rb_cPGconn, "set_notice_processor", pgconn_set_notice_processor, 0);

	/* TODO Other */
    rb_define_method(rb_cPGconn, "client_encoding", pgconn_client_encoding, 0);
    rb_define_method(rb_cPGconn, "set_client_encoding", pgconn_set_client_encoding, 1);

    /* Large Object support */
    rb_define_method(rb_cPGconn, "lo_creat", pgconn_locreat, -1);
    rb_define_alias(rb_cPGconn, "locreat", "lo_creat");
    rb_define_method(rb_cPGconn, "lo_create", pgconn_locreate, 1);
    rb_define_alias(rb_cPGconn, "locreate", "lo_create");
    rb_define_method(rb_cPGconn, "lo_import", pgconn_loimport, 1);
    rb_define_alias(rb_cPGconn, "loimport", "lo_import");
    rb_define_method(rb_cPGconn, "lo_export", pgconn_loexport, 2);
    rb_define_alias(rb_cPGconn, "loexport", "lo_export");
    rb_define_method(rb_cPGconn, "lo_open", pgconn_loopen, -1);
    rb_define_alias(rb_cPGconn, "loopen", "lo_open");
    rb_define_method(rb_cPGconn, "lo_write",pgconn_lowrite, 2);
	rb_define_alias(rb_cPGconn, "lowrite", "lo_write");
    rb_define_method(rb_cPGconn, "lo_read",pgconn_loread, 2);
	rb_define_alias(rb_cPGconn, "loread", "lo_read");
    rb_define_method(rb_cPGconn, "lo_lseek",pgconn_lolseek, 3);
	rb_define_alias(rb_cPGconn, "lolseek", "lo_lseek");
	rb_define_alias(rb_cPGconn, "lo_seek", "lo_lseek");
	rb_define_alias(rb_cPGconn, "loseek", "lo_lseek");
    rb_define_method(rb_cPGconn, "lo_tell",pgconn_lotell, 1);
	rb_define_alias(rb_cPGconn, "lotell", "lo_tell");
	rb_define_method(rb_cPGconn, "lo_truncate", pgconn_lotruncate, 2);
    rb_define_alias(rb_cPGconn, "lotruncate", "lo_truncate");
    rb_define_method(rb_cPGconn, "lo_close",pgconn_loclose, 1);
    rb_define_alias(rb_cPGconn, "loclose", "lo_close");
    rb_define_method(rb_cPGconn, "lo_unlink", pgconn_lounlink, 1);
    rb_define_alias(rb_cPGconn, "lounlink", "lo_unlink");
    
	/*************************
	 *  PGresult 
	 *************************/

    rb_include_module(rb_cPGresult, rb_mEnumerable);

	/******     CONSTANTS      ******/

	/* result status */
    rb_define_const(rb_cPGresult, "PGRES_EMPTY_QUERY", INT2FIX(PGRES_EMPTY_QUERY));
    rb_define_const(rb_cPGresult, "PGRES_COMMAND_OK", INT2FIX(PGRES_COMMAND_OK));
    rb_define_const(rb_cPGresult, "PGRES_TUPLES_OK", INT2FIX(PGRES_TUPLES_OK));
    rb_define_const(rb_cPGresult, "PGRES_COPY_OUT", INT2FIX(PGRES_COPY_OUT));
    rb_define_const(rb_cPGresult, "PGRES_COPY_IN", INT2FIX(PGRES_COPY_IN));
    rb_define_const(rb_cPGresult, "PGRES_BAD_RESPONSE", INT2FIX(PGRES_BAD_RESPONSE));
    rb_define_const(rb_cPGresult, "PGRES_NONFATAL_ERROR",INT2FIX(PGRES_NONFATAL_ERROR));
    rb_define_const(rb_cPGresult, "PGRES_FATAL_ERROR", INT2FIX(PGRES_FATAL_ERROR));

	/* result error field codes */
	rb_define_const(rb_cPGresult, "PG_DIAG_SEVERITY", INT2FIX(PG_DIAG_SEVERITY));
	rb_define_const(rb_cPGresult, "PG_DIAG_SQLSTATE", INT2FIX(PG_DIAG_SQLSTATE));
	rb_define_const(rb_cPGresult, "PG_DIAG_MESSAGE_PRIMARY", INT2FIX(PG_DIAG_MESSAGE_PRIMARY));
	rb_define_const(rb_cPGresult, "PG_DIAG_MESSAGE_DETAIL", INT2FIX(PG_DIAG_MESSAGE_DETAIL));
	rb_define_const(rb_cPGresult, "PG_DIAG_MESSAGE_HINT", INT2FIX(PG_DIAG_MESSAGE_HINT));
	rb_define_const(rb_cPGresult, "PG_DIAG_STATEMENT_POSITION", INT2FIX(PG_DIAG_STATEMENT_POSITION));
	rb_define_const(rb_cPGresult, "PG_DIAG_INTERNAL_POSITION", INT2FIX(PG_DIAG_INTERNAL_POSITION));
	rb_define_const(rb_cPGresult, "PG_DIAG_INTERNAL_QUERY", INT2FIX(PG_DIAG_INTERNAL_QUERY));
	rb_define_const(rb_cPGresult, "PG_DIAG_CONTEXT", INT2FIX(PG_DIAG_CONTEXT));
	rb_define_const(rb_cPGresult, "PG_DIAG_SOURCE_FILE", INT2FIX(PG_DIAG_SOURCE_FILE));
	rb_define_const(rb_cPGresult, "PG_DIAG_SOURCE_LINE", INT2FIX(PG_DIAG_SOURCE_LINE));
	rb_define_const(rb_cPGresult, "PG_DIAG_SOURCE_FUNCTION", INT2FIX(PG_DIAG_SOURCE_FUNCTION));

	/******     INSTANCE METHODS: libpq     ******/

    rb_define_method(rb_cPGresult, "result_status", pgresult_result_status, 0);
    rb_define_method(rb_cPGresult, "res_status", pgresult_res_status, 0);
    rb_define_method(rb_cPGresult, "result_error_message", pgresult_result_error_message, 0);
    rb_define_method(rb_cPGresult, "result_error_field", pgresult_result_error_field, 0);
    rb_define_method(rb_cPGresult, "ntuples", pgresult_ntuples, 0);
    rb_define_method(rb_cPGresult, "nfields", pgresult_nfields, 0);
    rb_define_method(rb_cPGresult, "fname", pgresult_fname, 1);
    rb_define_method(rb_cPGresult, "fnumber", pgresult_fnumber, 1);
    rb_define_method(rb_cPGresult, "ftable", pgresult_ftable, 1);
    rb_define_method(rb_cPGresult, "ftablecol", pgresult_ftablecol, 1);
    rb_define_method(rb_cPGresult, "fformat", pgresult_fformat, 1);
    rb_define_method(rb_cPGresult, "ftype", pgresult_ftype, 1);
    rb_define_method(rb_cPGresult, "fmod", pgresult_fmod, 1);
    rb_define_method(rb_cPGresult, "fsize", pgresult_fsize, 1);
    rb_define_method(rb_cPGresult, "getvalue", pgresult_getvalue, 2);
    rb_define_method(rb_cPGresult, "getisnull", pgresult_getisnull, 2);
    rb_define_method(rb_cPGresult, "getlength", pgresult_getlength, 2);
	rb_define_method(rb_cPGresult, "nparams", pgresult_nparams, 0);
	rb_define_method(rb_cPGresult, "paramtype", pgresult_paramtype, 0);
	rb_define_method(rb_cPGresult, "cmd_status", pgresult_cmd_status, 0);
	rb_define_method(rb_cPGresult, "cmd_tuples", pgresult_cmd_tuples, 0);
	rb_define_method(rb_cPGresult, "oid_value", pgresult_oid_value, 0);
    rb_define_method(rb_cPGresult, "clear", pgresult_clear, 0);

	/******     INSTANCE METHODS: other     ******/
    rb_define_method(rb_cPGresult, "[]", pgresult_aref, 1);
    rb_define_method(rb_cPGresult, "each", pgresult_each, 0);
    rb_define_method(rb_cPGresult, "fields", pgresult_fields, 0);

}
