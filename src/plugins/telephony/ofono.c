/*
 * Copyright (c) 2012, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdarg.h>
#include <string.h>
#include <murphy/common.h>
#include <murphy/core.h>
#include "ofono.h"


/**
 * oFono listener for Murphy
 *
 * For tracking voice calls handled by oFono, first all modems are listed and
 * tracked. Then, for each modem object which has VoiceCallManager interface,
 * VoiceCall objects are tracked and call information is updated to Murphy DB
 * using the provided listener callback (see telephony.c).
 */

#define _NOTIFY_MDB         1

#define OFONO_DBUS          "system"
#define OFONO_DBUS_PATH     "/org/ofono/"
#define OFONO_SERVICE       "org.ofono"
#define OFONO_MODEM_MGR     "org.ofono.Manager"
#define OFONO_MODEM         "org.ofono.Modem"
#define OFONO_CALL_MGR      "org.ofono.VoiceCallManager"
#define OFONO_CALL          "org.ofono.VoiceCall"

#define DUMP_STR(f)         (f) ? (f)   : ""
#define DUMP_YESNO(f)       (f) ? "yes" : "no"
#define FREE(p)             if((p) != NULL) mrp_free(p)

typedef void *(*array_cb_t) (DBusMessageIter *it, void *user_data);

/******************************************************************************/

static int install_ofono_handlers(ofono_t *ofono);
static void remove_ofono_handlers(ofono_t *ofono);
static void ofono_init_cb(mrp_dbus_t *dbus, const char *name, int running,
                          const char *owner, void *user_data);

static ofono_modem_t *find_modem(ofono_t *ofono, const char *path);
static int modem_has_interface(ofono_modem_t *modem, const char *interface);
static int modem_has_feature(ofono_modem_t *modem, const char *feature);
static int query_modems(ofono_t *ofono);
static void cancel_modem_query(ofono_t *ofono);
static void modem_query_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static int modem_added_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static int modem_removed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static int modem_changed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static int call_endreason_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static void *parse_modem(DBusMessageIter *it, void *user_data);
static void *parse_modem_property(DBusMessageIter *it, void *user_data);
static void purge_modems(ofono_t *ofono);

static ofono_call_t *find_call(ofono_modem_t *modem, const char *path);
static int query_calls(mrp_dbus_t *dbus, ofono_modem_t *modem);
static void call_query_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static int call_added_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static int call_removed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static int call_changed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data);
static void *parse_call(DBusMessageIter *it, void *user_data);
static void *parse_call_property(DBusMessageIter *it, void *user_data);
static void dump_call(ofono_call_t *call);
static void cancel_call_query(ofono_modem_t *modem);
static void purge_calls(ofono_modem_t *modem);

/******************************************************************************/

ofono_t *ofono_watch(mrp_mainloop_t *ml, tel_watcher_t notify)
{
    ofono_t *ofono;
    DBusError dbuserr;
    mrp_dbus_t *dbus;

    mrp_debug("entering ofono_watch");

    dbus = mrp_dbus_connect(ml, OFONO_DBUS, &dbuserr);
    if (dbus == NULL) {
        mrp_log_error("failed to open %s DBUS", OFONO_DBUS);
        return NULL;
    }

    if ((ofono = mrp_allocz(sizeof(*ofono))) != NULL) {
        mrp_list_init(&ofono->modems);

        ofono->dbus = dbus;

        if (install_ofono_handlers(ofono)) {
            ofono->notify = notify;
            /* if oFono is already running,  */
            query_modems(ofono);
            return ofono;
        }
        else
            mrp_log_error("failed to set up ofono DBUS handlers");
    }

    ofono_unwatch(ofono);

    return NULL;
}


void ofono_unwatch(ofono_t *ofono)
{
    if (ofono != NULL) {
        if (ofono->dbus)
            remove_ofono_handlers(ofono);

        mrp_free(ofono);
    }
}

/******************************************************************************/

