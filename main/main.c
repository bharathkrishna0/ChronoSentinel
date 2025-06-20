/*
 * SPDX-FileCopyrightText: 2017-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include "esp_log.h"
#include "nvs_flash.h"
/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble_prox_cent.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "freertos/event_groups.h"

static const char *tag = "NimBLE_PROX_HTTP_CENT";

static uint8_t peer_addr[6];
static uint8_t link_supervision_timeout;
static int8_t tx_pwr_lvl;
static struct ble_prox_cent_conn_peer conn_peer[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];
static struct ble_prox_cent_link_lost_peer disconn_peer[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];

/* Note: Path loss is calculated using formula : threshold - RSSI value
 *       by default threshold is kept -128 as per the spec
 *       high_threshold and low_threshold are hardcoded after testing and noting
 *       RSSI values when distance between devices are less and more.
 */
static int8_t high_threshold = -70;
static int8_t low_threshold = -100;

void ble_store_config_init(void);
static void ble_prox_cent_scan(void);
static int ble_prox_cent_gap_event(struct ble_gap_event *event, void *arg);


// +++ NEW: Wi-Fi Connection Logic +++
// =======================================================

// --- WiFi Credentials - CHANGE THESE ---
#define EXAMPLE_ESP_WIFI_SSID      "KITZ INCUBATION"
#define EXAMPLE_ESP_WIFI_PASS      "Kitz123!"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(tag, "Retrying to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(tag,"Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(tag, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(tag, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(tag, "Connected to AP SSID:%s", EXAMPLE_ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(tag, "Failed to connect to SSID:%s", EXAMPLE_ESP_WIFI_SSID);
    } else {
        ESP_LOGE(tag, "UNEXPECTED EVENT");
    }
}

// =======================================================
// +++ NEW: HTTP Function to Send Distance Data +++
// =======================================================

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    // This can be a very simple handler, or you can expand it as in the client example
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(tag, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(tag, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(tag, "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void send_distance_to_server(float distance)
{
    char json_payload[100];
    snprintf(json_payload, sizeof(json_payload),
             "{\"deviceId\": \"NimBLE-Central-01\", \"distance_m\": %.2f}",
             distance);

    esp_http_client_config_t config = {
        // *** IMPORTANT: CHANGE THIS TO YOUR SERVER'S LOCAL IP ADDRESS ***
        .url = "http://192.168.1.107/data", // Example: "http://<SERVER_IP>/<ENDPOINT>"
        .event_handler = http_event_handler,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_payload, strlen(json_payload));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(tag, "HTTP POST Status = %d, sent distance: %.2f m",
                esp_http_client_get_status_code(client), distance);
    } else {
        ESP_LOGE(tag, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}
static int
ble_prox_cent_on_read(uint16_t conn_handle,
                      const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr,
                      void *arg)
{
    MODLOG_DFLT(INFO, "Read on tx power level char completed; status=%d "
                "conn_handle=%d\n",
                error->status, conn_handle);
    if (error->status == 0) {
        MODLOG_DFLT(INFO, " attr_handle=%d value=", attr->handle);
        print_mbuf(attr->om);
        os_mbuf_copydata(attr->om, 0, attr->om->om_len, &tx_pwr_lvl);
        conn_peer[conn_handle].calc_path_loss = true;
    }

    return 0;
}

/**
 * Application callback.  Called when the write of alert level char
 * characteristic has completed.
 */
static int
ble_prox_cent_on_write(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr,
                       void *arg)
{
    MODLOG_DFLT(INFO, "Write alert level char completed; status=%d conn_handle=%d",
                error->status, conn_handle);

    /* Read Tx Power level characteristic. */
    const struct peer_chr *chr;
    int rc;
    const struct peer *peer = peer_find(conn_handle);

    chr = peer_chr_find_uuid(peer,
                             BLE_UUID16_DECLARE(BLE_SVC_TX_POWER_UUID16),
                             BLE_UUID16_DECLARE(BLE_SVC_PROX_CHR_UUID16_TX_PWR_LVL));
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "Error: Peer doesn't support the"
                    "Tx power level characteristic\n");
        goto err;
    }

    rc = ble_gattc_read(conn_handle, chr->chr.val_handle,
                        ble_prox_cent_on_read, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to read characteristic; rc=%d\n",
                    rc);
        goto err;
    }

    return 0;
err:
    /* Terminate the connection. */
    return ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Performs following GATT operations against the specified peer:
 * 1. Writes the alert level characteristic.
 * 2. After write is completed, it reads the Tx Power Level characteristic.
 *
 * If the peer does not support a required service, characteristic, or
 * descriptor, then the peer lied when it claimed support for the link
 * loss service!  When this happens, or if a GATT procedure fails,
 * this function immediately terminates the connection.
 */
static void
ble_prox_cent_read_write_subscribe(const struct peer *peer)
{
    const struct peer_chr *chr;
    int rc;

    /* Storing the val handle of immediate alert characteristic */
    chr = peer_chr_find_uuid(peer,
                             BLE_UUID16_DECLARE(BLE_SVC_IMMEDIATE_ALERT_UUID16),
                             BLE_UUID16_DECLARE(BLE_SVC_PROX_CHR_UUID16_ALERT_LVL));
    if (chr != NULL) {
        conn_peer[peer->conn_handle].val_handle = chr->chr.val_handle;
    } else {
        MODLOG_DFLT(ERROR, "Error: Peer doesn't support the alert level"
                    " characteristic of immediate alert loss service\n");
    }

    /* Write alert level characteristic. */
    chr = peer_chr_find_uuid(peer,
                             BLE_UUID16_DECLARE(BLE_SVC_LINK_LOSS_UUID16),
                             BLE_UUID16_DECLARE(BLE_SVC_PROX_CHR_UUID16_ALERT_LVL));
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "Error: Peer doesn't support the alert level"
                    " characteristic\n");
        goto err;
    }

    rc = ble_gattc_write_flat(peer->conn_handle, chr->chr.val_handle,
                              &link_supervision_timeout, sizeof(link_supervision_timeout),
                              ble_prox_cent_on_write, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to write characteristic; rc=%d\n",
                    rc);
        goto err;
    }

    return;
