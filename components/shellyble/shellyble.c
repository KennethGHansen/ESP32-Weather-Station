#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

/* AES‑CCM (only used if decrypt_enabled=true) */
#include "mbedtls/ccm.h"
#include "mbedtls/cipher.h"

#include "shellyble.h"

static const char *TAG = "SHELLYBLE";

/* BTHome v2 UUID (little endian in adv service data): D2 FC */
#define BTHOME_UUID_LE_0   0xD2
#define BTHOME_UUID_LE_1   0xFC

/* BTHome encryption layout constants */
#define BTHOME_KEY_LEN     16
#define BTHOME_MIC_LEN     4
#define BTHOME_CNT_LEN     4
#define BTHOME_NONCE_LEN   13   /* MAC(6)+UUID(2)+device_info(1)+counter(4) */

/* Scan params */
#define SCAN_ITVL          0x00A0  /* 100 ms */
#define SCAN_WINDOW        0x0050  /*  50 ms */
#define SCAN_DUR_MS        0x4000  /* ~16 s, then we restart */

/* Internal module state */
static bool s_started = false;
static shellyble_config_t s_cfg;

/* Forward */
static void start_scan(void);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ------------------------- helpers ------------------------- */

static inline bool key_is_all_zero(const uint8_t *k)
{
    for (int i = 0; i < BTHOME_KEY_LEN; i++) {
        if (k[i] != 0) return false;
    }
    return true;
}

/* Decode Shelly BLU H&T plaintext objects.
 * Known IDs:
 *  0x00 packet id (u8) -> skip
 *  0x01 battery  (u8)
 *  0x2E humidity (u8)
 *  0x3A button   (u8)
 *  0x45 temp     (i16, 0.1°C)
 */
static void decode_plain(const uint8_t *plain, size_t len, shellyble_update_t *out)
{
    size_t i = 0;

    while (i < len) {
        uint8_t id = plain[i++];

        switch (id) {

        case 0x00: /* packet id u8 */
            if (i + 1 > len) return;
            i += 1;
            break;

        case 0x01: /* battery u8 */
            if (i + 1 > len) return;
            out->batt_pct = plain[i++];
            out->has_batt = true;
            break;

        case 0x2E: /* humidity u8 */
            if (i + 1 > len) return;
            out->hum_pct = plain[i++];
            out->has_hum = true;
            break;

        case 0x3A: /* button u8 */
            if (i + 1 > len) return;
            out->button_evt = plain[i++];
            out->has_button = true;
            break;

        case 0x45: /* temperature i16, 0.1C */
            if (i + 2 > len) return;
            {
                int16_t raw = (int16_t)(plain[i] | (plain[i + 1] << 8));
                out->temp_c = raw / 10.0f;
                out->has_temp = true;
                i += 2;
            }
            break;

        default:
            /* Unknown length => stop safely */
            return;
        }
    }
}

/* AES‑CCM decrypt for BTHome v2 encrypted payload.
 * svc layout:
 *   [0..1] UUID (D2 FC)
 *   [2]    device_info
 *   [3..]  ciphertext
 *   [..-8..-5] counter (4)
 *   [..-4..-1] mic (4)
 */
static bool decrypt_ccm(const uint8_t *svc, size_t svc_len,
                        const uint8_t mac_be[6],
                        const uint8_t key[16],
                        uint8_t *out_plain, size_t *out_len)
{
    if (svc_len < (2 + 1 + BTHOME_CNT_LEN + BTHOME_MIC_LEN)) {
        return false;
    }

    size_t cipher_len = svc_len - (2 + 1 + BTHOME_CNT_LEN + BTHOME_MIC_LEN);

    const uint8_t *uuid    = &svc[0];
    uint8_t device_info    = svc[2];
    const uint8_t *cipher  = &svc[3];
    const uint8_t *counter = &svc[3 + cipher_len];
    const uint8_t *mic     = &svc[3 + cipher_len + BTHOME_CNT_LEN];

    uint8_t nonce[BTHOME_NONCE_LEN];
    memcpy(&nonce[0], mac_be, 6);
    memcpy(&nonce[6], uuid, 2);
    nonce[8] = device_info;
    memcpy(&nonce[9], counter, 4);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);

    if (mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) {
        mbedtls_ccm_free(&ctx);
        return false;
    }

    int rc = mbedtls_ccm_auth_decrypt(&ctx,
                                     cipher_len,
                                     nonce, sizeof(nonce),
                                     NULL, 0,
                                     cipher, out_plain,
                                     mic, BTHOME_MIC_LEN);

    mbedtls_ccm_free(&ctx);

    if (rc != 0) return false;

    *out_len = cipher_len;
    return true;
}

