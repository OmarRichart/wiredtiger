/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curversion_set_key --
 *     WT_CURSOR->set_key implementation for version cursors.
 */
static void
__curversion_set_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *table_cursor;
    WT_CURSOR_VERSION *version_cursor;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    table_cursor = version_cursor->table_cursor;
    table_cursor->set_key(table_cursor);
}

/*
 * __curversion_get_key --
 *     WT_CURSOR->get_key implementation for version cursors.
 */
static int
__curversion_get_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *table_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    va_list ap;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    table_cursor = version_cursor->table_cursor;
    va_start(ap, cursor);
    WT_ERR(__wt_cursor_get_keyv(cursor, cursor->flags, ap));
    WT_ERR(__wt_cursor_get_keyv(table_cursor, table_cursor->flags, ap));

err:
    va_end(ap);
    return (ret);
}

/*
 * __curversion_next --
 *     WT_CURSOR->next method for version cursors. The next function will position the cursor on the
 *     next update of the key it is positioned at. We traverse through updates on the update chain,
 *     then the ondisk value, and finally from the history store.
 */
static int
__curversion_next(WT_CURSOR *cursor)
{
    WT_CURSOR *hs_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_UPDATE *upd;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    hs_cursor = version_cursor->hs_cursor;
    CURSOR_API_CALL(cursor, session, next, NULL);

    /* The cursor should be positioned. */
    if (!F_ISSET(cursor, WT_CURSTD_KEY_INT))
        goto err;

    /*
     * - If there are updates in the update chain that have not been traversed,
     * return the next one.
     * - If there are no updates or all the updates have been exhausted,
     * return the ondisk value.
     * - Once the ondisk value is exhausted, check if there are any values
     * in the history store.
     */
    upd = version_cursor->next_upd;
    if (upd != NULL && !F_ISSET(version_cursor, WT_VERSION_CUR_UPDATE_EXHAUSTED)) {
        upd = upd->next;
        /* Set the record's metadata in the version cursor. */
        __wt_cursor_set_key(cursor);

        if (upd == NULL)
            F_SET(version_cursor, WT_VERSION_CUR_UPDATE_EXHAUSTED);
    } else if (!F_ISSET(version_cursor, WT_VERSION_CUR_ON_DISK_EXHAUSTED)) {
        __wt_cursor_set_key(cursor);
        F_SET(version_cursor, WT_VERSION_CUR_ON_DISK_EXHAUSTED);
    } else if (!F_ISSET(version_cursor, WT_VERSION_CUR_HS_EXAUSTED)) {
        /* Use the history store cursor to position on the key. */
        hs_cursor->next(hs_cursor);
        __wt_cursor_set_key(cursor);

        if (ret == WT_NOTFOUND)
            F_SET(version_cursor, WT_VERSION_CUR_HS_EXAUSTED);
    } else {
        /* We have exhausted all versions of the key. */
        ret = WT_NOTFOUND;
    }

    if (0) {
err:
        WT_TRET(cursor->reset(cursor));
    }
    API_END_RET(session, ret);
}

/*
 * __curversion_reset --
 *     WT_CURSOR::reset for version cursors.
 */
