#include "ruby.h"
#include "rubyio.h"
#include "st.h"

#include "compat.h"

/* grep '^#define' $(pg_config --includedir)/server/catalog/pg_type.h | grep OID */
#include "type-oids.h"
#include <libpq-fe.h>
#include <libpq/libpq-fs.h>              /* large-object interface */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

void Init_pg(void);

/* Large Object support */
typedef struct pglarge_object
{
    PGconn *pgconn;
    Oid lo_oid;
    int lo_fd;
} PGlarge;

