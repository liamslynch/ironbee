/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee --- HTP Module
 *
 * This module integrates libhtp.
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */
#include "ironbee_config_auto.h"

#include <ironbee/bytestr.h>
#include <ironbee/cfgmap.h>
#include <ironbee/engine.h>
#include <ironbee/field.h>
#include <ironbee/hash.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/provider.h>
#include <ironbee/state_notify.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef"
#endif
#include <htp.h>
#include <htp_private.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        htp
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Define the public module symbol. */
IB_MODULE_DECLARE();

/* Pre-declare types */
typedef enum htp_data_source_t modhtp_param_source_t;
typedef struct modhtp_context_t modhtp_context_t;

/**
 * Module Configuration Structure
 */
struct modhtp_config_t {
    const char                *personality;  /**< libhtp personality */
    const modhtp_context_t    *context;      /**< Module context data */
};
typedef struct modhtp_config_t modhtp_config_t;

/**
 * Module Context Structure
 */
struct modhtp_context_t {
    const ib_engine_t         *ib;           /**< Engine handle */
    const modhtp_config_t     *mod_config;   /**< Module config structure */
    const htp_cfg_t           *htp_config;   /**< Parser config handle */
};

/**
 * HTP Connection parser data
 */
struct modhtp_parser_data_t {
    const modhtp_context_t    *context;      /**< The module context data */
    htp_connp_t               *parser;       /**< HTP connection parser */
    ib_conn_t                 *iconn;        /**< IronBee connection */
    htp_time_t                 open_time;    /**< HTP connection start time */
    htp_time_t                 close_time;   /**< HTP connection close time */
};
typedef struct modhtp_parser_data_t modhtp_parser_data_t;

/**
 * HTP transaction data
 */
struct modhtp_txdata_t {
    const ib_engine_t          *ib;          /**< IronBee engine */
    htp_tx_t                   *htx;         /**< The HTP transaction */
    ib_tx_t                    *itx;         /**< The IronBee transaction */
    const modhtp_parser_data_t *parser_data; /**< Connection parser data */
    int                         error_code;  /**< Error code from parser */
    const char                 *error_msg;   /**< Error message from parser */
};
typedef struct modhtp_txdata_t modhtp_txdata_t;

/**
 * Callback data for the param iterator callback
 */
typedef struct {
    ib_field_t	              *field_list;   /**< Field list to populate */
    modhtp_param_source_t      source;       /**< Desired source */
    size_t                     count;        /**< Count of matches */
} modhtp_param_iter_data_t;

/* Instantiate a module global configuration. */
static modhtp_config_t modhtp_global_config = {
    "generic", /* personality */
    NULL
};

static ib_engine_t *modhtp_ib = NULL;

/* -- Define several function types for callbacks */

/**
 * Function used as a callback to modhtp_table_iterator()
 *
 * @param[in] tx IronBee transaction
 * @param[in] key Key of the key/value pair
 * @param[in] vptr Pointer to value of the key/value pair
 * @param[in] data Function specific data
 *
 * @returns Status code
 */
typedef ib_status_t (* modhtp_table_iterator_callback_fn_t)(
    const ib_tx_t             *tx,
    const bstr                *key,
    void                      *vptr,
    void                      *data);

/**
 * Function used as a callback to modhtp_set_data()
 *
 * This matches the signature of many htp_tx_{req,res}_set_xxx_c() functions
 * from htp_transaction.h
 *
 * @param[in] tx IronBee transaction
 * @param[in] data Data to set
 * @param[in] dlen Length of @a data
 * @param[in] alloc Allocation strategy
 *
 * @returns Status code
 */
typedef htp_status_t (* modhtp_set_fn_t)(
    htp_tx_t                  *tx,
    const char                *data,
    size_t                     dlen,
    enum htp_alloc_strategy_t  alloc);

/**
 * Function used as a callback to modhtp_set_header()
 *
 * This matches the signature of htp_tx_{req,res}_set_header_c() functions
 * from htp_transaction.h
 *
 * @param[in] htx HTP transaction
 * @param[in] name Header name
 * @param[in] name_len Length of @a name
 * @param[in] value Header value
 * @param[in] value_len Length of @a value
 * @param[in] alloc Allocation strategy
 *
 * @returns HTP status code
 */
typedef htp_status_t (* modhtp_set_header_fn_t)(
    htp_tx_t                  *htx,
    const char                *name,
    size_t                     name_len,
    const char                *value,
    size_t                     value_len,
    enum htp_alloc_strategy_t  alloc);

/* Define a name/val lookup record. */
struct modhtp_nameval_t {
    const char *name;
    int         val;
};
typedef struct modhtp_nameval_t modhtp_nameval_t;

/* Text versions of personalities */
static const modhtp_nameval_t modhtp_personalities[] = {
    { "",           HTP_SERVER_IDS },
    { "minimal",    HTP_SERVER_MINIMAL },
    { "generic",    HTP_SERVER_GENERIC },
    { "ids",        HTP_SERVER_IDS },
    { "iis_4_0",    HTP_SERVER_IIS_4_0 },
    { "iis_5_0",    HTP_SERVER_IIS_5_0 },
    { "iis_5_1",    HTP_SERVER_IIS_5_1 },
    { "iis_6_0",    HTP_SERVER_IIS_6_0 },
    { "iis_7_0",    HTP_SERVER_IIS_7_0 },
    { "iis_7_5",    HTP_SERVER_IIS_7_5 },
    { "apache_2",   HTP_SERVER_APACHE_2 },
    { NULL, 0 }
};

/* -- libhtp utility functions -- */


/* Lookup a numeric personality from a name. */
static int modhtp_personality(
    const char *name)
{
    const modhtp_nameval_t *rec = modhtp_personalities;

    if (name == NULL) {
        return -1;
    }

    while (rec->name != NULL) {
        if (strcasecmp(name, rec->name) == 0) {
            return rec->val;
        }

        ++rec;
    }

    return -1;
}

/* Log htp data via ironbee logging. */
static int modhtp_callback_log(
    htp_log_t *log)
{
    modhtp_context_t *modctx =
        (modhtp_context_t *)htp_connp_get_user_data(log->connp);
    int level;

    /* Parsing issues are unusual but not IronBee failures. */
    switch(log->level) {
    case HTP_LOG_ERROR:
    case HTP_LOG_WARNING:
    case HTP_LOG_NOTICE:
    case HTP_LOG_INFO:
        level = IB_LOG_INFO;
        break;
    case HTP_LOG_DEBUG:
        level = IB_LOG_DEBUG;
        break;
    default:
        level = IB_LOG_DEBUG3;
    }

    if (log->code != 0) {
        ib_log_ex(modctx->ib, level,
                  log->file, log->line,
                  "LibHTP [error %d] %s",
                  log->code, log->msg);
    }
    else {
        ib_log_ex(modctx->ib, level,
                  log->file, log->line,
                  "LibHTP %s",
                  log->msg);
    }

    return 0;
}

/* -- Table iterator functions -- */

/**
 * modhtp_table_iterator() callback to add a field to an IronBee list
 *
 * @param[in] tx IronBee transaction
 * @param[in] key Key of the key/value pair
 * @param[in] vptr Pointer to value of the key/value pair
 * @param[in] data Function specific data (IB field/list pointer)
 *
 * @returns Status code (IB_OK)
 */