err:
    /* Terminate the connection. */
    ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Called when service discovery of the specified peer has completed.
 */
static void
ble_prox_cent_on_disc_complete(const struct peer *peer, int status, void *arg)
{

    if (status != 0) {
        /* Service discovery failed.  Terminate the connection. */
        MODLOG_DFLT(ERROR, "Error: Service discovery failed; status=%d "
                    "conn_handle=%d\n", status, peer->conn_handle);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    /* Service discovery has completed successfully.  Now we have a complete
     * list of services, characteristics, and descriptors that the peer
     * supports.
     */
    MODLOG_DFLT(INFO, "Service discovery complete; status=%d "
                "conn_handle=%d\n", status, peer->conn_handle);

    /* Now perform GATT procedures against the peer: read,
     * write.
     */
    ble_prox_cent_read_write_subscribe(peer);
}

/**
 * Initiates the GAP general discovery procedure.
 */
static void
ble_prox_cent_scan(void)
{
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params;
    int rc;

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Tell the controller to filter duplicates; we don't want to process
     * repeated advertisements from the same device.
     */
    disc_params.filter_duplicates = 1;

    /**
     * Perform a passive scan.  I.e., don't send follow-up scan requests to
     * each advertiser.
     */
    disc_params.passive = 1;

    /* Use defaults for the rest of the parameters. */
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params,
                      ble_prox_cent_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error initiating GAP discovery procedure; rc=%d\n",
                    rc);
    }
}

/**
 * Indicates whether we should try to connect to the sender of the specified
 * advertisement.  The function returns a positive result if the device
 * advertises connectability and support for the Health Thermometer service.
 */

