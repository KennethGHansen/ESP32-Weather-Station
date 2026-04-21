#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded update from the Shelly BLU H&T advertisement */
typedef struct {
    uint8_t mac[6];          /* MAC in human order: AA:BB:CC:DD:EE:FF */
    int8_t  rssi;
    bool    encrypted;       /* BTHome device_info bit0 */

    bool    has_temp;
    bool    has_hum;
    bool    has_batt;
    bool    has_button;

    float   temp_c;          /* 0.1°C decoded to float */
    uint8_t hum_pct;         /* % */
    uint8_t batt_pct;        /* % */
    uint8_t button_evt;      /* 0x3A value when present (e.g. 1 = single press) */
} shellyble_update_t;

/* Callback fired whenever we successfully decode a packet */
typedef void (*shellyble_update_cb_t)(const shellyble_update_t *u, void *user);

/* Optional configuration */
typedef struct {
    /* If true, only accept packets from target_mac */
    bool filter_by_mac;
    uint8_t target_mac[6];

    /* Reduce logs */
    bool log_raw_plaintext;   /* prints payload bytes */
    bool log_alive;           /* prints “alive” line */

    /* Encryption support (optional; default off) */
    bool decrypt_enabled;
    uint8_t key[16];          /* 16-byte AES-CCM key */

    /* Callback hook (optional) */
    shellyble_update_cb_t cb;
    void *cb_user;
} shellyble_config_t;

/* Start the Shelly BLE scanner (call once). If cfg==NULL, defaults are used. */
esp_err_t shellyble_start(const shellyble_config_t *cfg);

#ifdef __cplusplus
}
#endif