static ib_status_t modhtp_field_list_callback(
    const ib_tx_t *tx,
    const bstr    *key,
    void          *vptr,
    void          *data)
{
    assert(tx != NULL);
    assert(key != NULL);
    assert(vptr != NULL);
    assert(data != NULL);

    ib_field_t *flist = (ib_field_t *)data;
    const bstr *value = (const bstr *)vptr;
    ib_field_t *field;
    ib_status_t rc;

    /* Create a list field as an alias into htp memory. */
    rc = ib_field_create_bytestr_alias(&field,
                                       tx->mp,
                                       (const char *)bstr_ptr(key),
                                       bstr_len(key),
                                       (uint8_t *)bstr_ptr(value),
                                       bstr_len(value));
    if (rc != IB_OK) {
        ib_log_debug3_tx(tx, "Failed to create field: %s",
                         ib_status_to_string(rc));
        return IB_OK;
    }

    /* Add the field to the field list. */
    rc = ib_field_list_add(flist, field);
    if (rc != IB_OK) {
        ib_log_debug3_tx(tx, "Failed to add field: %s",
                         ib_status_to_string(rc));
        return IB_OK;
    }

    /* Always return IB_OK */
    return IB_OK;
}

/**
 * modhtp_table_iterator() callback to add a field to handle
 * request / response parameters
 *
 * @param[in] tx IronBee transaction
 * @param[in] key Key of the key/value pair
 * @param[in] value Pointer to value of the key/value pair
 * @param[in] data Function specific data (modhtp_param_iter_data_t *)
 *
 * @returns Status code (IB_OK)
 */
static ib_status_t modhtp_param_iter_callback(
    const ib_tx_t *tx,
    const bstr    *key,
    void          *value,
    void          *data)
{
    assert(tx != NULL);
    assert(key != NULL);
    assert(value != NULL);
    assert(data != NULL);

    modhtp_param_iter_data_t *idata = (modhtp_param_iter_data_t *)data;
    const htp_param_t *param = (const htp_param_t *)value;
    ib_field_t *field;
    ib_status_t rc;

    /* Ignore if from wrong source */
    if (param->source != idata->source) {
        return IB_OK;
    }

    /* Create a list field as an alias into htp memory. */
    rc = ib_field_create_bytestr_alias(&field,
                                       tx->mp,
                                       (const char *)bstr_ptr(key),
                                       bstr_len(key),
                                       (uint8_t *)bstr_ptr(param->value),
                                       bstr_len(param->value));
    if (rc != IB_OK) {
        ib_log_debug3_tx(tx, "Failed to create field: %s",
                         ib_status_to_string(rc));
        return IB_OK;
    }

    /* Add the field to the field list. */
    rc = ib_field_list_add(idata->field_list, field);
    if (rc != IB_OK) {
        ib_log_debug3_tx(tx, "Failed to add field: %s",
                         ib_status_to_string(rc));
        return IB_OK;
    }

    /* Always return IB_OK */
    ++(idata->count);
    return IB_OK;
}

/**
 * Generic HTP table iterator that takes a callback function
 *
 * The callback function @a fn is called for each iteration of @a table.
 *
 * @param[in] tx IronBee transaction passed to @a fn
 * @param[in] table The table to iterate
 * @param[in] fn The callback function
 * @param[in] data Generic data passed to @a fn
 *
 * @note If @a fn returns an error, it will cause an error to returned
 * immediately without completing table iteration.
 *
 * @returns Status code
 *  - IB_OK All OK
 *  - IB_EINVAL If either key or value of any iteration is NULL
 *  - Errors returned by @a fn
 */
static ib_status_t modhtp_table_iterator(
    const ib_tx_t                       *tx,
    const htp_table_t                   *table,
    modhtp_table_iterator_callback_fn_t  fn,
    void                                *data)
{
    assert(table != NULL);
    assert(fn != NULL);

    size_t index;
    size_t tsize = htp_table_size(table);

    for (index = 0;  index < tsize;  ++index) {
        bstr *key = NULL;
        void *value = NULL;
        ib_status_t rc;

        value = htp_table_get_index(table, index, &key);
        if (key == NULL) {
            return IB_EINVAL;
        }

        rc = fn(tx, key, value, data);
        if (rc != IB_OK) {
            return rc;
        }
    }

    return IB_OK;
}

/**
 * Set a generic request / response item for libhtp by creating a
 * c-style (nul-terminated) string from @a bstr and then calling
 * @a fn with the new c string.
 *
 * @param[in] itx IronBee transaction
 * @param[in] htx HTP transaction
 * @param[in] data Data to set
 * @param[in] dlen Length of @a data
 * @param[in] fn libhtp function to call
 *
 * @returns Status code
 */
static inline ib_status_t modhtp_set_data(
    const ib_tx_t      *itx,
    htp_tx_t           *htx,
    const char         *data,
    size_t              dlen,
    modhtp_set_fn_t     fn)
{
    htp_status_t  hrc;

    /* If there's no NULL, libhtp will return an error, so ignore it. */
    if (data == NULL) {
        return IB_OK;
    }

    /* Hand it off to libhtp */
    hrc = fn(htx, data, dlen, HTP_ALLOC_COPY);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    return IB_OK;
}

/**
 * Set a generic request / response item for libhtp by creating a
 * c-style (nul terminated) string from the non-terminated string @a data
 * of length @a dlen and then calling @a fn with the new c string.
 *
 * @param[in] itx IronBee transaction
 * @param[in] htx HTP transaction
 * @param[in] bstr ByteString to set
 * @param[in] fn libhtp function to call
 *
 * @returns Status code
 */
static inline ib_status_t modhtp_set_bstr(
    const ib_tx_t      *itx,
    htp_tx_t           *htx,
    const ib_bytestr_t *bstr,
    modhtp_set_fn_t     fn)
{
    ib_status_t rc;
    const char *ptr;

    ptr = (const char *)ib_bytestr_const_ptr(bstr);
    if (ptr == NULL) {
        ptr = "";
    }
    rc = modhtp_set_data(itx, htx, ptr, ib_bytestr_length(bstr), fn);
    return rc;
}

/**
 * Set headers to libhtp
 *
 * The callback function @a fn is called for each iteration of @a header.
 *
 * @param[in] itx IronBee transaction
 * @param[in] htx HTP transaction passed to @a fn
 * @param[in] header The header to iterate
 * @param[in] fn The callback function
 *
 * @returns Status code
 *  - IB_OK All OK
 *  - IB_EINVAL If either key or value of any iteration is NULL
 */
static ib_status_t modhtp_set_header(
    const ib_tx_t                    *itx,
    htp_tx_t                         *htx,
    const ib_parsed_header_wrapper_t *header,
    modhtp_set_header_fn_t            fn)
{
    assert(htx != NULL);
    assert(header != NULL);
    assert(fn != NULL);

    const ib_parsed_name_value_pair_list_t *node;

    for (node = header->head;  node != NULL;  node = node->next) {
        htp_status_t hrc;
        const char *value = (const char *)ib_bytestr_const_ptr(node->value);
        size_t vlen = ib_bytestr_length(node->value);

        if (value == NULL) {
            value = "";
            vlen = 0;
        }
        hrc = fn(htx,
                 (const char *)ib_bytestr_const_ptr(node->name),
                 ib_bytestr_length(node->name),
                 value, vlen,
                 HTP_ALLOC_COPY);
        if (hrc != HTP_OK) {
            return IB_EUNKNOWN;
        }
    }

    return IB_OK;
}

/**
 * Set a IronBee bytestring if it's NULL or empty from a libhtp bstr.
 *
 * @param[in] itx IronBee transaction
 * @param[in] label Label for logging
 * @param[in] force Set even if value already set
 * @param[in] htp_bstr HTP bstr to copy from
 * @param[in] fallback Fallback string (or NULL)
 * @param[in,out] ib_bstr Pointer to IronBee bytestring to fill
 *
 * @returns IronBee Status code
 */
