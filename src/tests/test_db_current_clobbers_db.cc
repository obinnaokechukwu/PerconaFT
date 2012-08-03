/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007 Tokutek Inc.  All rights reserved."
#include "test.h"

/* DB_CURRENT */

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <db.h>


// ENVDIR is defined in the Makefile

DB_ENV *env;
DB *db;
DB_TXN* null_txn = NULL;

int
test_main (int UU(argc), char UU(*const argv[])) {
    int r;
    r = system("rm -rf " ENVDIR);
    CKERR(r);
    r=toku_os_mkdir(ENVDIR, S_IRWXU+S_IRWXG+S_IRWXO);    CKERR(r);
    r=db_env_create(&env, 0); CKERR(r);
    r=env->open(env, ENVDIR, DB_PRIVATE|DB_INIT_MPOOL|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);
    r = db->open(db, null_txn, "foo.db", "main", DB_BTREE, DB_CREATE, 0666); CKERR(r);
    DBC *cursor;
    r = db->cursor(db, null_txn, &cursor, 0); CKERR(r);
    DBT key, val;
    DBT ckey, cval;
    int k1 = 1, v1=7;
    enum foo { blob = 1 };
    int k2 = 2;
    int v2 = 8;
    r = db->put(db, null_txn, dbt_init(&key, &k1, sizeof(k1)), dbt_init(&val, &v1, sizeof(v1)), 0);
        CKERR(r);
    r = db->put(db, null_txn, dbt_init(&key, &k2, sizeof(k2)), dbt_init(&val, &v2, sizeof(v2)), 0);
        CKERR(r);

    r = cursor->c_get(cursor, dbt_init(&ckey, NULL, 0), dbt_init(&cval, NULL, 0), DB_LAST); 
        CKERR(r);
    //Copies a static pointer into val.
    r = db->get(db, null_txn, dbt_init(&key, &k1, sizeof(k1)), dbt_init(&val, NULL, 0), 0);
        CKERR(r);
    assert(val.data != &v1);
    assert(*(int*)val.data == v1);

    r = cursor->c_get(cursor, dbt_init(&ckey, NULL, 0), dbt_init(&cval, NULL, 0), DB_LAST); 
        CKERR(r);

    //Does not corrupt it.
    assert(val.data != &v1);
    assert(*(int*)val.data == v1);

    r = cursor->c_get(cursor, &ckey, &cval, DB_CURRENT); 
        CKERR(r);

    assert(*(int*)val.data == v1); // Will bring up valgrind error.


    r = db->del(db, null_txn, &ckey, DB_DELETE_ANY); assert(r == 0);
        CKERR(r);

    assert(*(int*)val.data == v1); // Will bring up valgrind error.

    r = cursor->c_close(cursor);
        CKERR(r);
    r=db->close(db, 0);       CKERR(r);
    r=env->close(env, 0);     CKERR(r);
    return 0;
}
