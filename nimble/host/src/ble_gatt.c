/**
 * Copyright (c) 2015 Runtime Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "os/os_mempool.h"
#include "nimble/ble.h"
#include "ble_gatt_priv.h"
#include "ble_hs_priv.h"
#include "host/ble_hs_uuid.h"
#include "ble_hs_conn.h"
#include "ble_att_cmd.h"
#include "ble_att_priv.h"

struct ble_gatt_entry {
    STAILQ_ENTRY(ble_gatt_entry) next;

    uint8_t op;
    uint8_t flags;
    uint16_t conn_handle;
    union {
        struct {
            int (*cb)(uint8_t ble_hs_status, uint8_t att_status,
                      uint16_t conn_handle, uint16_t mtu,
                      void *arg);
            void *cb_arg;
        } mtu;

        struct {
            uint16_t prev_handle;
            ble_gatt_disc_service_fn *cb;
            void *cb_arg;
        } disc_all_services;

        struct {
            uint8_t service_uuid[16];
            uint16_t prev_handle;
            ble_gatt_disc_service_fn *cb;
            void *cb_arg;
        } disc_service_uuid;

        struct {
            uint16_t prev_handle;
            uint16_t end_handle;
            ble_gatt_chr_fn *cb;
            void *cb_arg;
        } disc_all_chars;

        struct {
            uint16_t handle;
            ble_gatt_attr_fn *cb;
            void *cb_arg;
        } read;

        struct {
            uint16_t handle;
            void *value;
            uint16_t value_len;
            ble_gatt_write_fn *cb;
            void *cb_arg;
        } write;
    };
};

#define BLE_GATT_OP_NONE                        UINT8_MAX
#define BLE_GATT_OP_MTU                         0
#define BLE_GATT_OP_DISC_ALL_SERVICES           1
#define BLE_GATT_OP_DISC_SERVICE_UUID           2
#define BLE_GATT_OP_DISC_ALL_CHARS              3
#define BLE_GATT_OP_READ                        4
#define BLE_GATT_OP_WRITE_NO_RSP                5
#define BLE_GATT_OP_WRITE                       6
#define BLE_GATT_OP_MAX                         7

typedef int ble_gatt_kick_fn(struct ble_gatt_entry *entry);
typedef void ble_gatt_err_fn(struct ble_gatt_entry *entry,
                             uint8_t ble_hs_status, uint8_t att_status);

static int ble_gatt_kick_mtu(struct ble_gatt_entry *entry);
static int ble_gatt_kick_disc_all_services(struct ble_gatt_entry *entry);
static int ble_gatt_kick_disc_service_uuid(struct ble_gatt_entry *entry);
static int ble_gatt_kick_disc_all_chars(struct ble_gatt_entry *entry);
static int ble_gatt_kick_read(struct ble_gatt_entry *entry);
static int ble_gatt_kick_write_no_rsp(struct ble_gatt_entry *entry);
static int ble_gatt_kick_write(struct ble_gatt_entry *entry);

static void ble_gatt_err_mtu(struct ble_gatt_entry *entry,
                             uint8_t ble_hs_status, uint8_t att_status);
static void ble_gatt_err_disc_all_services(struct ble_gatt_entry *entry,
                                           uint8_t ble_hs_status,
                                           uint8_t att_status);
static void ble_gatt_err_disc_service_uuid(struct ble_gatt_entry *entry,
                                           uint8_t ble_hs_status,
                                           uint8_t att_status);
static void ble_gatt_err_disc_all_chars(struct ble_gatt_entry *entry,
                                        uint8_t ble_hs_status,
                                        uint8_t att_status);
static void ble_gatt_err_read(struct ble_gatt_entry *entry,
                              uint8_t ble_hs_status, uint8_t att_status);
static void ble_gatt_err_write(struct ble_gatt_entry *entry,
                               uint8_t ble_hs_status, uint8_t att_status);

struct ble_gatt_dispatch_entry {
    ble_gatt_kick_fn *kick_cb;
    ble_gatt_err_fn *err_cb;
};

static const struct ble_gatt_dispatch_entry
    ble_gatt_dispatch[BLE_GATT_OP_MAX] = {

    [BLE_GATT_OP_MTU] = {
        .kick_cb = ble_gatt_kick_mtu,
        .err_cb = ble_gatt_err_mtu,
    },
    [BLE_GATT_OP_DISC_ALL_SERVICES] = {
        .kick_cb = ble_gatt_kick_disc_all_services,
        .err_cb = ble_gatt_err_disc_all_services,
    },
    [BLE_GATT_OP_DISC_SERVICE_UUID] = {
        .kick_cb = ble_gatt_kick_disc_service_uuid,
        .err_cb = ble_gatt_err_disc_service_uuid,
    },
    [BLE_GATT_OP_DISC_ALL_CHARS] = {
        .kick_cb = ble_gatt_kick_disc_all_chars,
        .err_cb = ble_gatt_err_disc_all_chars,
    },
    [BLE_GATT_OP_READ] = {
        .kick_cb = ble_gatt_kick_read,
        .err_cb = ble_gatt_err_read,
    },
    [BLE_GATT_OP_WRITE_NO_RSP] = {
        .kick_cb = ble_gatt_kick_write_no_rsp,
        .err_cb = NULL,
    },
    [BLE_GATT_OP_WRITE] = {
        .kick_cb = ble_gatt_kick_write,
        .err_cb = ble_gatt_err_write,
    },
};

#define BLE_GATT_ENTRY_F_PENDING    0x01
#define BLE_GATT_ENTRY_F_EXPECTING  0x02

#define BLE_GATT_NUM_ENTRIES          4
static void *ble_gatt_entry_mem;
static struct os_mempool ble_gatt_entry_pool;

static STAILQ_HEAD(, ble_gatt_entry) ble_gatt_list;

/*****************************************************************************
 * @entry                                                                    *
 *****************************************************************************/