#if CONFIG_EXAMPLE_EXTENDED_ADV
static int
ext_ble_prox_cent_should_connect(const struct ble_gap_ext_disc_desc *disc)
{
    int offset = 0;
    int ad_struct_len = 0;

    if (disc->legacy_event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            disc->legacy_event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
        return 0;
    }
    if (strlen(CONFIG_EXAMPLE_PEER_ADDR) && (strncmp(CONFIG_EXAMPLE_PEER_ADDR, "ADDR_ANY", strlen    ("ADDR_ANY")) != 0)) {
        ESP_LOGI(tag, "Peer address from menuconfig: %s", CONFIG_EXAMPLE_PEER_ADDR);
        /* Convert string to address */
        sscanf(CONFIG_EXAMPLE_PEER_ADDR, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &peer_addr[5], &peer_addr[4], &peer_addr[3],
               &peer_addr[2], &peer_addr[1], &peer_addr[0]);
        if (memcmp(peer_addr, disc->addr.val, sizeof(disc->addr.val)) != 0) {
            return 0;
        }
    }

    /* The device has to advertise support for Proximity sensor (link loss)
    * service (0x1803).
    */
    do {
        ad_struct_len = disc->data[offset];

        if (!ad_struct_len) {
            break;
        }

        /* Search if Proximity Sensor (Link loss) UUID is advertised */
        if (disc->data[offset] == 0x03 && disc->data[offset + 1] == 0x03) {
            if ( disc->data[offset + 2] == 0x18 && disc->data[offset + 3] == 0x03 ) {
                return 1;
            }
        }

        offset += ad_struct_len + 1;

    } while ( offset < disc->length_data );

    return 0;
}
#else

static int
ble_prox_cent_should_connect(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    int rc;
    int i;

    /* The device has to be advertising connectability. */
    if (disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {

        return 0;
    }

    rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
    if (rc != 0) {
        return rc;
    }

    if (strlen(CONFIG_EXAMPLE_PEER_ADDR) && (strncmp(CONFIG_EXAMPLE_PEER_ADDR, "ADDR_ANY", strlen("ADDR_ANY")) != 0)) {
        ESP_LOGI(tag, "Peer address from menuconfig: %s", CONFIG_EXAMPLE_PEER_ADDR);
        /* Convert string to address */
        sscanf(CONFIG_EXAMPLE_PEER_ADDR, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &peer_addr[5], &peer_addr[4], &peer_addr[3],
               &peer_addr[2], &peer_addr[1], &peer_addr[0]);
        if (memcmp(peer_addr, disc->addr.val, sizeof(disc->addr.val)) != 0) {
            return 0;
        }
    }

    /* The device has to advertise support for the Proximity sensor (link loss)
     * service (0x1803).
     */
    for (i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == BLE_SVC_LINK_LOSS_UUID16) {
            return 1;
        }
    }

    return 0;
}
#endif

/**
 * Connects to the sender of the specified advertisement of it looks
 * interesting.  A device is "interesting" if it advertises connectability and
 * support for the Proximity Sensor service.
 */
static void
ble_prox_cent_connect_if_interesting(void *disc)
{
    uint8_t own_addr_type;
    int rc;
    ble_addr_t *addr;

    /* Don't do anything if we don't care about this advertiser. */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    if (!ext_ble_prox_cent_should_connect((struct ble_gap_ext_disc_desc *)disc)) {
        return;
    }
#else
    if (!ble_prox_cent_should_connect((struct ble_gap_disc_desc *)disc)) {
        return;
    }
#endif

#if !(MYNEWT_VAL(BLE_HOST_ALLOW_CONNECT_WITH_SCAN))
    /* Scanning must be stopped before a connection can be initiated. */
    rc = ble_gap_disc_cancel();
    if (rc != 0) {
        MODLOG_DFLT(DEBUG, "Failed to cancel scan; rc=%d\n", rc);
        return;
    }
#endif

    /* Figure out address to use for connect (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Try to connect the the advertiser.  Allow 30 seconds (30000 ms) for
     * timeout.
     */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    addr = &((struct ble_gap_ext_disc_desc *)disc)->addr;
#else
    addr = &((struct ble_gap_disc_desc *)disc)->addr;
#endif
    rc = ble_gap_connect(own_addr_type, addr, 30000, NULL,
                         ble_prox_cent_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to connect to device; addr_type=%d "
                    "addr=%s; rc=%d\n",
                    addr->type, addr_str(addr->val), rc);
        return;
    }
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that is
 * established.  ble_prox_cent uses the same callback for all connections.
 *
 * @param event                 The event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                              ble_prox_cent.
 *
 * @return                      0 if the application successfully handled the
 *                              event; nonzero on failure.  The semantics
 *                              of the return code is specific to the
 *                              particular GAP event being signalled.
 */
