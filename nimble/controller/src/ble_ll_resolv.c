/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "syscfg/syscfg.h"
#include "os/os.h"
#include "nimble/ble.h"
#include "nimble/nimble_opt.h"
#include "controller/ble_ll.h"
#include "controller/ble_ll_resolv.h"
#include "controller/ble_ll_hci.h"
#include "controller/ble_ll_scan.h"
#include "controller/ble_ll_adv.h"
#include "controller/ble_hw.h"
#include "ble_ll_conn_priv.h"

#if (MYNEWT_VAL(BLE_LL_CFG_FEAT_LL_PRIVACY) == 1)
struct ble_ll_resolv_data
{
    uint8_t addr_res_enabled;
    uint8_t rl_size;
    uint8_t rl_cnt;
    uint32_t rpa_tmo;
    struct os_callout_func rpa_timer;
};
struct ble_ll_resolv_data g_ble_ll_resolv_data;

struct ble_ll_resolv_entry g_ble_ll_resolv_list[MYNEWT_VAL(BLE_LL_RESOLV_LIST_SIZE)];

/**
 * Called to determine if a change is allowed to the resolving list at this
 * time. We are not allowed to modify the resolving list if address translation
 * is enabled and we are either scanning, advertising, or attempting to create
 * a connection.
 *
 * @return int 0: not allowed. 1: allowed.
 */
static int
ble_ll_resolv_list_chg_allowed(void)
{
    int rc;

    if (g_ble_ll_resolv_data.addr_res_enabled &&
            (ble_ll_adv_enabled() || ble_ll_scan_enabled() ||
             g_ble_ll_conn_create_sm)) {
        rc = 0;
    } else {
        rc = 1;
    }
    return rc;
}

/**
 * Called when the Resolvable private address timer expires. This timer
 * is used to regenerate local RPA's in the resolving list.
 *
 * @param arg
 */
void
ble_ll_resolv_rpa_timer_cb(void *arg)
{
    int i;
    os_sr_t sr;
    struct ble_ll_resolv_entry *rl;

    rl = &g_ble_ll_resolv_list[0];
    for (i = 0; i < g_ble_ll_resolv_data.rl_cnt; ++i) {
        OS_ENTER_CRITICAL(sr);
        rl->rl_local_rpa_set = 0;
        ble_ll_resolv_gen_priv_addr(rl, 1, rl->rl_local_rpa);
        rl->rl_local_rpa_set = 1;
        OS_EXIT_CRITICAL(sr);
        ++rl;
    }
    os_callout_reset(&g_ble_ll_resolv_data.rpa_timer.cf_c,
                     (int32_t)g_ble_ll_resolv_data.rpa_tmo);
}

/**
 * Called to determine if the IRK is all zero.
 *
 * @param irk
 *
 * @return int 0: IRK is zero . 1: IRK has non-zero value.
 */
int
ble_ll_resolv_irk_nonzero(uint8_t *irk)
{
    int i;
    int rc;

    rc = 0;
    for (i = 0; i < 16; ++i) {
        if (*irk != 0) {
            rc = 1;
            break;
        }
        ++irk;
    }

    return rc;
}

/**
 * Clear the resolving list
 *
 * @return int 0: success, BLE error code otherwise
 */
int
ble_ll_resolv_list_clr(void)
{
    /* Check proper state */
    if (!ble_ll_resolv_list_chg_allowed()) {
        return BLE_ERR_CMD_DISALLOWED;
    }

    /* Sets total on list to 0. Clears HW resolve list */
    g_ble_ll_resolv_data.rl_cnt = 0;
    ble_hw_resolv_list_clear();

    return BLE_ERR_SUCCESS;
}

/**
 * Read the size of the resolving list. This is the total number of resolving
 * list entries allowed by the controller.
 *
 * @param rspbuf Pointer to response buffer
 *
 * @return int 0: success.
 */
int
ble_ll_resolv_list_read_size(uint8_t *rspbuf, uint8_t *rsplen)
{
    rspbuf[0] = g_ble_ll_resolv_data.rl_size;
    *rsplen = 1;
    return BLE_ERR_SUCCESS;
}