static const struct ble_gatt_dispatch_entry *
ble_gatt_dispatch_get(uint8_t op)
{
    assert(op < BLE_GATT_OP_MAX);
    return ble_gatt_dispatch + op;
}

static struct ble_gatt_entry *
ble_gatt_entry_alloc(void)
{
    struct ble_gatt_entry *entry;

    entry = os_memblock_get(&ble_gatt_entry_pool);
    if (entry != NULL) {
        memset(entry, 0, sizeof *entry);
    }

    return entry;
}

static void
ble_gatt_entry_free(struct ble_gatt_entry *entry)
{
    int rc;

    rc = os_memblock_put(&ble_gatt_entry_pool, entry);
    assert(rc == 0);
}

static void
ble_gatt_entry_remove(struct ble_gatt_entry *entry,
                      struct ble_gatt_entry *prev)
{
    if (prev == NULL) {
        assert(STAILQ_FIRST(&ble_gatt_list) == entry);
        STAILQ_REMOVE_HEAD(&ble_gatt_list, next);
    } else {
        assert(STAILQ_NEXT(prev, next) == entry);
        STAILQ_NEXT(prev, next) = STAILQ_NEXT(entry, next);
    }
}

static void
ble_gatt_entry_remove_free(struct ble_gatt_entry *entry,
                           struct ble_gatt_entry *prev)
{
    ble_gatt_entry_remove(entry, prev);
    ble_gatt_entry_free(entry);
}

static int
ble_gatt_entry_matches(struct ble_gatt_entry *entry, uint16_t conn_handle,
                       uint8_t att_op, int expecting_only)
{
    if (conn_handle != entry->conn_handle) {
        return 0;
    }

    if (att_op != entry->op && att_op != BLE_GATT_OP_NONE) {
        return 0;
    }

    if (expecting_only &&
        !(entry->flags & BLE_GATT_ENTRY_F_EXPECTING)) {

        return 0;
    }

    return 1;
}

static struct ble_gatt_entry *
ble_gatt_find(uint16_t conn_handle, uint8_t att_op, int expecting_only,
              struct ble_gatt_entry **out_prev)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    prev = NULL;
    STAILQ_FOREACH(entry, &ble_gatt_list, next) {
        if (ble_gatt_entry_matches(entry, conn_handle, att_op,
                                   expecting_only)) {
            if (out_prev != NULL) {
                *out_prev = prev;
            }
            return entry;
        }

        prev = entry;
    }

    return NULL;
}