static int
ble_prox_cent_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    struct ble_hs_adv_fields fields;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                     event->disc.length_data);
        if (rc != 0) {
            return 0;
        }

        /* An advertisement report was received during GAP discovery. */
        print_adv_fields(&fields);

        /* Try to connect to the advertiser if it looks interesting. */
        ble_prox_cent_connect_if_interesting(&event->disc);
        return 0;

    case BLE_GAP_EVENT_LINK_ESTAB:
        /* A new connection was established or a connection attempt failed. */
        if (event->link_estab.status == 0) {
            /* Connection successfully established. */
            MODLOG_DFLT(INFO, "Connection established ");

            rc = ble_gap_conn_find(event->link_estab.conn_handle, &desc);
            assert(rc == 0);
            print_conn_desc(&desc);
            MODLOG_DFLT(INFO, "\n");

            link_supervision_timeout = 8 * desc.conn_itvl;

            /* Remember peer. */
            rc = peer_add(event->link_estab.conn_handle);
            if (rc != 0) {
                MODLOG_DFLT(ERROR, "Failed to add peer; rc=%d\n", rc);
                return 0;
            }

            /* Check if this device is reconnected */
            for (int i = 0; i <= MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
                if (disconn_peer[i].addr != NULL) {
                    if (memcmp(disconn_peer[i].addr, &desc.peer_id_addr.val, BLE_ADDR_LEN)) {
                        /* Peer reconnected. Stop alert for this peer */
                        free(disconn_peer[i].addr);
                        disconn_peer[i].addr = NULL;
                        disconn_peer[i].link_lost = false;
                        break;
                    }
                }
            }

#if CONFIG_EXAMPLE_ENCRYPTION
            /** Initiate security - It will perform
             * Pairing (Exchange keys)
             * Bonding (Store keys)
             * Encryption (Enable encryption)
             * Will invoke event BLE_GAP_EVENT_ENC_CHANGE
             **/
            rc = ble_gap_security_initiate(event->link_estab.conn_handle);
            if (rc != 0) {
                MODLOG_DFLT(INFO, "Security could not be initiated, rc = %d\n", rc);
                return ble_gap_terminate(event->link_estab.conn_handle,
                                         BLE_ERR_REM_USER_CONN_TERM);
            } else {
                MODLOG_DFLT(INFO, "Connection secured\n");
            }
#else
            /* Perform service discovery */
            rc = peer_disc_all(event->link_estab.conn_handle,
                               ble_prox_cent_on_disc_complete, NULL);
            if (rc != 0) {
                MODLOG_DFLT(ERROR, "Failed to discover services; rc=%d\n", rc);
                return 0;
            }
#endif
        } else {
            /* Connection attempt failed; resume scanning. */
            MODLOG_DFLT(ERROR, "Error: Connection failed; status=%d\n",
                        event->link_estab.status);
        }
        ble_prox_cent_scan();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        /* Connection terminated. */
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");

        /* Start the link loss alert for this connection handle */
        for (int i = 0; i <= MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
            if (disconn_peer[i].addr == NULL) {
                disconn_peer[i].addr = (uint8_t *)malloc(BLE_ADDR_LEN * sizeof(uint8_t));
                if (disconn_peer[i].addr == NULL) {
                    return BLE_HS_ENOMEM;
                }
                memcpy(disconn_peer[i].addr, &event->disconnect.conn.peer_id_addr.val,
                       BLE_ADDR_LEN);
                disconn_peer[i].link_lost = true;
                break;
            }
        }
        /* Stop calculating path loss, restart once connection is established again */
        conn_peer[event->disconnect.conn.conn_handle].calc_path_loss = false;
        conn_peer[event->disconnect.conn.conn_handle].val_handle = 0;

        /* Forget about peer. */
        peer_delete(event->disconnect.conn.conn_handle);

        /* Resume scanning. */
        ble_prox_cent_scan();
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        MODLOG_DFLT(INFO, "discovery complete; reason=%d\n",
                    event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Encryption has been enabled or disabled for this connection. */
        MODLOG_DFLT(INFO, "encryption change event; status=%d ",
                    event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        assert(rc == 0);
        print_conn_desc(&desc);
#if CONFIG_EXAMPLE_ENCRYPTION
        /*** Go for service discovery after encryption has been successfully enabled ***/
        rc = peer_disc_all(event->link_estab.conn_handle,
                           ble_prox_cent_on_disc_complete, NULL);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "Failed to discover services; rc=%d\n", rc);
            return 0;
        }