/**
 * Used to determine if the device is on the resolving list.
 *
 * @param addr
 * @param addr_type Public address (0) or random address (1)
 *
 * @return int 0: device is not on resolving list; otherwise the return value
 * is the 'position' of the device in the resolving list (the index of the
 * element plus 1).
 */
static int
ble_ll_is_on_resolv_list(uint8_t *addr, uint8_t addr_type)
{
    int i;
    struct ble_ll_resolv_entry *rl;

    rl = &g_ble_ll_resolv_list[0];
    for (i = 0; i < g_ble_ll_resolv_data.rl_cnt; ++i) {
        if ((rl->rl_addr_type == addr_type) &&
            (!memcmp(&rl->rl_identity_addr[0], addr, BLE_DEV_ADDR_LEN))) {
            return i + 1;
        }
        ++rl;
    }

    return 0;
}

/**
 * Used to determine if the device is on the resolving list.
 *
 * @param addr
 * @param addr_type Public address (0) or random address (1)
 *
 * @return Pointer to resolving list entry or NULL if no entry found.
 */
struct ble_ll_resolv_entry *
ble_ll_resolv_list_find(uint8_t *addr, uint8_t addr_type)
{
    int i;
    struct ble_ll_resolv_entry *rl;

    rl = &g_ble_ll_resolv_list[0];
    for (i = 0; i < g_ble_ll_resolv_data.rl_cnt; ++i) {
        if ((rl->rl_addr_type == addr_type) &&
            (!memcmp(&rl->rl_identity_addr[0], addr, BLE_DEV_ADDR_LEN))) {
            return rl;
        }
        ++rl;
    }

    return NULL;
}

/**
 * Add a device to the resolving list
 *
 * @return int
 */
int
ble_ll_resolv_list_add(uint8_t *cmdbuf)
{
    int rc;
    uint8_t addr_type;
    uint8_t *ident_addr;
    struct ble_ll_resolv_entry *rl;

    /* Must be in proper state */
    if (!ble_ll_resolv_list_chg_allowed()) {
        return BLE_ERR_CMD_DISALLOWED;
    }

    /* Check if we have any open entries */
    if (g_ble_ll_resolv_data.rl_cnt >= g_ble_ll_resolv_data.rl_size) {
        return BLE_ERR_MEM_CAPACITY;
    }

    addr_type = cmdbuf[0];
    ident_addr = cmdbuf + 1;

    rc = BLE_ERR_SUCCESS;
    if (!ble_ll_is_on_resolv_list(ident_addr, addr_type)) {
        rl = &g_ble_ll_resolv_list[g_ble_ll_resolv_data.rl_cnt];
        rl->rl_addr_type = addr_type;
        rl->rl_local_rpa_set = 0;
        memcpy(&rl->rl_identity_addr[0], ident_addr, BLE_DEV_ADDR_LEN);
        swap_buf(rl->rl_peer_irk, cmdbuf + 7, 16);
        swap_buf(rl->rl_local_irk, cmdbuf + 23, 16);

        /*
         * Add peer IRK to HW resolving list. If we can add it, also
         * generate a local RPA now to save time later.
         */
        rc = ble_hw_resolv_list_add(rl->rl_peer_irk);
        if (!rc) {
            ble_ll_resolv_gen_priv_addr(rl, 1, rl->rl_local_rpa);
            rl->rl_local_rpa_set = 1;
        }
        ++g_ble_ll_resolv_data.rl_cnt;
    }

    return rc;
}

/**
 * Remove a device from the resolving list
 *
 * @param cmdbuf
 *
 * @return int 0: success, BLE error code otherwise
 */
int
ble_ll_resolv_list_rmv(uint8_t *cmdbuf)
{
    int position;
    uint8_t addr_type;
    uint8_t *ident_addr;

    /* Must be in proper state */
    if (!ble_ll_resolv_list_chg_allowed()) {
        return BLE_ERR_CMD_DISALLOWED;
    }

    addr_type = cmdbuf[0];
    ident_addr = cmdbuf + 1;

    /* Remove from IRK records */
    position = ble_ll_is_on_resolv_list(ident_addr, addr_type);
    if (position && (position < g_ble_ll_resolv_data.rl_cnt)) {
        memmove(&g_ble_ll_resolv_list[position - 1],
                &g_ble_ll_resolv_list[position],
                g_ble_ll_resolv_data.rl_cnt - position);
        --g_ble_ll_resolv_data.rl_cnt;

        /* Remove from HW list */
        ble_hw_resolv_list_rmv(position - 1);
    }

    return BLE_ERR_SUCCESS;
}

