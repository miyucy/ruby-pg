
static VALUE 
parse_composite(VALUE conn, char * string, VALUE tupdesc)
{
  char * ptr = string;
	int    needComma = 0;
  int    i;
  VALUE  values=rb_ary_new();
  VALUE  attrs = rb_iv_get(tupdesc, "@attributes");
  char * buffer = ALLOCA_N(char, strlen(string)+1);
  char * bufstart = buffer;
    
  ++ptr;
	for (i = 0; i < RARRAY(attrs)->len ; i++)
	{
	  if (needComma)
		{
			/* Skip comma that separates prior field from this one */
			if (*ptr == ',')
				ptr++;
		}

		/* Check for null: completely empty input means null */
		if (*ptr == ',' || *ptr == ')')
		{
			rb_ary_push(values, Qnil);
		}
    else
		{
			/* Extract string for this column */
			int		inquote = 0;

      bufstart = buffer;
			while (inquote || !(*ptr == ',' || *ptr == ')'))
			{
				char ch = *ptr++;
        if (ch == '\0')
          rb_raise(rb_eArgError, "can't parse record: unexpected end of string");
				if (ch == '\\')
				{
          *bufstart++ = *ptr++;
				}
				else if (ch == '\"')
				{
					if (!inquote)
						inquote = 1;
					else if (*ptr == '\"')
					{
            *bufstart++ = *ptr++;
						/* doubled quote within quote sequence */
					}
					else
						inquote = 0;
				}
				else
          *bufstart++ = ch;
			}

      Oid _tip = NUM2INT(rb_iv_get( rb_ary_entry(attrs, i), "@type_oid"));
      int _mod = NUM2INT(rb_iv_get( rb_ary_entry(attrs, i), "@type_mod"));
      *bufstart++ =0;
      VALUE parsed_val = parse_string(conn, buffer, _tip, _mod);
      rb_ary_push(values, parsed_val);
		}

		/*
		 * Prep for next column
		 */
		needComma = 1;
	}

  VALUE params[2] = { tupdesc, values };
  VALUE ob = rb_class_new_instance(2, params, rb_cPGrecord);
  return ob;
}