static void
ble_gatt_entry_set_pending(struct ble_gatt_entry *entry)
{
    assert(!(entry->flags & BLE_GATT_ENTRY_F_PENDING));

    entry->flags &= ~BLE_GATT_ENTRY_F_EXPECTING;
    entry->flags |= BLE_GATT_ENTRY_F_PENDING;
    ble_hs_kick_gatt();
}

static void
ble_gatt_entry_set_expecting(struct ble_gatt_entry *entry,
                             struct ble_gatt_entry *prev)
{
    assert(!(entry->flags & BLE_GATT_ENTRY_F_EXPECTING));

    ble_gatt_entry_remove(entry, prev);
    entry->flags &= ~BLE_GATT_ENTRY_F_PENDING;
    entry->flags |= BLE_GATT_ENTRY_F_EXPECTING;
    STAILQ_INSERT_TAIL(&ble_gatt_list, entry, next);
}

static int
ble_gatt_new_entry(uint16_t conn_handle, uint8_t op,
                   struct ble_gatt_entry **entry)
{
    struct ble_hs_conn *conn;

    *entry = NULL;

    /* Ensure we have a connection with the specified handle. */
    conn = ble_hs_conn_find(conn_handle);
    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    }

    *entry = ble_gatt_entry_alloc();
    if (*entry == NULL) {
        return BLE_HS_ENOMEM;
    }

    memset(*entry, 0, sizeof **entry);
    (*entry)->op = op;
    (*entry)->conn_handle = conn_handle;

    STAILQ_INSERT_TAIL(&ble_gatt_list, *entry, next);

    ble_gatt_entry_set_pending(*entry);

    return 0;
}

/*****************************************************************************
 * @mtu                                                                      *
 *****************************************************************************/

static int
ble_gatt_mtu_cb(struct ble_gatt_entry *entry, uint8_t ble_hs_status,
                uint8_t att_status, uint16_t mtu)
{
    int rc;

    if (entry->mtu.cb == NULL) {
        rc = 0;
    } else {
        rc = entry->mtu.cb(entry->conn_handle, ble_hs_status, att_status, mtu,
                           entry->mtu.cb_arg);
    }

    return rc;
}

static int
ble_gatt_kick_mtu(struct ble_gatt_entry *entry)
{
    struct ble_att_mtu_cmd req;
    struct ble_l2cap_chan *chan;
    struct ble_hs_conn *conn;
    int rc;

    conn = ble_hs_conn_find(entry->conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto err;
    }

    chan = ble_hs_conn_chan_find(conn, BLE_L2CAP_CID_ATT);
    assert(chan != NULL);

    req.bamc_mtu = chan->blc_my_mtu;
    rc = ble_att_clt_tx_mtu(conn, &req);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gatt_mtu_cb(entry, rc, 0, 0);
    return rc;
}

static void
ble_gatt_err_mtu(struct ble_gatt_entry *entry, uint8_t ble_hs_status,
                 uint8_t att_status)
{
    ble_gatt_mtu_cb(entry, ble_hs_status, att_status, 0);
}

void
ble_gatt_rx_mtu(struct ble_hs_conn *conn, uint16_t chan_mtu)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_MTU, 1, &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    ble_gatt_mtu_cb(entry, 0, 0, chan_mtu);
    ble_gatt_entry_remove_free(entry, prev);
}