static inline ib_status_t modhtp_set_bytestr(
    const ib_tx_t          *itx,
    const char             *label,
    bool                    force,
    const bstr             *htp_bstr,
    const char             *fallback,
    ib_bytestr_t          **ib_bstr)
{
    assert(itx != NULL);
    assert(label != NULL);
    assert(ib_bstr != NULL);

    ib_status_t    rc;
    const uint8_t *ptr = NULL;
    size_t         len = 0;

    /* If it's already set, do nothing */
    if ( (*ib_bstr != NULL) && (ib_bytestr_length(*ib_bstr) != 0) ) {
        if (! force) {
            return IB_OK;
        }
    }

    /* If it's not set in the htp bytestring, try the fallback. */
    if ( (htp_bstr == NULL) || (bstr_len(htp_bstr) == 0) ) {
        if (fallback == NULL) {
            ib_log_debug_tx(itx, "%s unknown: no fallback", label);
            return IB_OK;
        }
        ib_log_debug_tx(itx,
                        "%s unknown: using fallback \"%s\"", label, fallback);
        ptr = (const uint8_t *)fallback;
        len = strlen(fallback);
    }
    else {
        ptr = bstr_ptr(htp_bstr);
        len = bstr_len(htp_bstr);
    }

    /*
     * If the target bytestring is NULL, create it, otherwise
     * append to the zero-length bytestring.
     */
    if (*ib_bstr == NULL) {
        rc = ib_bytestr_dup_mem(ib_bstr, itx->mp, ptr, len);
    }
    else if (force) {
        void *new = ib_mpool_memdup(itx->mp, ptr, len);
        if (new == NULL) {
            rc = IB_EALLOC;
            goto done;
        }
        rc = ib_bytestr_setv(*ib_bstr, new, len);
    }
    else {
        rc = ib_bytestr_append_mem(*ib_bstr, ptr, len);
    }

done:
    if (rc != IB_OK) {
        ib_log_error_tx(itx, "Failed to set %s: %s",
                        label, ib_status_to_string(rc));
    }
    return rc;
}

/**
 * Set a NUL-terminated string if it's NULL or empty from a libhtp bstr.
 *
 * @param[in] itx IronBee transaction
 * @param[in] label Label for logging
 * @param[in] force Set even if value already set
 * @param[in] htp_bstr HTP bstr to copy from
 * @param[in] fallback Fallback string (or NULL)
 * @param[in,out] nulstr Pointer to NUL-terminated string to fill
 *
 * @returns Status code
 */
static inline ib_status_t modhtp_set_nulstr(
    const ib_tx_t          *itx,
    const char             *label,
    bool                    force,
    const bstr             *htp_bstr,
    const char             *fallback,
    const char            **nulstr)
{
    assert(itx != NULL);
    assert(label != NULL);
    assert(nulstr != NULL);

    const char  *ptr = NULL;
    size_t       len = 0;

    /* If it's already set, do nothing */
    if ( (*nulstr != NULL) && (**nulstr != '\0') ) {
        if (! force) {
            return IB_OK;
        }
    }

    /* If it's not set in the htp bytestring, try the fallback. */
    if ( (htp_bstr == NULL) || (bstr_len(htp_bstr) == 0) ) {
        if (fallback == NULL) {
            ib_log_debug_tx(itx, "%s unknown: no fallback", label);
            return IB_OK;
        }
        ib_log_debug_tx(itx,
                        "%s unknown: using fallback \"%s\"", label, fallback);
        ptr = fallback;
        len = strlen(fallback);
    }
    else {
        ptr = (const char *)bstr_ptr(htp_bstr);
        len = bstr_len(htp_bstr);
    }

    *nulstr = ib_mpool_memdup_to_str(itx->mp, ptr, len);
    return (*nulstr == NULL) ? IB_EALLOC : IB_OK;
}

/**
 * Get the transaction data for an IronBee transaction
 *
 * @param[in] itx IronBee transaction
 *
 * @returns Pointer to the transaction data
 */
static modhtp_txdata_t *modhtp_get_txdata_ibtx(
    const ib_tx_t    *itx)
{
    assert(itx != NULL);
    assert(itx->conn != NULL);
    modhtp_txdata_t *txdata;
    ib_status_t rc;

    rc = ib_tx_get_module_data(itx, IB_MODULE_STRUCT_PTR, &txdata);
    assert(rc == IB_OK);
    assert(txdata != NULL);
    assert(txdata->itx == itx);

    return txdata;
}

/**
 * Get the transaction data for a libhtp transaction
 *
 * @param[in] htx libhtp transaction
 *
 * @returns Pointer to the transaction data
 */
static modhtp_txdata_t *modhtp_get_txdata_htptx(
    const htp_tx_t   *htx)
{
    assert(htx != NULL);
    modhtp_txdata_t *txdata;

    txdata = (modhtp_txdata_t *)htp_tx_get_user_data(htx);
    assert(txdata != NULL);
    assert(txdata->htx == htx);

    return txdata;
}

/**
 * Get the transaction data for a libhtp connection parser
 *
 * @param[in] parser libhtp connection parser
 *
 * @returns Pointer to the transaction data
 */
static modhtp_txdata_t* modhtp_get_txdata_parser(
    const htp_connp_t  *parser)
{
    assert(parser != NULL);
    modhtp_txdata_t      *txdata;
    modhtp_parser_data_t *parser_data;

    parser_data = htp_connp_get_user_data(parser);
    assert(parser_data->parser == parser);

    txdata = htp_tx_get_user_data(parser->in_tx);
    assert(txdata != NULL);
    assert(txdata->parser_data != NULL);
    assert(txdata->parser_data == parser_data);

    return txdata;
}

/**
 * Check the modhtp connection parser status, get the related transactions
 *
 * @param[in] parser libhtp connection parser
 * @param[in] label Label string (for logging)
 * @param[out] ptxdata Pointer to transaction data
 *
 * @returns libhtp status code
 */
static inline ib_status_t modhtp_check_parser(
    htp_connp_t      *parser,
    const char       *label,
    modhtp_txdata_t **ptxdata)
{
    assert(parser != NULL);
    assert(label != NULL);
    assert(ptxdata != NULL);

    htp_log_t       *log;
    modhtp_txdata_t *txdata;

    /* Get the transaction data */
    txdata = modhtp_get_txdata_parser(parser);
    *ptxdata = txdata;

    /* Sanity checks */
    if ( (txdata->htx == NULL) || (txdata->itx == NULL) ) {
        ib_log_error(txdata->ib, "modhtp/%s: No transaction", label);
        return IB_EUNKNOWN;
    }

    /* Check the connection parser status */
    log = htp_connp_get_last_error(parser);
    if (log == NULL) {
        txdata->error_code = 0;
        txdata->error_msg = NULL;
        return IB_OK;
    }

    ib_log_warning_tx(txdata->itx,
                      "modhtp/%s: Parser error %d \"%s\"",
                      label,
                      log->code,
                      (log->msg == NULL) ? "UNKNOWN" : log->msg);
    txdata->error_code = log->code;
    if (log->msg == NULL) {
        txdata->error_msg = "UNKNOWN";
    }
    else  {
        txdata->error_msg = ib_mpool_strdup(txdata->itx->mp, log->msg);
        if (txdata->error_msg == NULL) {
            ib_log_error_tx(txdata->itx,
                            "modhtp/%s: Error strdup()ing error message \"%s\"",
                            label, log->msg);
            txdata->error_msg = "ib_mpool_strdup() failed!";
            return IB_EALLOC;
        }
    }

    return IB_OK;
}

/* -- Field Generation Routines -- */