/* ------------------------- GAP handler ------------------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type == BLE_GAP_EVENT_DISC) {

        const struct ble_gap_disc_desc *desc = &event->disc;

        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
            return 0;
        }

        if (!fields.svc_data_uuid16 || fields.svc_data_uuid16_len < 3) {
            return 0;
        }

        const uint8_t *svc = fields.svc_data_uuid16;
        size_t svc_len = fields.svc_data_uuid16_len;

        /* BTHome UUID check */
        if (svc[0] != BTHOME_UUID_LE_0 || svc[1] != BTHOME_UUID_LE_1) {
            return 0;
        }

        /* MAC in display order */
        uint8_t mac_be[6] = {
            desc->addr.val[5], desc->addr.val[4], desc->addr.val[3],
            desc->addr.val[2], desc->addr.val[1], desc->addr.val[0]
        };

        /* Optional MAC filter */
        if (s_cfg.filter_by_mac && memcmp(mac_be, s_cfg.target_mac, 6) != 0) {
            return 0;
        }

        uint8_t device_info = svc[2];
        bool encrypted = (device_info & 0x01) != 0;

        if (s_cfg.log_alive) {
            ESP_LOGI(TAG, "BTHome alive | %02X:%02X:%02X:%02X:%02X:%02X rssi=%d enc=%d",
                     mac_be[0], mac_be[1], mac_be[2], mac_be[3], mac_be[4], mac_be[5],
                     desc->rssi, encrypted ? 1 : 0);
        }

        shellyble_update_t u = {0};
        memcpy(u.mac, mac_be, 6);
        u.rssi = desc->rssi;
        u.encrypted = encrypted;

        if (!encrypted) {
            const uint8_t *plain = &svc[3];
            size_t plain_len = (svc_len > 3) ? (svc_len - 3) : 0;

            if (s_cfg.log_raw_plaintext && plain_len) {
                printf("SHELLYBLE plain: ");
                for (size_t k = 0; k < plain_len; k++) printf("%02X ", plain[k]);
                printf("\n");
            }

            decode_plain(plain, plain_len, &u);

        } else {
            /* encrypted */
            if (!s_cfg.decrypt_enabled || key_is_all_zero(s_cfg.key)) {
                return 0; /* ignore encrypted if decrypt not enabled */
            }

            uint8_t plain_buf[64];
            size_t plain_len = 0;

            if (!decrypt_ccm(svc, svc_len, mac_be, s_cfg.key, plain_buf, &plain_len)) {
                return 0;
            }

            if (s_cfg.log_raw_plaintext && plain_len) {
                printf("SHELLYBLE dec: ");
                for (size_t k = 0; k < plain_len; k++) printf("%02X ", plain_buf[k]);
                printf("\n");
            }

            decode_plain(plain_buf, plain_len, &u);
        }

        /* Fire callback if any meaningful fields decoded OR button event occurred */
        if (s_cfg.cb) {
            s_cfg.cb(&u, s_cfg.cb_user);
        }

        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        start_scan();
        return 0;
    }

    return 0;
}

/* ------------------------- scan control ------------------------- */

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .passive = 1,
        .itvl = SCAN_ITVL,
        .window = SCAN_WINDOW,
        .filter_duplicates = 1
    };

    (void)ble_gap_disc(BLE_OWN_ADDR_PUBLIC, SCAN_DUR_MS, &params, gap_event_cb, NULL);
}

static void ble_on_sync(void)
{
    start_scan();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------- public API ------------------------- */

esp_err_t shellyble_start(const shellyble_config_t *cfg)
{
#if !CONFIG_BT_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#endif

    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_started = true;

    /* Defaults */
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.log_alive = true;
    s_cfg.log_raw_plaintext = false;
    s_cfg.decrypt_enabled = false;

    if (cfg) {
        s_cfg = *cfg;
    }

    /* NimBLE init: controller + host stack. NVS must already be initialized by the app. [1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/bluetooth/nimble/index.html)[2](https://sourcevu.sysprogs.com/espressif/esp-idf/symbols/nimble_port_init) */
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) {
        s_started = false;
        return rc;
    }

    /* Start NimBLE host thread */
    nimble_port_freertos_init(host_task);

    return ESP_OK;
}