int
ble_gatt_exchange_mtu(uint16_t conn_handle)
{
    struct ble_gatt_entry *entry;
    int rc;

    rc = ble_gatt_new_entry(conn_handle, BLE_GATT_OP_MTU, &entry);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/*****************************************************************************
 * @discover all services                                                    *
 *****************************************************************************/

static int
ble_gatt_disc_all_services_cb(struct ble_gatt_entry *entry,
                              uint8_t ble_hs_status, uint8_t att_status,
                              struct ble_gatt_service *service)
{
    int rc;

    if (entry->disc_all_services.cb == NULL) {
        rc = 0;
    } else {
        rc = entry->disc_all_services.cb(entry->conn_handle, ble_hs_status,
                                         att_status, service,
                                         entry->disc_all_services.cb_arg);
    }

    return rc;
}

static int
ble_gatt_kick_disc_all_services(struct ble_gatt_entry *entry)
{
    struct ble_att_read_group_type_req req;
    struct ble_hs_conn *conn;
    uint8_t uuid128[16];
    int rc;

    conn = ble_hs_conn_find(entry->conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto err;
    }

    rc = ble_hs_uuid_from_16bit(BLE_ATT_UUID_PRIMARY_SERVICE, uuid128);
    assert(rc == 0);

    req.bagq_start_handle = entry->disc_all_services.prev_handle + 1;
    req.bagq_end_handle = 0xffff;
    rc = ble_att_clt_tx_read_group_type(conn, &req, uuid128);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gatt_disc_all_services_cb(entry, rc, 0, NULL);
    return rc;
}

static void
ble_gatt_err_disc_all_services(struct ble_gatt_entry *entry,
                               uint8_t ble_hs_status, uint8_t att_status)
{
    if (ble_hs_status == BLE_HS_EATT &&
        att_status == BLE_ATT_ERR_ATTR_NOT_FOUND) {

        /* Discovery is complete. */
        ble_hs_status = 0;
        att_status = 0;
    }

    ble_gatt_disc_all_services_cb(entry, ble_hs_status, att_status, NULL);
}

void
ble_gatt_rx_read_group_type_adata(struct ble_hs_conn *conn,
                                  struct ble_att_clt_adata *adata)
{
    struct ble_gatt_service service;
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;
    uint16_t uuid16;
    int cbrc;
    int rc;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_DISC_ALL_SERVICES, 1,
                          &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    switch (adata->value_len) {
    case 2:
        uuid16 = le16toh(adata->value);
        rc = ble_hs_uuid_from_16bit(uuid16, service.uuid128);
        if (rc != 0) {
            goto done;
        }
        break;

    case 16:
        memcpy(service.uuid128, adata->value, 16);
        break;

    default:
        rc = BLE_HS_EMSGSIZE;
        goto done;
    }

    entry->disc_all_services.prev_handle = adata->end_group_handle;

    service.start_handle = adata->att_handle;
    service.end_handle = adata->end_group_handle;

    rc = 0;

done:
    cbrc = ble_gatt_disc_all_services_cb(entry, rc, 0, &service);
    if (rc != 0 || cbrc != 0) {
        ble_gatt_entry_remove_free(entry, prev);
    }
}

void
ble_gatt_rx_read_group_type_complete(struct ble_hs_conn *conn, int rc)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_DISC_ALL_SERVICES, 1,
                          &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    if (rc != 0 || entry->disc_all_services.prev_handle == 0xffff) {
        /* Error or all services discovered. */
        ble_gatt_disc_all_services_cb(entry, rc, 0, NULL);
        ble_gatt_entry_remove_free(entry, prev);
    } else {
        /* Send follow-up request. */
        ble_gatt_entry_set_pending(entry);
    }
}

int
ble_gatt_disc_all_services(uint16_t conn_handle, ble_gatt_disc_service_fn *cb,
                           void *cb_arg)
{
    struct ble_gatt_entry *entry;
    int rc;

    rc = ble_gatt_new_entry(conn_handle, BLE_GATT_OP_DISC_ALL_SERVICES,
                            &entry);
    if (rc != 0) {
        return rc;
    }
    entry->disc_all_services.prev_handle = 0x0000;
    entry->disc_all_services.cb = cb;
    entry->disc_all_services.cb_arg = cb_arg;

    return 0;
}

/*****************************************************************************
 * @discover service by uuid                                                 *
 *****************************************************************************/

static int
ble_gatt_disc_service_uuid_cb(struct ble_gatt_entry *entry,
                              uint8_t ble_hs_status, uint8_t att_status,
                              struct ble_gatt_service *service)
{
    int rc;

    if (entry->disc_service_uuid.cb == NULL) {
        rc = 0;
    } else {
        rc = entry->disc_service_uuid.cb(entry->conn_handle, ble_hs_status,
                                         att_status, service,
                                         entry->disc_service_uuid.cb_arg);
    }