static int install_ofono_handlers(ofono_t *ofono)
{
    const char *path;

    if (!mrp_dbus_follow_name(ofono->dbus, OFONO_SERVICE,
                              ofono_init_cb,(void *) ofono))
        goto fail;

    path  = "/";

    /** watch modem change signals */
    if (!mrp_dbus_subscribe_signal(ofono->dbus, modem_added_cb, ofono,
            OFONO_SERVICE, path, OFONO_MODEM_MGR, "ModemAdded", NULL)) {
        mrp_log_error("error watching ModemAdded on %s", OFONO_MODEM_MGR);
        goto fail;
    }

    /* TODO: check if really needed to be handled */
    if (!mrp_dbus_subscribe_signal(ofono->dbus, modem_removed_cb, ofono,
            OFONO_SERVICE, path, OFONO_MODEM_MGR, "ModemRemoved", NULL)) {
        mrp_log_error("error watching ModemRemoved on %s", OFONO_MODEM_MGR);
        goto fail;
    }

    /* TODO: check if really needed to be handled */
    if (!mrp_dbus_subscribe_signal(ofono->dbus, modem_changed_cb, ofono,
            OFONO_SERVICE, NULL, OFONO_MODEM, "PropertyChanged", NULL)) {
        mrp_log_error("error watching PropertyChanged on %s", OFONO_MODEM);
        goto fail;
    }

    /** watch call manager signals from a modem object */
    if (!mrp_dbus_subscribe_signal(ofono->dbus, call_added_cb, ofono,
            OFONO_SERVICE, NULL, OFONO_CALL_MGR, "CallAdded", NULL)) {
        mrp_log_error("error watching CallAdded on %s", OFONO_CALL_MGR);
        goto fail;
    }

    if (!mrp_dbus_subscribe_signal(ofono->dbus, call_removed_cb, ofono,
            OFONO_SERVICE, NULL, OFONO_CALL_MGR, "CallRemoved", NULL)) {
        mrp_log_error("error watching CallRemoved on %s", OFONO_CALL_MGR);
        goto fail;
    }

    /** watch call change signals from a call object */
    if (!mrp_dbus_subscribe_signal(ofono->dbus, call_changed_cb, ofono,
            OFONO_SERVICE, NULL, OFONO_CALL, "PropertyChanged", NULL)) {
        mrp_log_error("error watching PropertyChanged on %s", OFONO_CALL);
        goto fail;
    }

    if (!mrp_dbus_subscribe_signal(ofono->dbus, call_endreason_cb, ofono,
            OFONO_SERVICE, NULL, OFONO_CALL, "DisconnectReason", NULL)) {
        mrp_log_error("error watching DisconnectReason on %s", OFONO_CALL);
        goto fail;
    }

    mrp_debug("installed oFono signal handlers");
    return TRUE;

fail:
    remove_ofono_handlers(ofono);
    mrp_log_error("failed to install oFono signal handlers");
    return FALSE;
}


static void remove_ofono_handlers(ofono_t *ofono)
{
    const char *path;

    mrp_dbus_forget_name(ofono->dbus, OFONO_SERVICE,
                         ofono_init_cb, (void *) ofono);

    path  = "/";
    mrp_debug("removing DBUS signal watchers");

    mrp_dbus_unsubscribe_signal(ofono->dbus, modem_added_cb, ofono,
        OFONO_SERVICE, path, OFONO_MODEM_MGR, "ModemAdded", NULL);

    mrp_dbus_unsubscribe_signal(ofono->dbus, modem_removed_cb, ofono,
        OFONO_SERVICE, path, OFONO_MODEM_MGR, "ModemRemoved", NULL);

    mrp_dbus_unsubscribe_signal(ofono->dbus, modem_changed_cb, ofono,
        OFONO_SERVICE, NULL, OFONO_MODEM, "PropertyChanged", NULL);

    mrp_dbus_unsubscribe_signal(ofono->dbus, call_added_cb, ofono,
        OFONO_SERVICE, NULL, OFONO_CALL_MGR, "CallAdded", NULL);

    mrp_dbus_unsubscribe_signal(ofono->dbus, call_removed_cb, ofono,
        OFONO_SERVICE, NULL, OFONO_CALL_MGR, "CallRemoved", NULL);

    mrp_dbus_unsubscribe_signal(ofono->dbus, call_changed_cb, ofono,
        OFONO_SERVICE, NULL, OFONO_CALL, "PropertyChanged", NULL);

    mrp_dbus_unsubscribe_signal(ofono->dbus, call_changed_cb, ofono,
        OFONO_SERVICE, NULL, OFONO_CALL, "DisconnectReason", NULL);

}


static void ofono_init_cb(mrp_dbus_t *dbus, const char *name, int running,
                         const char *owner, void *user_data)
{
    ofono_t *ofono;

    (void)dbus;

    ofono = (ofono_t *)user_data;

    if (ofono == NULL) {
        mrp_log_error("NULL ofono");
        // not critical in this method
    }

    mrp_debug("%s is %s with owner ", name, running ? "up" : "down", owner);

    if (!running)
        purge_modems(ofono);
    else
        query_modems(ofono);
}


static int array_foreach(DBusMessageIter *it, array_cb_t callback,
                         void *user_data)
{
    DBusMessageIter arr;

    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY)
        return FALSE;

    for (dbus_message_iter_recurse(it, &arr);
         dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_INVALID;
         dbus_message_iter_next(&arr)) {

        if (callback(&arr, user_data) == NULL)
            return FALSE;
    }

    return TRUE;
}


static void free_strarr(char **strarr)
{
    int n;

    if (strarr != NULL) {
        for (n = 0; strarr[n] != NULL; n++)
            mrp_free(strarr[n]);

        mrp_free(strarr);
    }
}


static int strarr_contains(char **strarr, const char *str)
{
    int n;

    if (strarr != NULL)
        for (n = 0; strarr[n] != NULL; n++)
            if (!strcmp(strarr[n], str))
                return TRUE;

    return FALSE;
}


/******************************************************************************/