/**
 * Called to enable or disable address resolution in the controller
 *
 * @param cmdbuf
 *
 * @return int
 */
int
ble_ll_resolv_enable_cmd(uint8_t *cmdbuf)
{
    int rc;
    int32_t tmo;
    uint8_t enabled;

    if (ble_ll_adv_enabled() || ble_ll_scan_enabled() ||
        g_ble_ll_conn_create_sm) {
        rc = BLE_ERR_CMD_DISALLOWED;
    } else {
        enabled = cmdbuf[0];
        if (enabled <= 1) {
            /* If we change state, we need to disable/enable the RPA timer */
            if ((enabled ^ g_ble_ll_resolv_data.addr_res_enabled) != 0) {
                if (enabled) {
                    tmo = (int32_t)g_ble_ll_resolv_data.rpa_tmo;
                    os_callout_reset(&g_ble_ll_resolv_data.rpa_timer.cf_c, tmo);
                } else {
                    os_callout_stop(&g_ble_ll_resolv_data.rpa_timer.cf_c);
                }
                g_ble_ll_resolv_data.addr_res_enabled = enabled;
            }
            rc = BLE_ERR_SUCCESS;
        } else {
            rc = BLE_ERR_INV_HCI_CMD_PARMS;
        }
    }

    return rc;
}

int
ble_ll_resolv_peer_addr_rd(uint8_t *cmdbuf)
{
    /* XXX */
    return 0;
}

void
ble_ll_resolv_local_addr_rd(uint8_t *cmdbuf)
{
}

/**
 * Set the resolvable private address timeout.
 *
 * @param cmdbuf
 *
 * @return int
 */
int
ble_ll_resolv_set_rpa_tmo(uint8_t *cmdbuf)
{
    int rc;
    uint16_t tmo_secs;

    tmo_secs = le16toh(cmdbuf);
    if ((tmo_secs > 0) && (tmo_secs <= 0xA1B8)) {
        g_ble_ll_resolv_data.rpa_tmo = tmo_secs * OS_TICKS_PER_SEC;
        if (g_ble_ll_resolv_data.addr_res_enabled) {
            os_callout_reset(&g_ble_ll_resolv_data.rpa_timer.cf_c,
                             (int32_t)g_ble_ll_resolv_data.rpa_tmo);
        }
    } else {
        rc = BLE_ERR_INV_HCI_CMD_PARMS;
    }

    return rc;
}

/**
 * Returns the Resolvable Private address timeout, in os ticks
 *
 *
 * @return uint32_t
 */
uint32_t
ble_ll_resolv_get_rpa_tmo(void)
{
    return g_ble_ll_resolv_data.rpa_tmo;
}

/**
 * Called to generate a resolvable private address
 *
 * @param rl
 * @param local
 * @param addr Pointer to resolvable private address
 */
void
ble_ll_resolv_gen_priv_addr(struct ble_ll_resolv_entry *rl, int local,
                            uint8_t *addr)
{
    uint8_t *irk;
    uint8_t *prand;
    uint32_t *irk32;
    uint32_t *key32;
    uint32_t *pt32;
    struct ble_encryption_block ecb;

    assert(rl != NULL);
    assert(addr != NULL);

    /* If the local rpa has already been generated, just copy it */
    if (local && rl->rl_local_rpa_set) {
        memcpy(addr, rl->rl_local_rpa, BLE_DEV_ADDR_LEN);
        return;
    }

    /* Get prand */
    prand = addr + 3;
    ble_ll_rand_prand_get(prand);

    /* Calculate hash, hash = ah(local IRK, prand) */
    if (local) {
        irk = rl->rl_local_irk;
    } else {
        irk = rl->rl_peer_irk;
    }

    irk32 = (uint32_t *)irk;
    key32 = (uint32_t *)&ecb.key[0];
    key32[0] = irk32[0];
    key32[1] = irk32[1];
    key32[2] = irk32[2];
    key32[3] = irk32[3];
    pt32 = (uint32_t *)&ecb.plain_text[0];
    pt32[0] = 0;
    pt32[1] = 0;
    pt32[2] = 0;
    ecb.plain_text[12] = 0;
    ecb.plain_text[13] = prand[2];
    ecb.plain_text[14] = prand[1];
    ecb.plain_text[15] = prand[0];

    /* Calculate hash */
    ble_hw_encrypt_block(&ecb);

    addr[0] = ecb.cipher_text[15];
    addr[1] = ecb.cipher_text[14];
    addr[2] = ecb.cipher_text[13];
}