    return rc;
}

static int
ble_gatt_kick_disc_service_uuid(struct ble_gatt_entry *entry)
{
    struct ble_att_find_type_value_req req;
    struct ble_hs_conn *conn;
    int rc;

    conn = ble_hs_conn_find(entry->conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto err;
    }

    req.bavq_start_handle = entry->disc_service_uuid.prev_handle + 1;
    req.bavq_end_handle = 0xffff;
    req.bavq_attr_type = BLE_ATT_UUID_PRIMARY_SERVICE;

    rc = ble_att_clt_tx_find_type_value(conn, &req,
                                        entry->disc_service_uuid.service_uuid,
                                        16);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gatt_disc_service_uuid_cb(entry, rc, 0, NULL);
    return rc;
}

static void
ble_gatt_err_disc_service_uuid(struct ble_gatt_entry *entry,
                               uint8_t ble_hs_status, uint8_t att_status)
{
    if (ble_hs_status == BLE_HS_EATT &&
        att_status == BLE_ATT_ERR_ATTR_NOT_FOUND) {

        /* Discovery is complete. */
        ble_hs_status = 0;
        att_status = 0;
    }

    ble_gatt_disc_service_uuid_cb(entry, ble_hs_status, att_status, NULL);
}

void
ble_gatt_rx_find_type_value_hinfo(struct ble_hs_conn *conn,
                                  struct ble_att_clt_adata *adata)
{
    struct ble_gatt_service service;
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;
    int rc;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_DISC_SERVICE_UUID, 1,
                          &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    service.start_handle = adata->att_handle;
    service.end_handle = adata->end_group_handle;
    memcpy(service.uuid128, entry->disc_service_uuid.service_uuid, 16);

    rc = ble_gatt_disc_service_uuid_cb(entry, 0, 0, &service);
    if (rc != 0) {
        ble_gatt_entry_remove_free(entry, prev);
    }
}

void
ble_gatt_rx_find_type_value_complete(struct ble_hs_conn *conn, int rc)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_DISC_SERVICE_UUID, 1,
                          &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    if (rc != 0 || entry->disc_service_uuid.prev_handle == 0xffff) {
        /* Error or all services discovered. */
        ble_gatt_disc_service_uuid_cb(entry, rc, 0, NULL);
        ble_gatt_entry_remove_free(entry, prev);
    } else {
        /* Send follow-up request. */
        ble_gatt_entry_set_pending(entry);
    }
}

int
ble_gatt_disc_service_by_uuid(uint16_t conn_handle, void *service_uuid128,
                              ble_gatt_disc_service_fn *cb, void *cb_arg)
{
    struct ble_gatt_entry *entry;
    int rc;

    rc = ble_gatt_new_entry(conn_handle, BLE_GATT_OP_DISC_SERVICE_UUID,
                            &entry);
    if (rc != 0) {
        return rc;
    }
    memcpy(entry->disc_service_uuid.service_uuid, service_uuid128, 16);
    entry->disc_service_uuid.prev_handle = 0x0000;
    entry->disc_service_uuid.cb = cb;
    entry->disc_service_uuid.cb_arg = cb_arg;

    return 0;
}

/*****************************************************************************
 * @discover all characteristics                                             *
 *****************************************************************************/

static int
ble_gatt_disc_all_chars_cb(struct ble_gatt_entry *entry, uint8_t ble_hs_status,
                           uint8_t att_status, struct ble_gatt_chr *chr)
{
    int rc;

    if (entry->disc_all_chars.cb == NULL) {
        rc = 0;
    } else {
        rc = entry->disc_all_chars.cb(entry->conn_handle, ble_hs_status,
                                      att_status, chr,
                                      entry->disc_all_chars.cb_arg);
    }

    return rc;
}

static int
ble_gatt_kick_disc_all_chars(struct ble_gatt_entry *entry)
{
    struct ble_att_read_type_req req;
    struct ble_hs_conn *conn;
    uint8_t uuid128[16];
    int rc;

    conn = ble_hs_conn_find(entry->conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto err;
    }

    rc = ble_hs_uuid_from_16bit(BLE_ATT_UUID_CHARACTERISTIC, uuid128);
    assert(rc == 0);

    req.batq_start_handle = entry->disc_all_chars.prev_handle + 1;
    req.batq_end_handle = entry->disc_all_chars.end_handle;

    rc = ble_att_clt_tx_read_type(conn, &req, uuid128);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gatt_disc_all_chars_cb(entry, rc, 0, NULL);
    return rc;
}