static void dump_modem(ofono_modem_t *m)
{
    ofono_call_t     *call;
    mrp_list_hook_t  *p, *n;
    int               i;


    mrp_debug("\nmodem '%s' {", m->modem_id);
    mrp_debug("    name:         '%s'", DUMP_STR(m->name));
    mrp_debug("    manufacturer: '%s'", DUMP_STR(m->manufacturer));
    mrp_debug("    model:        '%s'", DUMP_STR(m->model));
    mrp_debug("    revision:     '%s'", DUMP_STR(m->revision));
    mrp_debug("    serial:       '%s'", DUMP_STR(m->serial));
    mrp_debug("    type:         '%s'", DUMP_STR(m->type));

    if (m->interfaces != NULL) {
        mrp_debug("    supported interfaces:");
        for (i = 0; m->interfaces[i] != NULL; i++)
            mrp_debug("        %s", m->interfaces[i]);
    }

    if (m->features != NULL) {
        mrp_debug("    supported features:");
        for (i = 0; m->features[i] != NULL; i++)
            mrp_debug("        %s", m->features[i]);
    }

    mrp_debug("    is powered %s", m->powered ? "on" : "off");
    mrp_debug("    is %sline" , m->online ? "on" : "off");
    mrp_debug("    is %slocked", m->lockdown ? "" : "un");
    mrp_debug("    has %s emergency call", m->emergency ? "ongoing" : "no");

    mrp_debug("    calls:");
    if (mrp_list_empty(&m->calls))
        mrp_debug("    none");
    else {
        mrp_list_foreach(&m->calls, p, n) {
            call = mrp_list_entry(p, ofono_call_t, hook);
            dump_call(call);
        }
    }

    mrp_debug("}");
}


static int free_modem(ofono_modem_t *modem)
{
    if (modem != NULL) {
        mrp_list_delete(&modem->hook);

        cancel_call_query(modem);
        purge_calls(modem);

        mrp_free(modem->modem_id);
        mrp_free(modem->name);
        mrp_free(modem->manufacturer);
        mrp_free(modem->model);
        mrp_free(modem->revision);
        mrp_free(modem->serial);
        mrp_free(modem->type);

        mrp_free(modem);
        return TRUE;
    }
    return FALSE;
}


static void purge_modems(ofono_t *ofono)
{
    mrp_list_hook_t *p, *n;
    ofono_modem_t  *modem;

    CHECK_PTR(ofono, ,"attempt to purge NULL ofono proxy");

    if (ofono->dbus) {
        if (ofono->modem_qry != 0) {
            mrp_dbus_call_cancel(ofono->dbus, ofono->modem_qry);
            ofono->modem_qry = 0;
        }

        mrp_list_foreach(&ofono->modems, p, n) {
            modem = mrp_list_entry(p, ofono_modem_t, hook);
            free_modem(modem);
        }
    }
}

static void dump_call(ofono_call_t *call)
{

    if (call == NULL) {
        mrp_debug("    none");
        return;
    }

    mrp_debug("\ncall '%s' {", call->call_id);
    mrp_debug("    service_id:           '%s'", DUMP_STR(call->service_id));
    mrp_debug("    line_id:              '%s'", DUMP_STR(call->line_id));
    mrp_debug("    name:                 '%s'", DUMP_STR(call->name));
    mrp_debug("    state:                '%s'", DUMP_STR(call->state));
    mrp_debug("    end_reason:           '%s'", DUMP_STR(call->end_reason));
    mrp_debug("    start_time:           '%s'", DUMP_STR(call->start_time));
    mrp_debug("    is multiparty:        '%s'", DUMP_YESNO(call->multiparty));
    mrp_debug("    is emergency:         '%s'", DUMP_YESNO(call->emergency));
    mrp_debug("    information:          '%s'", DUMP_STR(call->info));
    mrp_debug("    icon_id:              '%ud'", call->icon_id);
    mrp_debug("    remote held:          '%s'", DUMP_YESNO(call->remoteheld));
    mrp_debug("}");
}


static void free_call(ofono_call_t *call)
{
    if (call) {
        mrp_list_delete(&call->hook);
        mrp_free(call->call_id);
        mrp_free(call->service_id);
        mrp_free(call->line_id);
        mrp_free(call->name);
        mrp_free(call->state);
        mrp_free(call->end_reason);
        mrp_free(call->start_time);
        mrp_free(call->info);

        mrp_free(call);
    }
}


static void purge_calls(ofono_modem_t *modem)
{
    mrp_list_hook_t *p, *n;
    ofono_call_t  *call;
    ofono_t * ofono;

    if (modem) {
        ofono = modem->ofono;
        if (ofono->dbus) {
            if (modem->call_qry != 0) {
                mrp_dbus_call_cancel(ofono->dbus, modem->call_qry);
                modem->call_qry = 0;
            }

            mrp_list_foreach(&modem->calls, p, n) {
                call = mrp_list_entry(p, ofono_call_t, hook);
                ofono->notify(TEL_CALL_REMOVED, (tel_call_t *)call, modem);
                free_call(call);
            }
        }
    }
}


/*******************************************************************************
 * ofono modem handling
 */

ofono_modem_t *ofono_online_modem(ofono_t *ofono)
{
    ofono_modem_t  *modem;
    mrp_list_hook_t *p, *n;

    mrp_list_foreach(&ofono->modems, p, n) {
        modem = mrp_list_entry(p, ofono_modem_t, hook);

        if (modem->powered && modem->online) {
            return modem;
        }
    }

    return NULL;
}