static int
__curversion_reset(WT_CURSOR *cursor)
{
    WT_CURSOR *hs_cursor, *table_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    hs_cursor = version_cursor->hs_cursor;
    table_cursor = version_cursor->table_cursor;
    CURSOR_API_CALL(cursor, session, reset, NULL);

    if (table_cursor != NULL)
        WT_TRET(table_cursor->reset(table_cursor));
    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->reset(hs_cursor));
    version_cursor->next_upd = NULL;
    version_cursor->flags = 0;
    F_CLR(cursor, WT_CURSTD_KEY_SET);
    F_CLR(cursor, WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __curversion_search --
 *     WT_CURSOR->search method for version cursors.
 */
static int
__curversion_search(WT_CURSOR *cursor)
{
    WT_CURSOR *table_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;
    WT_UPDATE *upd;
    bool key_only;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    table_cursor = version_cursor->table_cursor;
    key_only = F_ISSET(cursor, WT_CURSTD_KEY_ONLY);

    /*
     * For now, we assume that we are using simple cursors only.
     */
    cbt = (WT_CURSOR_BTREE *)table_cursor;
    CURSOR_API_CALL(cursor, session, search, CUR2BT(cbt));
    WT_ERR(__cursor_checkkey(table_cursor));

    /* Do a search and position on they key if it is found */
    F_SET(cursor, WT_CURSTD_KEY_ONLY);
    WT_ERR(__wt_btcur_search(cbt));
    if (!F_ISSET(cbt, WT_CURSTD_KEY_SET))
        return (WT_NOTFOUND);

    /*
     * If we position on a key, set next update of the version cursor to be the first update on the
     * key if any.
     */
    page = cbt->ref->page;
    rip = &page->pg_row[cbt->slot];
    if (cbt->ins != NULL)
        version_cursor->next_upd = ins->upd;
    else if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
        version_cursor->next_upd = upd;
    else {
        version_cursor->next_upd = NULL;
        F_SET(version_cursor, WT_VERSION_CUR_UPDATE_EXHAUSTED);
    }

err:
    if (!key_only)
        F_CLR(cursor, WT_CURSTD_KEY_ONLY);
    API_END_RET(session, ret);
}

/*
 * __curversion_close --
 *     WT_CURSOR->close method for version cursors.
 */
static int
__curversion_close(WT_CURSOR *cursor)
{
    WT_CURSOR *hs_cursor, *table_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    hs_cursor = version_cursor->hs_cursor;
    table_cursor = version_cursor->table_cursor;
    CURSOR_API_CALL(cursor, session, close, NULL);
err:
    version_cursor->next_upd = NULL;
    if (table_cursor != NULL)
        WT_TRET(table_cursor->close(table_cursor));
    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->close(hs_cursor));
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __wt_curversion_open --
 *     Initialize a version cursor.
 */
int
__wt_curversion_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __curversion_get_key, /* get-key */
      __wt_cursor_get_value,                           /* get-value */
      __curversion_set_key,                            /* set-key */
      __wt_cursor_set_value_notsup,                    /* set-value */
      __wt_cursor_compare_notsup,                      /* compare */
      __wt_cursor_equals_notsup,                       /* equals */
      __curversion_next,                               /* next */
      __wt_cursor_notsup,                              /* prev */
      __curversion_reset,                              /* reset */
      __curversion_search,                             /* search */
      __wt_cursor_search_near_notsup,                  /* search-near */
      __wt_cursor_notsup,                              /* insert */
      __wt_cursor_modify_notsup,                       /* modify */
      __wt_cursor_notsup,                              /* update */
      __wt_cursor_notsup,                              /* remove */
      __wt_cursor_notsup,                              /* reserve */
      __wt_cursor_reconfigure_notsup,                  /* reconfigure */
      __wt_cursor_notsup,                              /* largest_key */
      __wt_cursor_notsup,                              /* cache */
      __wt_cursor_reopen_notsup,                       /* reopen */
      __curversion_close);                             /* close */

    WT_CURSOR *cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    /* The table cursor is read only. */
    const char *table_cursor_cfg[] = {
      WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "read_only=true", NULL};

    *cursorp = NULL;
    WT_RET(__wt_calloc_one(session, &version_cursor));
    cursor = (WT_CURSOR *)version_cursor;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;

    /* Open the table cursor. */
    WT_ERR(__wt_open_cursor(session, uri, cursor, table_cursor_cfg, &version_cursor->table_cursor));
    cursor->key_format = WT_UNCHECKED_STRING(QQQQQQBBB);
    cursor->value_format = version_cursor->table_cursor->value_format;
    WT_ERR(__wt_strdup(session, uri, &cursor->uri));

    /* Open the history store cursor for operations on the regular history store .*/
    WT_ERR(__wt_curhs_open(session, cursor, &version_cursor->hs_cursor));

    WT_ERR(__wt_cursor_init(cursor, cursor->uri, owner, cfg, cursorp));

    if (0) {
err:
        WT_TRET(cursor->close(cursor));
        *cursorp = NULL;
    }
    return (ret);
}
