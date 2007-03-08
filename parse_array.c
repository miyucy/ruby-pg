// for isspace()
#include <ctype.h> 

#define MAXDIM 6

// This OIDs for array types were resolved via
//  grep 'array' postgresql/src/include/catalog/pg_type.h | grep OID
#include "array_oids.h"

/**
 * convert string to ruby class value basing on given typeoid, typmod and pushing it to array.
 */
static void 
emit_item(conn, arrayStr, array, typeoid, typmod)
    VALUE conn;
    char * arrayStr;
    VALUE array;
    Oid typeoid;
    int typmod;
{
    rb_ary_push(array,std_single_value(conn, arrayStr, typeoid, typmod) );
}

/**
 * Code of  pg_parse_array() function is a modified version of ReadArrayStr function, found in : 
 * $PostgreSQL: pgsql/src/backend/utils/adt/arrayfuncs.c,v 1.123 2005/10/15 02:49:27 momjian Exp $
 * 
 * This function takes string representation of array, returned from PostgreSQL query and 
 * parses it, resulting in ruby array of ruby class values.
 * Types are resolved using given type OID and typmod. 
 *
 * Note: dimension descriptions are parsed, and dimesion string is set as instance var @dimestions to 
 * resulting array
 */
static VALUE 
pg_parse_array(conn, arrayStr, typeoid, typmod)
    VALUE conn;
    char* arrayStr;
    Oid typeoid;
    int typmod;
{
  int   i,  nest_level = 0, ndim = 0;
  char  *srcptr;
  int   in_quotes = 0;
  int   eoArray = 0;
  char  *copy = ALLOCA_N(char, strlen(arrayStr)+1);
  char  *p;
  VALUE     arrays[MAXDIM], dim_value = Qnil, result;
  strcpy(copy, arrayStr);

  p = srcptr = copy;

        arrays[0] = rb_ary_new();
	for (;;)
	{
		char   *q;
		while (isspace((unsigned char) *p))
			p++;
		if (*p != '[')
			break;				/* no more dimension items */
		p++;
		if (ndim >= MAXDIM)
			rb_raise(rb_eArgError, "number of array dimensions (%d) exceeds the maximum allowed (%d)",
							ndim, MAXDIM);

		for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++);
		if (q == p)				/* no digits? */
		        rb_raise(rb_eArgError, "missing dimension value");
		if (*q == ':')
		{
			/* [m:n] format */
			p = q + 1;
			for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++);
			if (q == p)			/* no digits? */
                                rb_raise(rb_eArgError, "missing dimension value");
		}
		if (*q != ']')
			rb_raise(rb_eArgError, "missing \"]\" in array dimensions");
		p = q + 1;
                ndim++;
	}

        if(ndim == 0){
                if('{' != *p) 
                        rb_raise(rb_eArgError, "array value must start with \"{\" or dimension information");
        }else {
               if (*p != '=')
                        rb_raise(rb_eArgError, "missing assignment operator");
               p += 1;
               while (isspace((unsigned char) *p))
                  p++;
               dim_value = rb_str_new(srcptr, p-srcptr);
        }
        srcptr=p;
        
	while (!eoArray)
	{
		int		itemdone = 0;
		int		leadingspace = 1;
		char	*itemstart;
		char	*dstptr;
		char	*dstendptr;
                int   showed = 0;
		itemstart = dstptr = dstendptr = srcptr;
                i = -1;
		while (!itemdone)
		{
			switch (*srcptr)
			{
				case '\0':
					rb_raise(rb_eArgError, "can't parse array : unexpected end of string!");
				case '\\':
					srcptr++;
					*dstptr++ = *srcptr++;
					leadingspace = 0;
					dstendptr = dstptr;
					break;
				case '\"':
					in_quotes = !in_quotes;
					if (in_quotes)
						leadingspace = 0;
					else
						dstendptr = dstptr;
					srcptr++;
					break;
				case '{':
					if (!in_quotes)
					{
						nest_level++;
						srcptr++;
                                                rb_ary_push( arrays[nest_level-1], arrays[nest_level]=rb_ary_new());
					}
					else
						*dstptr++ = *srcptr++;
					break;
				case '}':
					if (!in_quotes)
					{
						nest_level--;
                                                if(!showed){
                                                        *dstendptr = '\0';
                                                        emit_item(conn, itemstart, arrays[nest_level+1] , typeoid, typmod);
                                                }
                                                showed = 1;
                                                if (nest_level == 0)
							eoArray = itemdone = 1;
						else
						        srcptr++;
					}
					else
						*dstptr++ = *srcptr++;
					break;
				default:
					if (in_quotes)
						*dstptr++ = *srcptr++;
					else if (*srcptr == ',')
					{
						itemdone = 1;
 						srcptr++;
					}
					else if (isspace((unsigned char) *srcptr))
					{
						if (leadingspace)
							srcptr++;
						else
							*dstptr++ = *srcptr++;
					}
					else
					{
						*dstptr++ = *srcptr++;
						leadingspace = 0;
						dstendptr = dstptr;
					}
					break;
			}
		}
                if(!showed) {
                        *dstendptr = '\0';
                        emit_item(conn, itemstart, arrays[nest_level], typeoid, typmod);
                }
	}
  result = rb_ary_entry(arrays[0], 0);
  if(Qnil != dim_value) 
        rb_iv_set(result, "@dimensions", dim_value);
  return result;
}