static int modem_has_interface(ofono_modem_t *modem, const char *interface)
{
    (void)modem_has_feature; /* ugh, yuck ... */
    mrp_debug("checking interface %s on modem %s, with interfaces",
              interface, modem->modem_id, modem->interfaces);

    return strarr_contains(modem->interfaces, interface);
}


static int modem_has_feature(ofono_modem_t *modem, const char *feature)
{
    return strarr_contains(modem->features, feature);
}


static int query_modems(ofono_t *ofono)
{
    int32_t    qry;

    mrp_debug("querying modems on oFono");

    if (ofono == NULL)
        return FALSE;

    if(ofono->dbus == NULL)
        return FALSE;

    cancel_modem_query(ofono);

    qry = mrp_dbus_call(ofono->dbus, OFONO_SERVICE, "/",
                        OFONO_MODEM_MGR, "GetModems", 5000,
                        modem_query_cb, ofono, DBUS_TYPE_INVALID);

    ofono->modem_qry = qry;

    return qry != 0;
}


static void cancel_modem_query(ofono_t *ofono)
{
    if (ofono->dbus != NULL && ofono->modem_qry)
        mrp_dbus_call_cancel(ofono->dbus, ofono->modem_qry);

    ofono->modem_qry = 0;
}


static void modem_query_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    DBusMessageIter  it;
    ofono_t         *ofono = (ofono_t *)user_data;
    ofono_modem_t   *modem;
    mrp_list_hook_t  *p, *n;

    mrp_debug("modem query response on oFono");

    if (dbus_message_iter_init(msg, &it)) {
        if (array_foreach(&it, parse_modem, ofono)) {
            /* array_foreach successfully fetched all modems into the list */
            mrp_list_foreach(&ofono->modems, p, n) {
                modem = mrp_list_entry(p, ofono_modem_t, hook);
                query_calls(dbus, modem);
            }
            return;
        }
    }

    mrp_log_error("failed to process modem query response");
}


static ofono_modem_t *find_modem(ofono_t *ofono, const char *path)
{
    mrp_list_hook_t *p, *n;
    ofono_modem_t  *modem;

    CHECK_PTR(ofono, NULL, "ofono is NULL");
    CHECK_PTR(path, NULL, "path is NULL");

    mrp_list_foreach(&ofono->modems, p, n) {
        modem = mrp_list_entry(p, ofono_modem_t, hook);

        if (modem->modem_id != NULL && !strcmp(modem->modem_id, path))
            return modem;
    }

    return NULL;
}


static int modem_added_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    ofono_t         *ofono = (ofono_t *)user_data;
    ofono_modem_t   *modem;
    DBusMessageIter  it;

    (void)dbus;

    mrp_debug("new modem added on oFono...");

    if(ofono == NULL)
        return FALSE;

    if (dbus_message_iter_init(msg, &it)) {
        modem = (ofono_modem_t*) parse_modem(&it, ofono);
        return query_calls(dbus, modem);
    }

    return FALSE;
}


static int modem_removed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    ofono_t       *ofono = (ofono_t *)user_data;
    ofono_modem_t *modem;
    const char    *path;

    (void)dbus;

    if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
                              DBUS_TYPE_INVALID)) {
        mrp_debug("modem '%s' was removed", path);

        modem = find_modem(ofono, path);
        if(modem != NULL)
            return free_modem(modem);
    }

    return FALSE;
}


static int modem_changed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    const char      *path  = dbus_message_get_path(msg);
    ofono_t         *ofono = (ofono_t *)user_data;
    ofono_modem_t   *modem = find_modem(ofono, path);
    DBusMessageIter  it;

    (void)dbus;

    if (modem != NULL && dbus_message_iter_init(msg, &it)) {
        mrp_debug("changes in modem '%s'...", modem->modem_id);

        if (parse_modem_property(&it, modem)) {
            dump_modem(modem);
            /* place to handle Online, Powered, Lockdown signals */
            /* since call objects will be signaled separately, do nothing */
        }
    }

    return TRUE;
}


static void *parse_modem(DBusMessageIter *msg, void *user_data)
{
    ofono_t         *ofono = (ofono_t *)user_data;
    ofono_modem_t   *modem;
    char            *path;
    DBusMessageIter  mdm, *it;
    int              type;

    modem = NULL;

    /*
     * We can be called from an initial modem query callback
     *    array{object, dict} GetModems(), where dict is array{property, value}
     * on each element of the array,
     * or then from a modem add notification callback:
     *    ModemAdded(object_path, dict)
     * We first take care of the message content differences here
     * (object vs object path)
     */

    if ((type = dbus_message_iter_get_arg_type(msg)) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(msg, &mdm);
        it = &mdm;
    }
    else {
        if (type == DBUS_TYPE_OBJECT_PATH)
            it = msg;
        else
            goto malformed;
    }

    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_OBJECT_PATH)
        goto malformed;

    /* at this point we have an object path and a property-value array (dict) */
    dbus_message_iter_get_basic(it, &path);
    dbus_message_iter_next(it);

    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY)
        goto malformed;

    if ((modem = mrp_allocz(sizeof(*modem))) == NULL)
        goto fail;

    mrp_list_init(&modem->hook);
    mrp_list_init(&modem->calls);
    modem->ofono = ofono;

    if ((modem->modem_id = mrp_strdup(path)) == NULL)
        goto fail;

    mrp_list_append(&ofono->modems, &modem->hook);

    if (!array_foreach(it, parse_modem_property, modem))
        goto malformed;

    mrp_debug("found modem %s", modem->modem_id);

    return (void *)modem;