static void
ble_gatt_err_disc_all_chars(struct ble_gatt_entry *entry,
                            uint8_t ble_hs_status, uint8_t att_status)
{
    if (ble_hs_status == BLE_HS_EATT &&
        att_status == BLE_ATT_ERR_ATTR_NOT_FOUND) {

        /* Discovery is complete. */
        ble_hs_status = 0;
        att_status = 0;
    }

    ble_gatt_disc_all_chars_cb(entry, ble_hs_status, att_status, NULL);
}

void
ble_gatt_rx_read_type_adata(struct ble_hs_conn *conn,
                            struct ble_att_clt_adata *adata)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;
    struct ble_gatt_chr chr;
    uint16_t uuid16;
    int cbrc;
    int rc;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_DISC_ALL_CHARS, 1,
                          &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    memset(&chr, 0, sizeof chr);
    chr.decl_handle = adata->att_handle;

    switch (adata->value_len) {
    case BLE_GATT_CHR_DECL_SZ_16:
        uuid16 = le16toh(adata->value + 3);
        rc = ble_hs_uuid_from_16bit(uuid16, chr.uuid128);
        if (rc != 0) {
            rc = BLE_HS_EBADDATA;
            goto done;
        }
        break;

    case BLE_GATT_CHR_DECL_SZ_128:
        memcpy(chr.uuid128, adata->value + 3, 16);
        break;

    default:
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    chr.properties = adata->value[0];
    chr.value_handle = le16toh(adata->value + 1);

    entry->disc_all_chars.prev_handle = adata->att_handle;

    rc = 0;

done:
    cbrc = ble_gatt_disc_all_chars_cb(entry, rc, 0, &chr);
    if (rc != 0 || cbrc != 0) {
        ble_gatt_entry_remove_free(entry, prev);
    }
}

void
ble_gatt_rx_read_type_complete(struct ble_hs_conn *conn, int rc)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_DISC_ALL_CHARS, 1,
                          &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    if (rc != 0 || entry->disc_all_chars.prev_handle ==
                   entry->disc_all_chars.end_handle) {
        /* Error or all services discovered. */
        ble_gatt_disc_all_chars_cb(entry, rc, 0, NULL);
        ble_gatt_entry_remove_free(entry, prev);
    } else {
        /* Send follow-up request. */
        ble_gatt_entry_set_pending(entry);
    }
}

int
ble_gatt_disc_all_chars(uint16_t conn_handle, uint16_t start_handle,
                        uint16_t end_handle, ble_gatt_chr_fn *cb,
                        void *cb_arg)
{
    struct ble_gatt_entry *entry;
    int rc;

    rc = ble_gatt_new_entry(conn_handle, BLE_GATT_OP_DISC_ALL_CHARS, &entry);
    if (rc != 0) {
        return rc;
    }
    entry->disc_all_chars.prev_handle = start_handle - 1;
    entry->disc_all_chars.end_handle = end_handle;
    entry->disc_all_chars.cb = cb;
    entry->disc_all_chars.cb_arg = cb_arg;

    return 0;
}

/*****************************************************************************
 * @read                                                                     *
 *****************************************************************************/

static int
ble_gatt_read_cb(struct ble_gatt_entry *entry, uint8_t ble_hs_status,
                 uint8_t att_status, struct ble_gatt_attr *attr)
{
    int rc;

    if (entry->read.cb == NULL) {
        rc = 0;
    } else {
        rc = entry->read.cb(entry->conn_handle, ble_hs_status, att_status,
                            attr, entry->read.cb_arg);
    }

    return rc;
}