/**
 * Generate a resolvable private address.
 *
 * @param addr
 * @param addr_type
 * @param rpa
 *
 * @return int
 */
int
ble_ll_resolv_gen_rpa(uint8_t *addr, uint8_t addr_type, uint8_t *rpa, int local)
{
    int rc;
    uint8_t *irk;
    struct ble_ll_resolv_entry *rl;

    rc = 0;
    rl = ble_ll_resolv_list_find(addr, addr_type);
    if (rl) {
        if (local) {
            irk = rl->rl_local_irk;
        } else {
            irk = rl->rl_peer_irk;
        }
        if (ble_ll_resolv_irk_nonzero(irk)) {
            ble_ll_resolv_gen_priv_addr(rl, local, rpa);
            rc = 1;
        }
    }

    return rc;
}

/**
 * Resolve a Resolvable Private Address
 *
 * @param rpa
 * @param index
 *
 * @return int
 */
int
ble_ll_resolv_rpa(uint8_t *rpa, uint8_t *irk)
{
    int rc;
    uint32_t *irk32;
    uint32_t *key32;
    uint32_t *pt32;
    struct ble_encryption_block ecb;

    irk32 = (uint32_t *)irk;
    key32 = (uint32_t *)&ecb.key[0];

    key32[0] = irk32[0];
    key32[1] = irk32[1];
    key32[2] = irk32[2];
    key32[3] = irk32[3];

    pt32 = (uint32_t *)&ecb.plain_text[0];
    pt32[0] = 0;
    pt32[1] = 0;
    pt32[2] = 0;
    pt32[3] = 0;

    ecb.plain_text[15] = rpa[3];
    ecb.plain_text[14] = rpa[4];
    ecb.plain_text[13] = rpa[5];

    ble_hw_encrypt_block(&ecb);
    if ((ecb.cipher_text[15] == rpa[0]) && (ecb.cipher_text[14] == rpa[1]) &&
        (ecb.cipher_text[13] == rpa[2])) {
        rc = 1;
    } else {
        rc = 0;
    }

    return rc;
}

/**
 * Returns whether or not address resolution is enabled.
 *
 * @return uint8_t
 */
uint8_t
ble_ll_resolv_enabled(void)
{
    return g_ble_ll_resolv_data.addr_res_enabled;
}

/**
 * Called to reset private address resolution module.
 */
void
ble_ll_resolv_list_reset(void)
{
    g_ble_ll_resolv_data.addr_res_enabled = 0;
    os_callout_stop(&g_ble_ll_resolv_data.rpa_timer.cf_c);
    ble_ll_resolv_list_clr();
    ble_ll_resolv_init();
}

void
ble_ll_resolv_init(void)
{
    uint8_t hw_size;

    /* Default is 15 minutes */
    g_ble_ll_resolv_data.rpa_tmo = 15 * 60 * OS_TICKS_PER_SEC;

    hw_size = ble_hw_resolv_list_size();
    if (hw_size > MYNEWT_VAL(BLE_LL_RESOLV_LIST_SIZE)) {
        hw_size = MYNEWT_VAL(BLE_LL_RESOLV_LIST_SIZE);
    }
    g_ble_ll_resolv_data.rl_size = hw_size;

    os_callout_func_init(&g_ble_ll_resolv_data.rpa_timer,
                         &g_ble_ll_data.ll_evq,
                         ble_ll_resolv_rpa_timer_cb,
                         NULL);
}

#endif  /* if MYNEWT_VAL(BLE_LL_CFG_FEAT_LL_PRIVACY) == 1 */