#endif
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Peer sent us a notification or indication. */
        MODLOG_DFLT(INFO, "received %s; conn_handle=%d attr_handle=%d "
                    "attr_len=%d\n",
                    event->notify_rx.indication ?
                    "indication" :
                    "notification",
                    event->notify_rx.conn_handle,
                    event->notify_rx.attr_handle,
                    OS_MBUF_PKTLEN(event->notify_rx.om));

        /* Attribute data is contained in event->notify_rx.om. Use
         * `os_mbuf_copydata` to copy the data received in notification mbuf */
        return 0;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link.  This app sacrifices security for
         * convenience: just throw away the old bond and accept the new link.
         */

        /* Delete the old bond. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);

        /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
         * continue with the pairing operation.
         */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

#if CONFIG_EXAMPLE_EXTENDED_ADV
    case BLE_GAP_EVENT_EXT_DISC:
        /* An advertisement report was received during GAP discovery. */
        ext_print_adv_report(&event->disc);

        ble_prox_cent_connect_if_interesting(&event->disc);
        return 0;
#endif

    default:
        return 0;
    }
}




#include <math.h>

#define RSSI_SAMPLE_COUNT 1  // Only need 1 sample at a time for Kalman

// --- Kalman Filter Structures and Functions ---
typedef struct {
    float estimate;
    float error_covariance;
    float process_noise;
    float measurement_noise;
} KalmanFilter;

void kalman_init(KalmanFilter *kf, float process_noise, float measurement_noise) {
    kf->estimate = 0.0f;
    kf->error_covariance = 1.0f;
    kf->process_noise = process_noise;
    kf->measurement_noise = measurement_noise;
}

float kalman_update(KalmanFilter *kf, float measurement) {
    kf->error_covariance += kf->process_noise;
    float kalman_gain = kf->error_covariance / (kf->error_covariance + kf->measurement_noise);
    kf->estimate += kalman_gain * (measurement - kf->estimate);
    kf->error_covariance *= (1 - kalman_gain);
    return kf->estimate;
}

// --- Distance Estimation Function ---
static float estimate_distance(float rssi) {
    const int measured_power = -75;  // Calibrated RSSI at 1 meter for your setup
    const float path_loss_exponent = 3.0f;  // Suitable for indoor office environment
    float ratio = (measured_power - rssi) / (10.0f * path_loss_exponent);
    float distance = powf(10.0f, ratio);
    if (distance > 5.0f) distance = 5.0f;  // Clamp to 5 meters max
    return distance;
}