static int
ble_gatt_kick_read(struct ble_gatt_entry *entry)
{
    struct ble_att_read_req req;
    struct ble_hs_conn *conn;
    int rc;

    conn = ble_hs_conn_find(entry->conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto err;
    }

    req.barq_handle = entry->read.handle;
    rc = ble_att_clt_tx_read(conn, &req);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gatt_read_cb(entry, rc, 0, NULL);
    return rc;
}

static void
ble_gatt_err_read(struct ble_gatt_entry *entry, uint8_t ble_hs_status,
                  uint8_t att_status)
{
    ble_gatt_read_cb(entry, ble_hs_status, att_status, NULL);
}

void
ble_gatt_rx_read_rsp(struct ble_hs_conn *conn, int status, void *value,
                     int value_len)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;
    struct ble_gatt_attr attr;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_READ, 1, &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    attr.handle = entry->read.handle;
    attr.value_len = value_len;
    attr.value = value;

    ble_gatt_read_cb(entry, status, 0, &attr);

    /* The read operation only has a single request / response exchange. */
    ble_gatt_entry_remove_free(entry, prev);
}

int
ble_gatt_read(uint16_t conn_handle, uint16_t attr_handle,
              ble_gatt_attr_fn *cb, void *cb_arg)
{
    struct ble_gatt_entry *entry;
    int rc;

    rc = ble_gatt_new_entry(conn_handle, BLE_GATT_OP_READ, &entry);
    if (rc != 0) {
        return rc;
    }
    entry->read.handle = attr_handle;
    entry->read.cb = cb;
    entry->read.cb_arg = cb_arg;

    return 0;
}

/*****************************************************************************
 * @write no response                                                        *
 *****************************************************************************/

static int
ble_gatt_write_cb(struct ble_gatt_entry *entry, uint8_t ble_hs_status,
                  uint8_t att_status, uint16_t attr_handle)
{
    int rc;

    if (entry->write.cb == NULL) {
        rc = 0;
    } else {
        rc = entry->write.cb(entry->conn_handle, ble_hs_status, att_status,
                             attr_handle, entry->write.cb_arg);
    }

    return rc;
}

static int
ble_gatt_kick_write_no_rsp(struct ble_gatt_entry *entry)
{
    struct ble_att_write_req req;
    struct ble_hs_conn *conn;
    int rc;

    conn = ble_hs_conn_find(entry->conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto err;
    }

    req.bawq_handle = entry->write.handle;
    rc = ble_att_clt_tx_write_cmd(conn, &req, entry->write.value,
                                  entry->write.value_len);
    if (rc != 0) {
        goto err;
    }

    /* No response expected; call callback immediately and return nonzero to
     * indicate the entry should be freed.
     */
    ble_gatt_write_cb(entry, 0, 0, entry->write.handle);

    return 1;

err:
    ble_gatt_write_cb(entry, rc, 0, entry->write.handle);
    return rc;
}

int
ble_gatt_write_no_rsp(uint16_t conn_handle, uint16_t attr_handle, void *value,
                      uint16_t value_len, ble_gatt_write_fn *cb, void *cb_arg)
{
    struct ble_gatt_entry *entry;
    int rc;

    rc = ble_gatt_new_entry(conn_handle, BLE_GATT_OP_WRITE_NO_RSP, &entry);
    if (rc != 0) {
        return rc;
    }

    entry->write.handle = attr_handle;
    entry->write.value = value;
    entry->write.value_len = value_len;
    entry->write.cb = cb;
    entry->write.cb_arg = cb_arg;

    return 0;
}

/*****************************************************************************
 * @write                                                                    *
 *****************************************************************************/

static int
ble_gatt_kick_write(struct ble_gatt_entry *entry)
{
    struct ble_att_write_req req;
    struct ble_hs_conn *conn;
    int rc;

    conn = ble_hs_conn_find(entry->conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto err;
    }

    req.bawq_handle = entry->write.handle;
    rc = ble_att_clt_tx_write_req(conn, &req, entry->write.value,
                                  entry->write.value_len);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gatt_write_cb(entry, rc, 0, entry->write.handle);
    return rc;
}

static void
ble_gatt_err_write(struct ble_gatt_entry *entry, uint8_t ble_hs_status,
                   uint8_t att_status)
{
    ble_gatt_write_cb(entry, ble_hs_status, att_status, 0);
}