/**
 * Generate an IB bytestring field from a libhtp bstr
 *
 * @param[in] tx IronBee transaction
 * @param[in] name Field name
 * @param[in] bs libhtp byte string
 * @param[in] copy true to copy the bstr, else use pointer to the data
 * @param[out] pf Pointer to the output field (or NULL)
 *
 * @returns Status code
 */
static ib_status_t modhtp_field_gen_bytestr(
    ib_tx_t     *tx,
    const char  *name,
    bstr        *bs,
    bool         copy,
    ib_field_t **pf)
{
    assert(tx != NULL);
    assert(name != NULL);

    ib_field_t   *f;
    ib_bytestr_t *ibs;
    ib_status_t   rc;
    uint8_t      *dptr;

    /* Initialize the field pointer */
    if (pf != NULL) {
        *pf = NULL;
    }

    /* If bs is NULL, do return ENOENT */
    if (bs == NULL) {
        ib_log_debug2_tx(tx, "HTP bytestr for for \"%s\" is NULL", name);
        return IB_ENOENT;
    }

    /* Make a copy of the bstr if need be */
    if (copy) {
        dptr = ib_mpool_memdup(tx->mp, (uint8_t *)bstr_ptr(bs), bstr_len(bs));
        if (dptr == NULL) {
            return IB_EALLOC;
        }
    }
    else {
        dptr = (uint8_t *)bstr_ptr(bs);
    }

    /* First lookup the field to see if there is already one
     * that needs the value set.
     */
    rc = ib_data_get(tx->data, name, &f);
    if (rc == IB_OK) {
        rc = ib_field_mutable_value(f, ib_ftype_bytestr_mutable_out(&ibs));
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to get field value for \"%s\": %s",
                            name, ib_status_to_string(rc));
            return rc;
        }

        rc = ib_bytestr_setv_const(ibs, dptr, bstr_len(bs));
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed to set field value for \"%s\": %s",
                            name, ib_status_to_string(rc));
            return rc;
        }
    }
    else {
        /* If no field exists, then create one. */
        rc = ib_data_add_bytestr_ex(tx->data, name, strlen(name),
                                    dptr, bstr_len(bs), &f);
        if (rc != IB_OK) {
            ib_log_error_tx(tx, "Failed add bytestring field for \"%s\": %s",
                            name, ib_status_to_string(rc));
            return rc;
        }
    }

    if (pf != NULL) {
        *pf = f;
    }
    return IB_OK;
}

#define modhtp_field_gen_list(data, name, pf) \
    ib_data_add_list_ex((data), (name), strlen((name)), (pf))

/* -- Utility functions -- */


/*
 * Macro to check a single libhtp parser flag, set appropriate IB TX flag.
 *
 * This macro invokes the modhtp_check_flag function.  This is implemented as
 * a macro so that we can use IB_STRINGIFY().
 *
 * @param[in,out] itx IronBee transaction
 * @param[in] collection Collection name
 * @param[in,out] flags Libhtp flags
 * @param[in] name flag Flag to check
 */
#define MODHTP_PROCESS_PARSER_FLAG(itx,collection,flags,flag)           \
    do {                                                                \
        if ((flags) & (HTP_##flag)) {                                   \
            modhtp_parser_flag(itx,                                     \
                               collection,                              \
                               &flags,                                  \
                               (HTP_##flag),                            \
                               IB_XSTRINGIFY(flag));                    \
        }                                                               \
    } while(0)

/**
 * Process and handle a single libhtp parser flag
 *
 * @param[in,out] itx IronBee transaction
 * @param[in] collection Collection name
 * @param[in] pflags Pointer to libhtp flags
 * @param[in] flagbit Libhtp flag bit to check
 * @param[in] flagname Flag name
 */
static void modhtp_parser_flag(
    ib_tx_t    *itx,
    const char *collection,
    uint64_t   *pflags,
    uint64_t    flagbit,
    const char *flagname)
{
    assert(itx != NULL);
    assert(itx->mp != NULL);
    assert(itx->data != NULL);
    assert(flagname != NULL);

    ib_status_t rc;
    ib_field_t *field;
    ib_field_t *listfield;
    ib_num_t value = 1;

    (*pflags) ^= flagbit;

    rc = ib_data_get(itx->data, collection, &field);
    if (rc == IB_ENOENT) {
        rc = ib_data_add_list(itx->data, collection, &field);
        if (rc != IB_OK) {
            ib_log_warning_tx(itx,
                              "Failed to add collection \"%s\": %s",
                              collection, ib_status_to_string(rc));
            return;
        }
    }
    rc = ib_field_create(&listfield,
                         itx->mp,
                         IB_FIELD_NAME(flagname),
                         IB_FTYPE_NUM,
                         ib_ftype_num_in(&value));
    if (rc != IB_OK) {
        ib_log_warning_tx(itx, "Failed to create \"%s\" flag field: %s",
                          flagname, ib_status_to_string(rc));
        return;
    }
    rc = ib_field_list_add(field, listfield);
    if (rc != IB_OK) {
        ib_log_warning_tx(itx,
                          "Failed to add \"%s\" flag to collection \"%s\": %s",
                          flagname, collection, ib_status_to_string(rc));
        return;
    }
    return;
}

static ib_status_t modhtp_set_parser_flags(
    modhtp_txdata_t  *txdata,
    const char       *collection)
{
    ib_status_t  rc = IB_OK;
    uint64_t     flags = txdata->htx->flags;
    ib_tx_t     *itx = txdata->itx;

    if (flags == 0) {
        return IB_OK;
    }

    /* FILED_xxxx */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, FIELD_UNPARSEABLE);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, FIELD_INVALID);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, FIELD_FOLDED);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, FIELD_REPEATED);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, FIELD_LONG);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, FIELD_RAW_NUL);

    /* REQUEST_SMUGGLING */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, REQUEST_SMUGGLING);

    /* INVALID_xxx */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, INVALID_FOLDING);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, INVALID_CHUNKING);

    /* MULTI_PACKET_HEAD */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, MULTI_PACKET_HEAD);

    /* HOST_xxx */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, HOST_MISSING);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, HOST_AMBIGUOUS);

    /* PATH_xxx */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_ENCODED_NUL);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_INVALID_ENCODING);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_INVALID);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_OVERLONG_U);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_ENCODED_SEPARATOR);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_UTF8_VALID);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_UTF8_INVALID);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_UTF8_OVERLONG);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, PATH_HALF_FULL_RANGE);

    /* STATUS_LINE_INVALID */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, STATUS_LINE_INVALID);

    /* HOSTx_INVALID */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, HOSTU_INVALID);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, HOSTH_INVALID);

    /* URLEN_xxx */
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, URLEN_ENCODED_NUL);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, URLEN_INVALID_ENCODING);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, URLEN_OVERLONG_U);
    MODHTP_PROCESS_PARSER_FLAG(itx, collection, flags, URLEN_HALF_FULL_RANGE);

    /* If flags is not 0 we did not handle one of the bits. */
    if (flags != 0) {
        ib_log_error_tx(itx, "HTP parser unknown flag: 0x%"PRIx64, flags);
        rc = IB_EUNKNOWN;
    }

    return rc;
}

/* -- LibHTP Callbacks -- */

static int modhtp_htp_req_start(
    htp_connp_t *connp)
{
    ib_status_t      irc;
    modhtp_txdata_t *txdata;
    
    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Request Start", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    return HTP_OK;
}