malformed:
    mrp_log_error("malformed modem entry");

fail:
    mrp_log_error("parsing modem entry failed");
    free_modem(modem);

    return NULL;
}


static void *parse_modem_property(DBusMessageIter *it, void *user_data)
{
    ofono_modem_t   *modem = (ofono_modem_t *)user_data;
    DBusMessageIter  dict, vrnt, arr, *prop;
    const char      *key;
    int              expected, t, n;
    char           **strarr, *str;
    void            *valptr;

    key = NULL;
    t   = dbus_message_iter_get_arg_type(it);

    /*
     * We are either called from a modem query response callback, or
     * from a modem property change notification callback. In the former
     * case we get an iterator pointing to a dictionary entry, in the
     * latter case we get an iterator already pointing to the property
     * key. We take care of the differences here...
     */

    if (t == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(it, &dict);

        if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_STRING)
            goto malformed;
        else
            prop = &dict;
    }
    else {
        if (t == DBUS_TYPE_STRING)
            prop = it;
        else
            goto malformed;
    }

    dbus_message_iter_get_basic(prop, &key);
    dbus_message_iter_next(prop);

    if (dbus_message_iter_get_arg_type(prop) != DBUS_TYPE_VARIANT)
        goto malformed;

    dbus_message_iter_recurse(prop, &vrnt);

#define HANDLE_TYPE(_key, _type, _field) \
    if (!strcmp(key, _key)) {			 \
        expected = _type;					 \
        valptr   = &modem->_field;			 \
        if (_type == DBUS_TYPE_STRING || _type == DBUS_TYPE_OBJECT_PATH) \
            mrp_free(*(char **)valptr);			 \
        goto getval;						 \
    }

    /* one of the HANDLE_TYPE calls will match */
    HANDLE_TYPE("Type"        , DBUS_TYPE_STRING , type);
    HANDLE_TYPE("Powered"     , DBUS_TYPE_BOOLEAN, powered);
    HANDLE_TYPE("Online"      , DBUS_TYPE_BOOLEAN, online);
    HANDLE_TYPE("Lockdown"    , DBUS_TYPE_BOOLEAN, lockdown);
    HANDLE_TYPE("Emergency"   , DBUS_TYPE_BOOLEAN, emergency);
    HANDLE_TYPE("Name"        , DBUS_TYPE_STRING , name);
    HANDLE_TYPE("Manufacturer", DBUS_TYPE_STRING , manufacturer);
    HANDLE_TYPE("Model"       , DBUS_TYPE_STRING , model);
    HANDLE_TYPE("Revision"    , DBUS_TYPE_STRING , revision);
    HANDLE_TYPE("Serial"      , DBUS_TYPE_STRING , serial);
    HANDLE_TYPE("Interfaces"  , DBUS_TYPE_ARRAY  , interfaces);
    HANDLE_TYPE("Features"    , DBUS_TYPE_ARRAY  , features);

    return modem;                        /* an ignored property */
#undef HANDLE_TYPE

getval:
    if (expected != DBUS_TYPE_ARRAY) {
        if (dbus_message_iter_get_arg_type(&vrnt) != expected)
            goto malformed;
        else
            dbus_message_iter_get_basic(&vrnt, valptr);

        if (expected == DBUS_TYPE_STRING ||
                expected == DBUS_TYPE_OBJECT_PATH) {
            *(char **)valptr = mrp_strdup(*(char **)valptr);

            if (*(char **)valptr == NULL) {
                mrp_log_error("failed to allocate modem field %s", key);
                return NULL;
            }
        }
    }
    else {
        dbus_message_iter_recurse(&vrnt, &arr);
        strarr = NULL;
        n      = 1;

        while ((t = dbus_message_iter_get_arg_type(&arr)) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&arr, &str);

            if (mrp_reallocz(strarr, n, n + 1) != NULL) {
                if ((strarr[n-1] = mrp_strdup(str)) == NULL)
                    goto arrfail;
                n++;
            }

            dbus_message_iter_next(&arr);
        }

        if (t == DBUS_TYPE_INVALID) {
            free_strarr(*(char ***)valptr);
            *(char ***)valptr = strarr;
        }
        else {
            mrp_log_error("malformed array entry for key %s", key);
            goto arrfail;
        }
    }

    return modem;

malformed:
    if (key != NULL)
        mrp_log_error("malformed modem entry for key %s", key);
    else
        mrp_log_error("malformed modem entry");

    return NULL;

arrfail:
    free_strarr(strarr);

    return NULL;
}


/*******************************************************************************
 * ofono call manager
 */

static ofono_call_t *find_call(ofono_modem_t *modem, const char *path)
{
    mrp_list_hook_t *p, *n;
    ofono_call_t  *call;

    mrp_list_foreach(&modem->calls, p, n) {
        call = mrp_list_entry(p, ofono_call_t, hook);

        if (call->call_id != NULL && !strcmp(call->call_id, path))
            return call;
    }

    return NULL;
}