// --- Main Path Loss Task Using Kalman Filter ---
void ble_prox_cent_path_loss_task(void *pvParameters) {
    int rc;
    int path_loss;
    static KalmanFilter kf[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];
    static bool kf_initialized[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1] = {false};

    while (1) {
        for (int i = 0; i <= MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
            if (conn_peer[i].calc_path_loss) {

                // Init Kalman for this connection
                if (!kf_initialized[i]) {
                    kalman_init(&kf[i], 0.5f, 2.0f);  // Tune as needed
                    kf_initialized[i] = true;
                }

                int8_t raw_rssi = 0;
                rc = ble_gap_conn_rssi(i, &raw_rssi);
                if (rc != 0) {
                    MODLOG_DFLT(ERROR, "Failed to read RSSI for conn_handle=%d", i);
                    continue;
                }

                float filtered_rssi = kalman_update(&kf[i], raw_rssi);
                float distance = estimate_distance(filtered_rssi);

                int8_t rssi_int = (int8_t)roundf(filtered_rssi);
                path_loss = tx_pwr_lvl - rssi_int;

                MODLOG_DFLT(INFO, "RSSI = %d, Filtered = %.2f, Distance = %.2f m", raw_rssi, filtered_rssi, distance);
                send_distance_to_server(distance);
                if ((conn_peer[i].val_handle != 0) &&
                    (path_loss > high_threshold || path_loss < low_threshold)) {

                    if (path_loss < low_threshold) {
                        path_loss = 0;
                    }

                    rc = ble_gattc_write_no_rsp_flat(i, conn_peer[i].val_handle,
                                                     &path_loss, sizeof(path_loss));
                    if (rc != 0) {
                        MODLOG_DFLT(ERROR, "Error: Failed to write characteristic; rc=%d\n", rc);
                    } else {
                        MODLOG_DFLT(INFO, "Write to alert level characteristic done");
                    }
                }
            }
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);  // Every 1 sec
    }
}
     



void
ble_prox_cent_link_loss_task(void *pvParameters)
{
    while (1) {
        for (int i = 0; i <= MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
            if (disconn_peer[i].link_lost && disconn_peer[i].addr != NULL) {
                MODLOG_DFLT(INFO, "Link lost for device with conn_handle %d", i);
            }
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

static void
ble_prox_cent_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void
ble_prox_cent_on_sync(void)
{
    int rc;

    /* Make sure we have proper identity address set (public preferred) */
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Begin scanning for a peripheral to connect to. */
    ble_prox_cent_scan();
}

void ble_prox_cent_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

static void
ble_prox_cent_init(void)
{
    /* Task for calculating path loss */
    xTaskCreate(ble_prox_cent_path_loss_task, "ble_prox_cent_path_loss_task", 4096, NULL, 10, NULL);

    /* Task for alerting when link is lost */
    xTaskCreate(ble_prox_cent_link_loss_task, "ble_prox_cent_link_loss_task", 4096, NULL, 10, NULL);
    return;
}

void
app_main(void)
{
    int rc;
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if  (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // =======================================================
    // +++ NEW: Connect to Wi-Fi before starting anything else +++
    // =======================================================
    ESP_LOGI(tag, "Connecting to Wi-Fi...");
    wifi_init_sta();
    ESP_LOGI(tag, "Wi-Fi Connected. Initializing BLE...");
    // ===========================================================
    /* Initialize the NimBLE host configuration. */

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Failed to init nimble %d ", ret);
        return;
    }

    /* Initialize a task to keep checking path loss of the link */
    ble_prox_cent_init();

    for (int i = 0; i <= MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
        disconn_peer[i].addr = NULL;
        disconn_peer[i].link_lost = true;
    }

    /* Configure the host. */
    ble_hs_cfg.reset_cb = ble_prox_cent_on_reset;
    ble_hs_cfg.sync_cb = ble_prox_cent_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Initialize data structures to track connected peers. */
    rc = peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 64, 64, 64);
    assert(rc == 0);

    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set("nimble-prox-cent");
    assert(rc == 0);

    /* XXX Need to have template for store */
    ble_store_config_init();

    nimble_port_freertos_init(ble_prox_cent_host_task);
}