static int modhtp_htp_req_line(
    htp_connp_t   *connp,
    unsigned char *line,
    size_t         len)
{
    modhtp_txdata_t *txdata;
    ib_tx_t         *itx;
    htp_tx_t        *htx;
    ib_status_t      irc;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Request Line", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }
    itx = txdata->itx;
    htx = txdata->htx;

    /* Store the request line if required */
    if ( (itx->request_line->raw == NULL) ||
         (ib_bytestr_length(itx->request_line->raw) == 0) )
    {
        irc = ib_bytestr_dup_mem(&itx->request_line->raw, itx->mp, line, len);
        if (irc != IB_OK) {
            return HTP_ERROR;
        }
    }

    /* Store the request method */
    irc = modhtp_set_bytestr(itx, "Request method", false,
                             htx->request_method, NULL,
                             &(itx->request_line->method));
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    /* Store the request URI */
    irc = modhtp_set_bytestr(itx, "Request URI", false,
                             htx->request_uri, NULL,
                             &(itx->request_line->uri));
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    /* Store the request protocol */
    irc = modhtp_set_bytestr(itx, "Request protocol", false,
                             htx->request_protocol, NULL,
                             &(itx->request_line->protocol));
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    /* Store the transaction URI path. */
    irc = modhtp_set_nulstr(itx, "URI Path", false,
                            htx->parsed_uri->path, "/",
                            &(itx->path));
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    modhtp_set_parser_flags(txdata, "HTP_REQUEST_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_req_headers(
    htp_connp_t    *connp)
{
    modhtp_txdata_t *txdata;
    ib_status_t      irc;
    ib_tx_t         *itx;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Request Headers", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }
    itx = txdata->itx;

    /* Update the hostname that may have changed with headers. */
    irc = modhtp_set_nulstr(itx, "Hostname", true,
                            txdata->htx->parsed_uri->hostname,
                            itx->conn->local_ipstr,
                            &(itx->hostname));
    if (irc != IB_OK) {
        return irc;
    }

    modhtp_set_parser_flags(txdata, "HTP_REQUEST_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_req_body_data(
    htp_tx_data_t    *htp_tx_data)
{
    assert(htp_tx_data != NULL);

    modhtp_txdata_t *txdata;

    /* Get the txdata */
    txdata = modhtp_get_txdata_htptx(htp_tx_data->tx);
    modhtp_set_parser_flags(txdata, "HTP_REQUEST_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_req_trailer(
    htp_connp_t *connp)
{
    modhtp_txdata_t *txdata;
    ib_status_t      irc;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Request Trailer", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    modhtp_set_parser_flags(txdata, "HTP_REQUEST_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_req_complete(
    htp_connp_t *connp)
{
    modhtp_txdata_t *txdata;
    ib_status_t      irc;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Request Complete", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    modhtp_set_parser_flags(txdata, "HTP_REQUEST_FLAGS");
    return HTP_OK;
}

static int modhtp_htp_rsp_line(
    htp_connp_t *connp)
{
    modhtp_txdata_t *txdata;
    ib_tx_t         *itx;
    htp_tx_t        *htx;
    ib_status_t      irc;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Response Line", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }
    itx = txdata->itx;
    htx = txdata->htx;

    /* Store the response protocol */
    irc = modhtp_set_bytestr(itx, "Response protocol", false,
                             htx->response_protocol, NULL,
                             &itx->response_line->protocol);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    /* Store the response status */
    irc = modhtp_set_bytestr(itx, "Response status", false,
                             htx->response_status, NULL,
                             &itx->response_line->status);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    /* Store the request URI */
    irc = modhtp_set_bytestr(itx, "Response message", false,
                             htx->response_message, NULL,
                             &itx->response_line->msg);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    modhtp_set_parser_flags(txdata, "HTP_RESPONSE_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_rsp_headers(
    htp_connp_t *connp)
{
    modhtp_txdata_t *txdata;
    ib_status_t      irc;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Response Headers", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    modhtp_set_parser_flags(txdata, "HTP_RESPONSE_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_rsp_body_data(
    htp_tx_data_t *htp_tx_data)
{
    modhtp_txdata_t *txdata;

    /* Get the txdata */
    txdata = modhtp_get_txdata_htptx(htp_tx_data->tx);
    modhtp_set_parser_flags(txdata, "HTP_RESPONSE_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_rsp_complete(
    htp_connp_t *connp)
{
    modhtp_txdata_t *txdata;
    ib_status_t      irc;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Response Complete", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    modhtp_set_parser_flags(txdata, "HTP_RESPONSE_FLAGS");

    return HTP_OK;
}

static int modhtp_htp_rsp_trailer(
    htp_connp_t      *connp)
{
    modhtp_txdata_t *txdata;
    ib_status_t      irc;

    /* Check the parser status */
    irc = modhtp_check_parser(connp, "Response Trailser", &txdata);
    if (irc != IB_OK) {
        return HTP_ERROR;
    }

    modhtp_set_parser_flags(txdata, "HTP_RESPONSE_FLAGS");

    return HTP_OK;
}


/**
 * Generate IronBee request header fields
 *
 * @param[in] txdata Transaction data
 *
 * @returns IronBee status code
 */
static ib_status_t modhtp_gen_request_header_fields(
    const modhtp_txdata_t *txdata)
{
    assert(txdata != NULL);
    assert(txdata->itx != NULL);
    assert(txdata->htx != NULL);

    ib_field_t  *f;
    size_t       param_count = 0;
    ib_status_t  rc;
    ib_tx_t     *itx = txdata->itx;
    htp_tx_t    *htx = txdata->htx;
    bstr        *uri;

    modhtp_field_gen_bytestr(itx, "request_line",
                             htx->request_line, false, NULL);

    uri = htp_unparse_uri_noencode(htx->parsed_uri);
    if (uri == NULL) {
        ib_log_error_tx(itx, "Failed to generate normalized URI");
    }
    else {
        modhtp_field_gen_bytestr(itx, "request_uri", uri, true, NULL);
        bstr_free(uri);
    }

    modhtp_field_gen_bytestr(itx, "request_uri_scheme",
                             htx->parsed_uri->scheme, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_uri_username",
                             htx->parsed_uri->username, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_uri_password",
                             htx->parsed_uri->password, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_uri_host",
                             htx->parsed_uri->hostname, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_host",
                             htx->parsed_uri->hostname, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_uri_port",
                             htx->parsed_uri->port, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_uri_path",
                             htx->parsed_uri->path, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_uri_query",
                             htx->parsed_uri->query, false, NULL);

    modhtp_field_gen_bytestr(itx, "request_uri_fragment",
                             htx->parsed_uri->fragment, false, NULL);

    rc = ib_data_add_list(itx->data, "request_cookies", &f);
    if ( (htx->request_cookies != NULL) &&
         htp_table_size(htx->request_cookies) &&
         (rc == IB_OK) )
    {
        rc = modhtp_table_iterator(itx, htx->request_cookies,
                                   modhtp_field_list_callback, f);
        if (rc != IB_OK) {
            ib_log_warning_tx(itx, "Error adding request cookies");
        }
    }
    else if (rc == IB_OK) {
        ib_log_debug3_tx(itx, "No request cookies");
    }
    else {
        ib_log_error_tx(itx,
                        "Failed to create request cookies list: %s",
                        ib_status_to_string(rc));
    }

    /* Extract the query parameters into the IronBee tx's URI parameters */
    rc = ib_data_add_list(itx->data, "request_uri_params", &f);
    if ( (rc == IB_OK) && (htx->request_params != NULL) ) {
        modhtp_param_iter_data_t idata =
            { f, HTP_SOURCE_QUERY_STRING, 0 };
        rc = modhtp_table_iterator(itx, htx->request_params,
                                   modhtp_param_iter_callback, &idata);
        if (rc != IB_OK) {
            ib_log_warning_tx(itx, "Failed to populate URI params: %s",
                              ib_status_to_string(rc));
        }
        param_count = idata.count;
    }

    if (rc != IB_OK) {
        ib_log_error_tx(itx, "Failed to create request URI parameters: %s",
                        ib_status_to_string(rc));
    }
    else {
        ib_log_debug3_tx(itx, "%zd request URI parameters", param_count);
    }

    return IB_OK;
}

static ib_status_t modhtp_gen_request_fields(
    const htp_tx_t *htx,
    ib_tx_t        *itx)
{
    ib_field_t  *f;
    ib_status_t  rc;

    ib_log_debug3_tx(itx, "LibHTP: modhtp_gen_request_fields");
    if (htx == NULL) {
        return IB_OK;
    }

    /* Use the current parser transaction to generate fields. */
    /// @todo Check htp state, etc.
    size_t param_count = 0;

    rc = ib_data_add_list(itx->data, "request_body_params", &f);
    if ( (rc == IB_OK) && (htx->request_params != NULL) ) {
        modhtp_param_iter_data_t idata =
            { f, HTP_SOURCE_BODY, 0 };
        rc = modhtp_table_iterator(itx, htx->request_params,
                                   modhtp_param_iter_callback, &idata);
        if (rc != IB_OK) {
            ib_log_warning_tx(itx, "Failed to populate body params: %s",
                              ib_status_to_string(rc));
        }
        param_count = idata.count;
    }

    if (rc != IB_OK) {
        ib_log_error_tx(itx, "Failed to create request body parameters: %s",
                        ib_status_to_string(rc));
    }
    else {
        ib_log_debug3_tx(itx, "%zd request body parameters", param_count);
    }

    return IB_OK;
}

static ib_status_t modhtp_gen_response_header_fields(
    htp_tx_t *htx,
    ib_tx_t  *itx)
{
    return IB_OK;
}

static ib_status_t modhtp_gen_response_fields(
    htp_tx_t *htx,
    ib_tx_t  *itx)
{
    return IB_OK;
}

/* -- Parser Provider Interface Implementation -- */

/**
 * Create and populate a module configuration context object
 *
 * @param[in] ib IronBee engine
 * @param[in] mp Memory pool to use for allocations
 * @param[in] mod_config modhtp configuration structure
 * @param[out] pcontext Pointer to module context object
 *
 * @returns Staus code
 */
static ib_status_t modhtp_build_context (
    const ib_engine_t      *ib,
    ib_mpool_t             *mp,
    const modhtp_config_t  *mod_config,
    modhtp_context_t      **pcontext)
{
    assert(ib != NULL);
    assert(mp != NULL);
    assert(mod_config != NULL);
    assert(pcontext != NULL);

    int               personality;
    modhtp_context_t *context;
    htp_cfg_t        *htp_config;

    /* Figure out the personality to use. */
    personality = modhtp_personality(mod_config->personality);
    if (personality == -1) {
        personality = HTP_SERVER_APACHE_2;
    }

    /* Create a context. */
    context = ib_mpool_calloc(mp, 1, sizeof(*context));
    if (context == NULL) {
        return IB_EALLOC;
    }
    context->ib = ib;
    context->mod_config = mod_config;

    /* Create a parser configuration. */
    htp_config = htp_config_create();
    if (htp_config == NULL) {
        return IB_EALLOC;
    }
    context->htp_config = htp_config;

    /* Fill in the configuration */
    htp_config_set_server_personality(htp_config, personality);

    /* @todo Make all these configurable??? */
    htp_config->log_level = HTP_LOG_DEBUG2;
    htp_config_set_tx_auto_destroy(htp_config, 0);
    htp_config_set_generate_request_uri_normalized(htp_config, 1);

    htp_config_register_urlencoded_parser(htp_config);
    htp_config_register_multipart_parser(htp_config);
    htp_config_register_log(htp_config, modhtp_callback_log);

    /* Cookies */
    htp_config->parse_request_cookies = 1;

    /* Register callbacks. */
    htp_config_register_request_start(htp_config, modhtp_htp_req_start);
    htp_config_register_request_line(htp_config, modhtp_htp_req_line);
    htp_config_register_request_headers(htp_config, modhtp_htp_req_headers);
    htp_config_register_request_body_data(htp_config, modhtp_htp_req_body_data);
    htp_config_register_request_trailer(htp_config, modhtp_htp_req_trailer);
    htp_config_register_request_complete(htp_config, modhtp_htp_req_complete);
    htp_config_register_response_line(htp_config, modhtp_htp_rsp_line);
    htp_config_register_response_headers(htp_config, modhtp_htp_rsp_headers);
    htp_config_register_response_body_data(htp_config,modhtp_htp_rsp_body_data);
    htp_config_register_response_trailer(htp_config, modhtp_htp_rsp_trailer);
    htp_config_register_response_complete(htp_config, modhtp_htp_rsp_complete);

    *pcontext = context;
    return IB_OK;
}

/*****************************************************************************
 *
 * The server will call the parsed versions of the ib_state_notify_*()
 * functions directly with the parsed HTTP data. Because of limitations in
 * libhtp, the parsed functions implemented here will reconstruct a normalized
 * raw HTTP stream and call the data_in/data_out functions as if it was
 * receiving a raw stream.
 *
 ****************************************************************************/

static ib_status_t modhtp_iface_conn_init(
    ib_provider_inst_t *pi,
    ib_conn_t          *iconn)
{
    ib_engine_t            *ib = iconn->ib;
    ib_context_t           *ctx = iconn->ctx;
    ib_status_t             rc;
    modhtp_config_t        *config;
    const modhtp_context_t *context;
    modhtp_parser_data_t   *parser_data;
    htp_connp_t            *parser;

    /* Get the module config. */
    rc = ib_context_module_config(ctx, IB_MODULE_STRUCT_PTR, (void *)&config);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to fetch module %s config: %s",
                     MODULE_NAME_STR, ib_status_to_string(rc));
        return rc;
    }

    /* If no context, create one */
    context = config->context;
    if (context == NULL) {
        modhtp_context_t *new;
        rc = modhtp_build_context (ib, iconn->mp, config, &new);
        if (rc != IB_OK) {
            ib_log_error(ib, "Failed to create a configuration context");
        }
        context = new;
        config->context = context;
    }

    /* Create the connection parser */
    ib_log_debug3(ib, "Creating LibHTP parser");
    parser = htp_connp_create((htp_cfg_t *)context->htp_config);
    if (parser == NULL) {
        return IB_EALLOC;
    }

    /* Create the modhtp connection parser data struct */
    parser_data = ib_mpool_alloc(iconn->mp, sizeof(*parser_data));
    if (parser_data == NULL) {
        return IB_EALLOC;
    }
    parser_data->context = context;
    parser_data->parser  = parser;
    parser_data->iconn   = iconn;
    parser_data->parser  = parser;

    /* Store the parser data for access from callbacks. */
    ib_conn_parser_context_set(iconn, parser_data);
    htp_connp_set_user_data(parser, parser_data);

    return IB_OK;
}

static ib_status_t modhtp_iface_conn_cleanup(
    ib_provider_inst_t *pi,
    ib_conn_t          *iconn)
{
    modhtp_parser_data_t   *parser_data;

    /* Get the parser data */
    parser_data = ib_conn_parser_context_get(iconn);
    if (parser_data == NULL) {
        ib_log_error(iconn->ib,
                     "Failed to get connection parser data from IB connection");
        return IB_EUNKNOWN;
    }

    /* Destroy the parser on disconnect. */
    ib_log_debug3(iconn->ib, "Destroying LibHTP parser");
    htp_connp_destroy_all(parser_data->parser);

    return IB_OK;
}

static ib_status_t modhtp_iface_connect(
    ib_provider_inst_t *pi,
    ib_conn_t          *iconn)
{
    modhtp_parser_data_t *parser_data;

    /* Get the parser data */
    parser_data = ib_conn_parser_context_get(iconn);
    if (parser_data == NULL) {
        ib_log_error(iconn->ib,
                     "Failed to get connection parser data from IB connection");
        return IB_EUNKNOWN;
    }

    /* Open the connection */
    htp_connp_open(parser_data->parser,
                   iconn->remote_ipstr, iconn->remote_port,
                   iconn->local_ipstr, iconn->local_port,
                   &parser_data->open_time);

    return IB_OK;
}

static ib_status_t modhtp_iface_disconnect(
    ib_provider_inst_t *pi,
    ib_conn_t          *iconn)
{
    modhtp_parser_data_t *parser_data;

    /* Get the parser data */
    parser_data = (modhtp_parser_data_t *)ib_conn_parser_context_get(iconn);
    if (parser_data == NULL) {
        ib_log_error(iconn->ib,
                     "Failed to get connection parser data from IB connection");
        return IB_EUNKNOWN;
    }

    /*
     * @todo This seems to cause libhtp to go into an infinite loop in
     * the luajit test.
     *
     * htp_connp_close(parser_data->parser, &parser_data->close_time);
    */
    return IB_OK;
}

static ib_status_t modhtp_iface_tx_init(
    ib_provider_inst_t *pi,
    ib_tx_t            *itx)
{
    modhtp_txdata_t      *txdata;
    modhtp_parser_data_t *parser_data;
    htp_tx_t             *htx;
    ib_status_t           irc;

    /* Get the parser data from the transaction */
    parser_data = ib_conn_parser_context_get(itx->conn);
    if (parser_data == NULL) {
        ib_log_error_tx(itx, "Failed to get parser data for connection");
    }

    /* Create the transaction data */
    txdata = ib_mpool_calloc(itx->mp, sizeof(*txdata), 1);
    if (txdata == NULL) {
        ib_log_error_tx(itx, "Failed to allocate transaction data");
        return IB_EALLOC;
    }
    txdata->ib = itx->ib;
    txdata->itx = itx;
    txdata->parser_data = parser_data;

    /* Create the transaction */
    htx = htp_connp_tx_create(parser_data->parser);
    if (htx == NULL) {
        ib_log_error_tx(itx, "Failed to create HTP transaction");
        return IB_EALLOC;
    }
    txdata->htx = htx;

    /* Point both transactions at the transaction data */
    htp_tx_set_user_data(htx, txdata);
    irc = ib_tx_set_module_data(itx, IB_MODULE_STRUCT_PTR, txdata);
    if (irc != IB_OK) {
        return irc;
    }

    return IB_OK;
}

static ib_status_t modhtp_iface_tx_cleanup(
    ib_provider_inst_t *pi,
    ib_tx_t            *itx)
{
    assert(itx != NULL);
    assert(itx->conn != NULL);

    modhtp_txdata_t *txdata;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    /* Reset libhtp connection parser. */
    ib_log_debug_tx(itx, "Destroying LibHTP transaction (%p)", txdata->htx);
    htp_tx_destroy(txdata->htx);
    txdata->htx = NULL;

    htp_connp_clear_error(txdata->parser_data->parser);
    return IB_OK;
}

static ib_status_t modhtp_iface_request_line(
    ib_provider_inst_t   *pi,
    ib_tx_t              *itx,
    ib_parsed_req_line_t *line)
{
    assert(pi != NULL);
    assert(itx != NULL);
    assert(line != NULL);

    const modhtp_txdata_t *txdata;
    htp_status_t           hrc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    ib_log_debug_tx(itx,
                    "SEND REQUEST LINE TO LIBHTP: modhtp_iface_request_line");

    /* Start the request */
    hrc = htp_tx_state_request_start(txdata->htx);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    /* Hand the whole request line to libhtp */
    hrc = htp_tx_req_set_line(txdata->htx,
                              (const char *)ib_bytestr_const_ptr(line->raw),
                              ib_bytestr_length(line->raw),
                              HTP_ALLOC_COPY);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    /* Update the state */
    hrc = htp_tx_state_request_line(txdata->htx);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    return IB_OK;
}

static ib_status_t modhtp_iface_request_header_data(
    ib_provider_inst_t         *pi,
    ib_tx_t                    *itx,
    ib_parsed_header_wrapper_t *header)
{
    assert(pi != NULL);
    assert(itx != NULL);
    assert(header != NULL);

    const modhtp_txdata_t *txdata;
    ib_status_t            irc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    ib_log_debug_tx(itx,
                    "SEND REQUEST HEADER DATA TO LIBHTP: "
                    "modhtp_iface_request_header_data");

    /* Hand the headers off to libhtp */
    irc = modhtp_set_header(itx, txdata->htx, header, htp_tx_req_set_header);
    if (irc != IB_OK) {
        return irc;
    }

    return IB_OK;
}

static ib_status_t modhtp_iface_request_header_finished(
    ib_provider_inst_t *pi,
    ib_tx_t            *itx)
{
    assert(pi != NULL);
    assert(itx != NULL);

    const modhtp_txdata_t *txdata;
    ib_status_t            irc;
    htp_status_t           hrc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    /* Update the state */
    hrc = htp_tx_state_request_headers(txdata->htx);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    /* Generate header fields. */
    irc = modhtp_gen_request_header_fields(txdata);
    if (irc != IB_OK) {
        return irc;
    }

    ib_log_debug_tx(itx,
                    "SEND REQUEST HEADER FINISHED TO LIBHTP: "
                    "modhtp_iface_request_header_finished");

    return IB_OK;
}

static ib_status_t modhtp_iface_request_body_data(
    ib_provider_inst_t *pi,
    ib_tx_t            *itx,
    ib_txdata_t        *ib_txdata)
{
    assert(pi != NULL);
    assert(itx != NULL);
    assert(ib_txdata != NULL);

    const modhtp_txdata_t *txdata;
    htp_status_t           hrc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    ib_log_debug_tx(itx,
                    "SEND REQUEST BODY DATA TO LIBHTP: "
                    "modhtp_iface_request_body_data");

    /* Hand the request body data to libhtp. */
    hrc = htp_tx_req_process_body_data(txdata->htx,
                                       ib_txdata->data, ib_txdata->dlen);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    return IB_OK;
}

static ib_status_t modhtp_iface_request_finished(
    ib_provider_inst_t *pi,
    ib_tx_t            *itx)
{
    assert(pi != NULL);
    assert(itx != NULL);

    const modhtp_txdata_t *txdata;
    ib_status_t            irc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    /* Generate fields. */
    irc = modhtp_gen_request_fields(txdata->htx, itx);
    return irc;
}

static ib_status_t modhtp_iface_response_line(
    ib_provider_inst_t    *pi,
    ib_tx_t               *itx,
    ib_parsed_resp_line_t *line)
{
    assert(pi != NULL);
    assert(itx != NULL);

    const modhtp_txdata_t *txdata;
    htp_tx_t              *htx;
    htp_status_t           hrc;

    /* This is not valid for HTTP/0.9 requests. */
    if (line == NULL) {
        return IB_OK;
    }

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);
    htx = txdata->htx;

    ib_log_debug_tx(itx,
                    "SEND RESPONSE LINE TO LIBHTP: "
                    "modhtp_iface_response_line");

    /* Start the response transaction */
    hrc = htp_tx_state_response_start(htx);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    /* Hand off the status line */
    hrc = htp_tx_res_set_status_line(
        htx,
        (const char *)ib_bytestr_const_ptr(line->raw),
        ib_bytestr_length(line->raw),
        HTP_ALLOC_COPY);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    /* Set the state */
    hrc = htp_tx_state_response_line(htx);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    return IB_OK;
}

static ib_status_t modhtp_iface_response_header_data(
    ib_provider_inst_t         *pi,
    ib_tx_t                    *itx,
    ib_parsed_header_wrapper_t *header)
{
    assert(pi != NULL);
    assert(itx != NULL);
    assert(header != NULL);

    const modhtp_txdata_t *txdata;
    ib_status_t            irc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    /* This is required for parsed data only. */
    if (ib_conn_flags_isset(itx->conn, IB_CONN_FSEENDATAIN)) {
        return IB_OK;
    }

    ib_log_debug_tx(itx,
                    "SEND RESPONSE HEADER DATA TO LIBHTP: "
                    "modhtp_iface_response_header_data");

    /* Hand the response headers off to libhtp */
    irc = modhtp_set_header(itx, txdata->htx, header, htp_tx_res_set_header);
    if (irc != IB_OK) {
        return irc;
    }

    return IB_OK;
}

static ib_status_t modhtp_iface_response_header_finished(
    ib_provider_inst_t *pi,
    ib_tx_t *itx)
{
    assert(pi != NULL);
    assert(itx != NULL);

    const modhtp_txdata_t *txdata;
    ib_status_t            irc;
    htp_status_t           hrc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    /* Update the state */
    hrc = htp_tx_state_response_headers(txdata->htx);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    /* Generate header fields. */
    irc = modhtp_gen_response_header_fields(txdata->htx, itx);
    if (irc != IB_OK) {
        return irc;
    }

    /* This is required for parsed data only. */
    if (ib_conn_flags_isset(itx->conn, IB_CONN_FSEENDATAIN)) {
        return IB_OK;
    }

    ib_log_debug_tx(itx,
                    "SEND RESPONSE HEADER FINISHED TO LIBHTP: "
                    "modhtp_iface_response_header_finished");

    return IB_OK;
}

static ib_status_t modhtp_iface_response_body_data(
    ib_provider_inst_t *pi,
    ib_tx_t            *itx,
    ib_txdata_t        *ib_txdata)
{
    assert(pi != NULL);
    assert(itx != NULL);
    assert(ib_txdata != NULL);

    const modhtp_txdata_t *txdata;
    htp_status_t           hrc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    ib_log_debug_tx(itx,
                    "SEND RESPONSE BODY DATA TO LIBHTP: "
                    "modhtp_iface_response_body_data");

    hrc = htp_tx_res_process_body_data(txdata->htx,
                                       ib_txdata->data, ib_txdata->dlen);
    if (hrc != HTP_OK) {
        return IB_EUNKNOWN;
    }

    return IB_OK;
}

static ib_status_t modhtp_iface_response_finished(
    ib_provider_inst_t *pi,
    ib_tx_t            *itx)
{
    assert(pi != NULL);
    assert(itx != NULL);

    const modhtp_txdata_t *txdata;
    ib_status_t            irc;

    /* Fetch the transaction data */
    txdata = modhtp_get_txdata_ibtx(itx);

    /* Generate fields. */
    irc = modhtp_gen_response_fields(txdata->htx, itx);

    return irc;
}

static IB_PROVIDER_IFACE_TYPE(parser) modhtp_parser_iface = {
    IB_PROVIDER_IFACE_HEADER_DEFAULTS,

    /* Connection Init/Cleanup */
    modhtp_iface_conn_init,
    modhtp_iface_conn_cleanup,

    /* Connect/Disconnect */
    modhtp_iface_connect,
    modhtp_iface_disconnect,

    /* Transaction Init/Cleanup */
    modhtp_iface_tx_init,
    modhtp_iface_tx_cleanup,

    /* Request */
    modhtp_iface_request_line,
    modhtp_iface_request_header_data,
    modhtp_iface_request_header_finished,
    modhtp_iface_request_body_data,
    modhtp_iface_request_finished,

    /* Response */
    modhtp_iface_response_line,
    modhtp_iface_response_header_data,
    modhtp_iface_response_header_finished,
    modhtp_iface_response_body_data,
    modhtp_iface_response_finished,
};


/* -- Module Routines -- */

static ib_status_t modhtp_init(ib_engine_t *ib,
                               ib_module_t *m,
                               void        *cbdata)
{
    ib_status_t rc;

    /* Register as a parser provider. */
    rc = ib_provider_register(ib, IB_PROVIDER_TYPE_PARSER,
                              MODULE_NAME_STR, NULL,
                              &modhtp_parser_iface,
                              NULL);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     MODULE_NAME_STR ": Error registering htp parser provider: "
                     "%s", ib_status_to_string(rc));
        return IB_OK;
    }
    modhtp_ib = ib;

    return IB_OK;
}

static ib_status_t modhtp_context_close(
    ib_engine_t  *ib,
    ib_module_t  *m,
    ib_context_t *ctx,
    void         *cbdata)
{
    ib_status_t         rc;
    ib_provider_inst_t *pi;
    modhtp_config_t    *config;
    modhtp_context_t   *modctx;

    /* If there is not a parser set, then use this parser. */
    pi = ib_parser_provider_get_instance(ctx);
    if (pi == NULL) {
        ib_log_debug(ib, "Using \"%s\" parser by default in context %s.",
                     MODULE_NAME_STR, ib_context_full_get(ctx));

        /* Lookup/set this parser provider instance. */
        rc = ib_provider_instance_create(ib, IB_PROVIDER_TYPE_PARSER,
                                         MODULE_NAME_STR, &pi,
                                         ib_engine_pool_main_get(ib), NULL);
        if (rc != IB_OK) {
            ib_log_alert(ib, "Failed to create %s parser instance: %s",
                         MODULE_NAME_STR, ib_status_to_string(rc));
            return rc;
        }

        rc = ib_parser_provider_set_instance(ctx, pi);
        if (rc != IB_OK) {
            ib_log_alert(ib, "Failed to set %s as default parser: %s",
                         MODULE_NAME_STR, ib_status_to_string(rc));
            return rc;
        }
        pi = ib_parser_provider_get_instance(ctx);
    }

    /* Get the module config. */
    rc = ib_context_module_config(ctx, IB_MODULE_STRUCT_PTR, (void *)&config);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to fetch module %s config: %s",
                     MODULE_NAME_STR, ib_status_to_string(rc));
        return rc;
    }

    /* Build a context */
    rc = modhtp_build_context(ib, ib_engine_pool_main_get(ib), config, &modctx);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to create a module context for %s: %s",
                     MODULE_NAME_STR, ib_status_to_string(rc));
        return rc;
    }
    config->context = modctx;

    return IB_OK;
}

static IB_CFGMAP_INIT_STRUCTURE(modhtp_config_map) = {
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".personality",
        IB_FTYPE_NULSTR,
        modhtp_config_t,
        personality
    ),
    IB_CFGMAP_INIT_LAST
};

/**
 * Module structure.
 *
 * This structure defines some metadata, config data and various functions.
 */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,               /**< Default metadata */
    MODULE_NAME_STR,                         /**< Module name */
    IB_MODULE_CONFIG(&modhtp_global_config), /**< Global config data */
    modhtp_config_map,                       /**< Configuration field map */
    NULL,                                    /**< Config directive map */
    modhtp_init,                             /**< Initialize function */
    NULL,                                    /**< Callback data */
    NULL,                                    /**< Finish function */
    NULL,                                    /**< Callback data */
    NULL,                                    /**< Context open function */
    NULL,                                    /**< Callback data */
    modhtp_context_close,                    /**< Context close function */
    NULL,                                    /**< Callback data */
    NULL,                                    /**< Context destroy function */
    NULL                                     /**< Callback data */
);