static int query_calls(mrp_dbus_t *dbus, ofono_modem_t *modem)
{
    CHECK_PTR(modem, FALSE, "modem is NULL");

    if (modem->call_qry != 0)
        return TRUE;    /* already querying */

    if (modem->online && modem_has_interface(modem, OFONO_CALL_MGR)) {
        modem->call_qry  = mrp_dbus_call(dbus, OFONO_SERVICE, modem->modem_id,
                                       OFONO_CALL_MGR, "GetCalls", 5000,
                                       call_query_cb, modem, DBUS_TYPE_INVALID);

        return modem->call_qry != 0;
    } else
        mrp_debug("call query canceled on modem %s: offline or no callmanager",
                  modem->modem_id);


    return FALSE;
}


static void cancel_call_query(ofono_modem_t *modem)
{
    CHECK_PTR(modem, ,"modem is NULL");
    CHECK_PTR(modem->ofono, ,"ofono is NULL in modem %s", modem->modem_id);

    if (modem->ofono->dbus && modem->call_qry != 0)
        mrp_dbus_call_cancel(modem->ofono->dbus, modem->call_qry);

    modem->call_qry = 0;
}


static void call_query_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    ofono_t         *ofono;
    ofono_modem_t   *modem;
    ofono_call_t    *call;
    DBusMessageIter  it;
    mrp_list_hook_t *p, *n;

    (void)dbus;

    ofono = (ofono_t *)user_data;
    CHECK_PTR(ofono, , "ofono is NULL");

    modem = find_modem(ofono, dbus_message_get_path (msg));
    CHECK_PTR(modem, , "modem is NULL");
    FAIL_IF(modem->ofono != ofono, , "corrupted modem data");

    mrp_debug("call query callback on modem %s", modem->modem_id);

    modem->call_qry = 0;

    if (dbus_message_iter_init(msg, &it)) {
        if (!array_foreach(&it, parse_call, modem)) {
            mrp_log_error("failed processing call query response");
            return;
        }
        mrp_debug("calling notify TEL_CALL_PURGE on modem %s", modem->modem_id);
        /*modem->ofono->notify(TEL_CALL_PURGE, NULL, modem->modem_id);*/
        mrp_list_foreach(&modem->calls, p, n) {
            call = mrp_list_entry(p, ofono_call_t, hook);
            mrp_debug("calling notify TEL_CALL_LISTED on call %s, modem %s",
                      (tel_call_t*)call->call_id, modem->modem_id);
#if _NOTIFY_MDB
            ofono->notify(TEL_CALL_LISTED, (tel_call_t*)call, modem->modem_id);
#else
            dump_call((ofono_call_t*)call);
#endif
            mrp_debug("new oFono call listed: %s", call->call_id);
        }
        return;
    }
    mrp_log_error("listing oFono calls failed");
}


static int call_added_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    ofono_t         *ofono;
    ofono_modem_t   *modem;
    ofono_call_t    *call;
    DBusMessageIter  it;

    (void)dbus;

    ofono = (ofono_t *)user_data;
    CHECK_PTR(ofono, FALSE, "ofono is NULL");

    modem = find_modem(ofono, dbus_message_get_path (msg));
    CHECK_PTR(modem, FALSE, "modem is NULL");
    FAIL_IF(modem->ofono != ofono, FALSE, "corrupted modem data");

    mrp_debug("new oFono call signaled on modem %s", modem->modem_id);

    if (dbus_message_iter_init(msg, &it)) {
        call = parse_call(&it, modem);
        if (call) {
            CHECK_PTR(modem->ofono->notify, FALSE, "notify is NULL");
            mrp_debug("calling notify TEL_CALL_ADDED on call %s, modem %s",
                      (tel_call_t*)call->call_id, modem->modem_id);
#if _NOTIFY_MDB
            ofono->notify(TEL_CALL_ADDED, (tel_call_t*)call, modem->modem_id);
#else
            dump_call((ofono_call_t*)call);
#endif
            mrp_debug("new oFono call added: %s", call->call_id);
            dump_modem(modem);
            return TRUE;
        }
    }

    mrp_log_error("adding new oFono call failed");
    return FALSE;
}


static int call_removed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    ofono_t         *ofono;
    ofono_modem_t   *modem;
    ofono_call_t    *call;
    const char      *path;

    (void)dbus;

    ofono = (ofono_t *)user_data;
    CHECK_PTR(ofono, FALSE, "ofono is NULL");

    path = dbus_message_get_path (msg);
    modem = find_modem(ofono, path);
    CHECK_PTR(modem, FALSE, "modem is not found based on path %s", path);
    FAIL_IF(modem->ofono != ofono, FALSE, "corrupted modem data");

    if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
                              DBUS_TYPE_INVALID)) {
        mrp_debug("call '%s' signaled to be removed", path);
        call = find_call(modem, path);
        if (call) {
            CHECK_PTR(ofono->notify, FALSE, "notify is NULL");
            mrp_debug("calling notify TEL_CALL_REMOVED on call %s, modem %s",
                      (tel_call_t*)call->call_id, modem->modem_id);
#if _NOTIFY_MDB
            ofono->notify(TEL_CALL_REMOVED, (tel_call_t*)call, modem->modem_id);
#else
            dump_call((ofono_call_t*)call);
#endif
            mrp_debug("oFono call removed: %s", call->call_id);
            free_call(call);
            dump_modem(modem);
            return TRUE;
        } else
            mrp_log_error("could not find call based on path %s", path);
    } else
        mrp_log_error("removing oFono call failed: could not get DBUS path");

    return FALSE;
}


