#include "ble.h"

#include <stdio.h>
#include <string.h>

#include "channel.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// 6e7a000X-9a3b-4c2d-8f00-6d6963726f77 (байты в обратном порядке)
#define MW_UUID(x)                                                                          \
  BLE_UUID128_INIT(0x77, 0x6f, 0x72, 0x63, 0x69, 0x6d, 0x00, 0x8f, 0x2d, 0x4c, 0x3b, 0x9a, \
                   x, 0x00, 0x7a, 0x6e)

static const ble_uuid128_t SVC_UUID = MW_UUID(0x01);
static const ble_uuid128_t RX_UUID = MW_UUID(0x02);
static const ble_uuid128_t TX_UUID = MW_UUID(0x03);

static uint16_t tx_handle;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t own_addr_type;
static volatile bool synced, enabled = true, slow;
static char name[20];

// Сборка кадров из пакетов: [длина 2 байта BE][кадр]
static uint8_t rx[2 + CHAN_FRAME_MAX];
static size_t rx_len;

static void advertise(void);

static int rx_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                     void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
  uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
  if (rx_len + n > sizeof rx) {  // мусор или атака — начинаем заново
    rx_len = 0;
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  uint16_t got;
  if (ble_hs_mbuf_to_flat(ctxt->om, rx + rx_len, n, &got) != 0) return BLE_ATT_ERR_UNLIKELY;
  rx_len += got;
  while (rx_len >= 2) {
    size_t len = (size_t)rx[0] << 8 | rx[1];
    if (len > CHAN_FRAME_MAX) {
      rx_len = 0;
      break;
    }
    if (rx_len < 2 + len) break;
    mw_post(MW_SRC_BLE, rx + 2, len);
    memmove(rx, rx + 2 + len, rx_len - 2 - len);
    rx_len -= 2 + len;
  }
  return 0;
}

static int tx_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                     void *arg) {
  return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &RX_UUID.u,
                 .access_cb = rx_access,
                 .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = &TX_UUID.u,
                 .access_cb = tx_access,
                 .val_handle = &tx_handle,
                 .flags = BLE_GATT_CHR_F_NOTIFY},
                {0},
            },
    },
    {0},
};

static int gap_event(struct ble_gap_event *ev, void *arg) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) {
        conn = ev->connect.conn_handle;
        rx_len = 0;
      } else {
        advertise();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      conn = BLE_HS_CONN_HANDLE_NONE;
      rx_len = 0;
      mw_post(MW_SRC_BLE_RESET, NULL, 0);
      advertise();
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      break;
  }
  return 0;
}

static void advertise(void) {
  if (!synced || !enabled || conn != BLE_HS_CONN_HANDLE_NONE) return;
  struct ble_hs_adv_fields f = {0}, rsp = {0};
  f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  f.uuids128 = (ble_uuid128_t *)&SVC_UUID;
  f.num_uuids128 = 1;
  f.uuids128_is_complete = 1;
  ble_gap_adv_set_fields(&f);
  // Имя не помещается рядом со 128-битным UUID — отдаём его в ответе на сканирование
  rsp.name = (uint8_t *)name;
  rsp.name_len = strlen(name);
  rsp.name_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&rsp);
  // Обычно ~100 мс, во сне ~1 с: находится чуть дольше, но тратит в разы меньше
  struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND,
                                 .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                 .itvl_min = slow ? 1600 : 160,   // x 0.625 мс
                                 .itvl_max = slow ? 1760 : 240};
  ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &own_addr_type);
  synced = true;
  advertise();
}

static void on_reset(int reason) { synced = false; }

static void host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void ble_init(void) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(name, sizeof name, "MicroWallet-%02X%02X", mac[4], mac[5]);
  if (nimble_port_init() != ESP_OK) return;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_gatts_count_cfg(svcs);
  ble_gatts_add_svcs(svcs);
  ble_svc_gap_device_name_set(name);
  nimble_port_freertos_init(host_task);
}

bool ble_running(void) { return synced; }
bool ble_enabled(void) { return enabled; }
bool ble_connected(void) { return conn != BLE_HS_CONN_HANDLE_NONE; }

void ble_set_enabled(bool on) {
  enabled = on;
  if (!synced) return;
  if (on) {
    advertise();
  } else {
    ble_gap_adv_stop();
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
  }
}

void ble_set_slow(bool on) {
  if (slow == on) return;
  slow = on;
  if (synced && enabled && conn == BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_adv_stop();
    advertise();
  }
}

// Кадр -> уведомления по (MTU - 3) байт. Если буферы стека заняты — ждём.
void ble_send_frame(const uint8_t *frame, size_t len) {
  static uint8_t out[2 + CHAN_FRAME_MAX];
  uint16_t c = conn;
  if (c == BLE_HS_CONN_HANDLE_NONE || len > CHAN_FRAME_MAX) return;
  out[0] = (uint8_t)(len >> 8);
  out[1] = (uint8_t)len;
  memcpy(out + 2, frame, len);
  size_t total = len + 2, chunk = ble_att_mtu(c) - 3;
  if (chunk < 20) chunk = 20;
  for (size_t off = 0; off < total;) {
    size_t n = total - off < chunk ? total - off : chunk;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(out + off, n);
    int rc = om ? ble_gatts_notify_custom(c, tx_handle, om) : BLE_HS_ENOMEM;
    if (rc == 0) {
      off += n;
    } else if (rc == BLE_HS_ENOMEM && conn == c) {
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      break;  // соединение пропало
    }
  }
  memset(out, 0, total);
}