void
ble_gatt_rx_write_rsp(struct ble_hs_conn *conn)
{
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    entry = ble_gatt_find(conn->bhc_handle, BLE_GATT_OP_WRITE, 1, &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    ble_gatt_write_cb(entry, 0, 0, entry->write.handle);

    /* The write operation only has a single request / response exchange. */
    ble_gatt_entry_remove_free(entry, prev);
}

int
ble_gatt_write(uint16_t conn_handle, uint16_t attr_handle, void *value,
               uint16_t value_len, ble_gatt_write_fn *cb, void *cb_arg)
{
    struct ble_gatt_entry *entry;
    int rc;

    rc = ble_gatt_new_entry(conn_handle, BLE_GATT_OP_WRITE, &entry);
    if (rc != 0) {
        return rc;
    }

    entry->write.handle = attr_handle;
    entry->write.value = value;
    entry->write.value_len = value_len;
    entry->write.cb = cb;
    entry->write.cb_arg = cb_arg;

    return 0;
}

/*****************************************************************************
 * @misc                                                                     *
 *****************************************************************************/

void
ble_gatt_wakeup(void)
{
    const struct ble_gatt_dispatch_entry *dispatch;
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;
    struct ble_gatt_entry *next;
    int rc;

    prev = NULL;
    entry = STAILQ_FIRST(&ble_gatt_list);
    while (entry != NULL) {
        next = STAILQ_NEXT(entry, next);

        if (entry->flags & BLE_GATT_ENTRY_F_PENDING) {
            dispatch = ble_gatt_dispatch_get(entry->op);
            rc = dispatch->kick_cb(entry);
            if (rc == 0) {
                ble_gatt_entry_set_expecting(entry, prev);
                /* Current entry got moved to back; old prev still valid. */
            } else {
                ble_gatt_entry_remove_free(entry, prev);
                /* Current entry removed; old prev still valid. */
            }
        } else {
            prev = entry;
        }

        entry = next;
    }
}

void
ble_gatt_rx_err(uint16_t conn_handle, struct ble_att_error_rsp *rsp)
{
    const struct ble_gatt_dispatch_entry *dispatch;
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    entry = ble_gatt_find(conn_handle, BLE_GATT_OP_NONE, 1, &prev);
    if (entry == NULL) {
        /* Not expecting a response from this device. */
        return;
    }

    dispatch = ble_gatt_dispatch_get(entry->op);
    if (dispatch->err_cb != NULL) {
        dispatch->err_cb(entry, BLE_HS_EATT, rsp->baep_error_code);
    }

    ble_gatt_entry_remove_free(entry, prev);
}

void
ble_gatt_connection_broken(uint16_t conn_handle)
{
    const struct ble_gatt_dispatch_entry *dispatch;
    struct ble_gatt_entry *entry;
    struct ble_gatt_entry *prev;

    while (1) {
        entry = ble_gatt_find(conn_handle, BLE_GATT_OP_NONE, 0, &prev);
        if (entry == NULL) {
            break;
        }

        dispatch = ble_gatt_dispatch_get(entry->op);
        dispatch->err_cb(entry, BLE_HS_ENOTCONN, 0);

        ble_gatt_entry_remove_free(entry, prev);
    }
}

int
ble_gatt_init(void)
{
    int rc;

    free(ble_gatt_entry_mem);

    ble_gatt_entry_mem = malloc(
        OS_MEMPOOL_BYTES(BLE_GATT_NUM_ENTRIES,
                         sizeof (struct ble_gatt_entry)));
    if (ble_gatt_entry_mem == NULL) {
        rc = BLE_HS_ENOMEM;
        goto err;
    }

    rc = os_mempool_init(&ble_gatt_entry_pool,
                         BLE_GATT_NUM_ENTRIES,
                         sizeof (struct ble_gatt_entry),
                         ble_gatt_entry_mem,
                         "ble_gatt_entry_pool");
    if (rc != 0) {
        goto err;
    }

    STAILQ_INIT(&ble_gatt_list);

    return 0;

err:
    free(ble_gatt_entry_mem);
    ble_gatt_entry_mem = NULL;

    return rc;
}