/*******************************************************************************
 * ofono call handling
 */

/**
 * find service_id (modem) from call path (call_id)
 * example: path = "/hfp/00DBDF143ADC_44C05C71BAF6/voicecall01",
 * modem_id = "/hfp/00DBDF143ADC_44C05C71BAF6"
 * call_id = path
 */
static void get_modem_from_call_path(char *call_path, char **modem_id)
{
    unsigned int   i = call_path == NULL ? 0 : strlen(call_path)-1;
    char * dest = NULL;

    for(; i > 0 && call_path[i] != '/'; i--);
    if(i > 0) {
        call_path[i] = '\0';
        dest = mrp_strdup(call_path);
        call_path[i] = '/'; /* restore path */
    }
    *modem_id = dest;
}


static int call_changed_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    char            *path, *modem_id;
    DBusMessageIter  it;
    ofono_t         *ofono;
    ofono_modem_t   *modem;
    ofono_call_t    *call;

    (void)dbus;

    ofono = (ofono_t *)user_data;
    CHECK_PTR(ofono, FALSE, "ofono is NULL");

    path = (char *) dbus_message_get_path(msg); /* contains call object path */
    get_modem_from_call_path(path, &modem_id);  /* get modem from modem path */
    modem = find_modem(ofono, modem_id);        /* modem path is modem_id    */
    CHECK_PTR(modem, FALSE, "modem is NULL");
    FAIL_IF(modem->ofono != ofono, FALSE, "corrupted modem data");

    call = find_call(modem, path);
    CHECK_PTR(call, FALSE, "call not found based on path %s", path);

    if (dbus_message_iter_init(msg, &it)) {
        mrp_debug("changes in call '%s'...", path);

        if (parse_call_property(&it, call)) {
            CHECK_PTR(ofono->notify, FALSE, "notify is NULL");
            mrp_debug("calling notify TEL_CALL_CHANGED on call %s, modem %s",
                      (tel_call_t*)call->call_id, modem->modem_id);
#if _NOTIFY_MDB
            ofono->notify(TEL_CALL_CHANGED, (tel_call_t*)call, modem->modem_id);
#else
            dump_call((ofono_call_t*)call);
#endif
            mrp_debug("oFono call changed: %s", call->call_id);
            dump_modem(modem);
            return TRUE;
        } else
            mrp_debug("parsing error in call change callback for %s",
                      call->call_id);
    } else
        mrp_debug("dbus iterator error in call change callback for %s",
                  call->call_id);

    return FALSE;
}


static int call_endreason_cb(mrp_dbus_t *dbus, DBusMessage *msg, void *user_data)
{
    char            *path, *modem_id;
    DBusMessageIter  it;
    ofono_t         *ofono;
    ofono_modem_t   *modem;
    ofono_call_t    *call;

    (void)dbus;

    ofono = (ofono_t *)user_data;
    CHECK_PTR(ofono, FALSE, "ofono is NULL");

    path = (char *) dbus_message_get_path(msg); /* contains call object path */
    get_modem_from_call_path(path, &modem_id);  /* get modem from modem path */
    modem = find_modem(ofono, modem_id);        /* modem path is modem_id    */
    CHECK_PTR(modem, FALSE, "modem is NULL");
    FAIL_IF(modem->ofono != ofono, FALSE, "corrupted modem data");

    call = find_call(modem, path);
    CHECK_PTR(call, FALSE, "call not found based on path %s", path);

    if (dbus_message_iter_init(msg, &it)) {
        if(dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
            mrp_debug("wrong dbus argument type in call change callback for %s",
                       call->call_id);
            return FALSE;
        }

        dbus_message_iter_get_basic(&it, path);
        call->end_reason = mrp_strdup(path);

        if(MRP_UNLIKELY(call->end_reason == NULL)) {
            mrp_log_error("failed in mrp_strdup %s", path);
            return FALSE;
        }
        mrp_debug("disconnect reason in call '%s': %s",
                  call->call_id, call->end_reason);

        CHECK_PTR(ofono->notify, FALSE, "notify is NULL");
        mrp_debug("calling notify TEL_CALL_CHANGED on call %s, modem %s",
                  (tel_call_t*)call->call_id, modem->modem_id);
#if _NOTIFY_MDB
        ofono->notify(TEL_CALL_CHANGED, (tel_call_t*)call, modem->modem_id);
#else
        dump_call((ofono_call_t*)call);
#endif
        mrp_debug("oFono call end reason changed: %s", call->call_id);
        dump_modem(modem);
        return TRUE;
    } else
        mrp_debug("dbus iterator error in call end reason callback for %s",
                  call->call_id);

    return FALSE;
}


static void *parse_call(DBusMessageIter *msg, void *user_data)
{
    ofono_modem_t   *modem;
    ofono_call_t    *call;
    char            *path;
    DBusMessageIter  sub, *it;
    int              type;

    call = NULL;

    modem = (ofono_modem_t *)user_data;
    CHECK_PTR(modem, FALSE, "modem is NULL");

    mrp_debug("parsing call in modem '%s'...", modem->modem_id);

    /*
     * We can be called from an initial call query callback, which returns
     * array of { object, property-dictionary },
     * or from a "call added" notification callback, which provides
     * and object path and a property-dictionary.
     * We first take care of the message content differences here.
     */

    if ((type = dbus_message_iter_get_arg_type(msg)) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(msg, &sub);
        it = &sub;
    }
    else {
        if (type == DBUS_TYPE_OBJECT_PATH)
            it = msg;
        else
            goto malformed;
    }

    /* in both cases the first should be an object path */
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_OBJECT_PATH)
        goto malformed;

    dbus_message_iter_get_basic(it, &path);
    if(path == NULL)
        goto fail;

    dbus_message_iter_next(it);

    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY)
        goto malformed;

    if ((call = mrp_allocz(sizeof(*call))) == NULL)
        goto fail;

    mrp_list_init(&call->hook);
    mrp_list_init(&modem->calls);
    call->modem = modem;

    if ((call->call_id = mrp_strdup(path)) == NULL)
        goto fail;

    get_modem_from_call_path(path, &(call->service_id));

    mrp_list_append(&modem->calls, &call->hook);

    if (!array_foreach(it, parse_call_property, call))
        goto malformed;



    mrp_debug("returning from parsing call %s", call->call_id);\
    return (void *)call;

malformed:
    mrp_log_error("malformed call entry");

fail:
    free_call(call);
    return NULL;
}


static void *parse_call_property(DBusMessageIter *it, void *user_data)
{
    ofono_call_t    *call;
    DBusMessageIter  dict, vrnt, *prop;
    const char      *key;
    int              expected, t;
    void            *valptr;

    call = (ofono_call_t *)user_data;
    CHECK_PTR(call, FALSE, "call is NULL");

    key = NULL;
    t   = dbus_message_iter_get_arg_type(it);

    /*
     * We are either called from a call query response callback, or
     * from a call propery change notification callback. In the former
     * case we get an iterator pointing to a dictionary entry, in the
     * latter case we get an iterator already pointing to the property
     * key. We take care of the differences here...
     */

    if (t == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(it, &dict);

        if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_STRING)
            goto malformed;
        else
            prop = &dict;
    }
    else {
        if (t == DBUS_TYPE_STRING)
            prop = it;
        else
            goto malformed;
    }

    dbus_message_iter_get_basic(prop, &key);
    dbus_message_iter_next(prop);

    if (dbus_message_iter_get_arg_type(prop) != DBUS_TYPE_VARIANT)
        goto malformed;

    dbus_message_iter_recurse(prop, &vrnt);

#define HANDLE_TYPE(_key, _type, _field) \
    if (!strcmp(key, _key)) {			 \
        mrp_debug("parsing call property %s", key);\
        expected = _type;					 \
        valptr   = &call->_field;			 \
        if (_type == DBUS_TYPE_STRING || _type == DBUS_TYPE_OBJECT_PATH) \
            mrp_free(*(char **)valptr);			 \
        goto getval;						 \
    }

    /* one of the HANDLE_TYPE calls will match */
    HANDLE_TYPE("LineIdentification", DBUS_TYPE_STRING  , line_id);
    HANDLE_TYPE("IncomingLine",       DBUS_TYPE_STRING  , incoming_line);
    HANDLE_TYPE("Name",               DBUS_TYPE_STRING  , name);
    HANDLE_TYPE("Multiparty",         DBUS_TYPE_BOOLEAN , multiparty);
    HANDLE_TYPE("State",              DBUS_TYPE_STRING  , state);
    HANDLE_TYPE("StartTime",          DBUS_TYPE_STRING  , start_time);
    HANDLE_TYPE("Information",        DBUS_TYPE_STRING  , info);
    HANDLE_TYPE("Icon",               DBUS_TYPE_BYTE    , icon_id);
    HANDLE_TYPE("Emergency",          DBUS_TYPE_BOOLEAN , emergency);
    HANDLE_TYPE("RemoteHeld",         DBUS_TYPE_BOOLEAN , remoteheld);

    return call;                      /* other properties ignored */
#undef HANDLE_TYPE

getval:
    if (dbus_message_iter_get_arg_type(&vrnt) != expected)
        goto malformed;
    else
        dbus_message_iter_get_basic(&vrnt, valptr);

    if (expected == DBUS_TYPE_STRING ||
            expected == DBUS_TYPE_OBJECT_PATH) {
        *(char **)valptr = mrp_strdup(*(char **)valptr);

        if (*(char **)valptr == NULL) {
            mrp_log_error("failed to allocate modem field %s", key);
            return NULL;
        }
    }

    return call;

malformed:
    if (key != NULL)
        mrp_log_error("malformed call entry for key %s", key);
    else
        mrp_log_error("malformed call entry");

    return NULL;
}