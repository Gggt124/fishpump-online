#include "tb_ota.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dry_run_guard.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_store.h"
#include "ota_lock.h"
#include "psa/crypto.h"
#include "session.h"
#include "tb_link_logic.h"
#include "uplink.h"

static const char *TAG = "tb_ota";

#define IDX_NONE UINT32_MAX /* the task is not waiting for a chunk */

typedef enum { NOTE_OK = 1, NOTE_BAD_CHUNK, NOTE_SERVER_ERROR } tb_ota_note_t;

static atomic_bool s_busy = false;
static atomic_uint_least32_t s_wait_req = 0;
static atomic_uint_least32_t s_wait_idx = IDX_NONE;
static TaskHandle_t s_task = NULL; /* written by the task before it waits, read by the hook */
static uint8_t *s_buf = NULL;      /* chunk buffer; the hook writes it only while s_wait_idx matches */
static tb_fw_info_t s_job;         /* the update in progress; written by the hook before the task exists */
static bool s_rx_active = false;   /* hook only: a matching chunk is being assembled */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_failed_version[TB_FW_VERSION_MAX]; /* final failures, until reboot; guarded by s_mux */

bool tb_ota_busy(void) { return atomic_load(&s_busy); }

static void remember_failed(const char *version) {
  portENTER_CRITICAL(&s_mux);
  strncpy(s_failed_version, version, sizeof s_failed_version - 1);
  s_failed_version[sizeof s_failed_version - 1] = '\0';
  portEXIT_CRITICAL(&s_mux);
}

/* Publishes fw_state (and fw_error); with_current adds the running title and version. A report while
 * disconnected is lost; the next connect re-reads the state. */
static void report_state(const char *state, const char *error, bool with_current) {
  if (error) {
    ESP_LOGW(TAG, "state %s, error %s", state, error);
  } else {
    ESP_LOGI(TAG, "state %s", state);
  }
  char json[192];
  int n = tb_fw_state_json(json, sizeof json, state, error, with_current ? TB_FW_TITLE : NULL,
                           with_current ? esp_app_get_description()->version : NULL);
  if (n > 0) {
    tb_link_publish(TB_TELEMETRY_TOPIC, json, n, 1);
  }
}

/* ---- the update task ---- */

/* Waits for chunk idx of request req. Returns NULL when s_buf holds the complete chunk, else the fw_error code. */
static const char *fetch_chunk(uint32_t req, uint32_t idx) {
  char req_topic[64];
  if (tb_fw_request_topic(req, idx, req_topic, sizeof req_topic) < 0) {
    return "bad_chunk";
  }
  portENTER_CRITICAL(&s_mux);
  atomic_store(&s_wait_req, req);
  atomic_store(&s_wait_idx, idx);
  portEXIT_CRITICAL(&s_mux);
  uint32_t offline_ms = 0;
  const uint32_t offline_max = TB_FW_CHUNK_TIMEOUT_MS * TB_FW_CHUNK_TRIES;
  int attempts = 0;
  while (attempts < TB_FW_CHUNK_TRIES) {
    uint32_t note = 0;
    bool sent = tb_link_is_connected() &&
                tb_link_publish(req_topic, TB_FW_CHUNK_SIZE_STR, (int)strlen(TB_FW_CHUNK_SIZE_STR), 0) >= 0;
    if (!sent) {
      /* Disconnected: wait without spending an attempt, up to the total budget. */
      if (offline_ms >= offline_max) {
        return "chunk_timeout";
      }
      if (xTaskNotifyWait(0, ULONG_MAX, &note, pdMS_TO_TICKS(500)) != pdTRUE) {
        offline_ms += 500;
        continue;
      }
    } else if (xTaskNotifyWait(0, ULONG_MAX, &note, pdMS_TO_TICKS(TB_FW_CHUNK_TIMEOUT_MS)) != pdTRUE) {
      attempts++;
      /* The same request is published again; a late reply to an earlier one is still accepted. */
      continue;
    }
    switch ((tb_ota_note_t)note) {
      case NOTE_OK:
        return NULL;
      case NOTE_BAD_CHUNK:
        return "bad_chunk";
      case NOTE_SERVER_ERROR:
        return "server_error";
      default:
        break;
    }
  }
  return "chunk_timeout";
}

static void tb_ota_task(void *arg) {
  (void)arg;
  const char *code = NULL;
  bool begun = false, hash_open = false;
  esp_ota_handle_t handle = 0;
  uint8_t digest[32];
  size_t digest_len = 0;
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  uint8_t *buf = s_buf;
  const uint32_t size = s_job.size;
  const uint32_t count = tb_fw_chunk_count(size);
  static uint32_t s_req_counter = 0;
  uint32_t req = ++s_req_counter;
  int logged_pct = 0;

  s_task = xTaskGetCurrentTaskHandle();
  report_state("DOWNLOADING", NULL, true);

  if (next == NULL || esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
    code = "ota_begin_failed";
    goto fail;
  }
  begun = true;
  if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
    code = "no_memory";
    goto fail;
  }
  hash_open = true;

  for (uint32_t idx = 0; idx < count; idx++) {
    code = fetch_chunk(req, idx);
    if (code) {
      goto fail;
    }
    uint32_t len = tb_fw_chunk_len(size, idx);
    if (psa_hash_update(&op, buf, len) != PSA_SUCCESS || esp_ota_write(handle, buf, len) != ESP_OK) {
      code = "write_failed";
      goto fail;
    }
    int pct = (int)(((uint64_t)(idx + 1) * 100u) / count);
    if (pct / 25 > logged_pct / 25) {
      logged_pct = pct;
      ESP_LOGI(TAG, "downloaded %d%% (%u of %u chunks)", pct / 25 * 25, (unsigned)(idx + 1), (unsigned)count);
    }
  }

  if (psa_hash_finish(&op, digest, sizeof digest, &digest_len) != PSA_SUCCESS) {
    code = "checksum_mismatch";
    goto fail;
  }
  hash_open = false;
  if (digest_len != sizeof digest || !tb_fw_digest_matches(digest, s_job.checksum)) {
    code = "checksum_mismatch";
    goto fail;
  }
  report_state("DOWNLOADED", NULL, false);

  begun = false; /* esp_ota_end releases the handle whether or not it succeeds */
  if (esp_ota_end(handle) != ESP_OK) {
    code = "invalid_image";
    goto fail;
  }
  report_state("VERIFIED", NULL, false);

  bool logged = false;
  for (;;) {
    dry_run_measure_status_t ms = {0};
    bool measuring = dry_run_guard_get_measure(&ms) && ms.state == DRY_RUN_MEASURE_RUNNING;
    if (!measuring) {
      break;
    }
    if (!logged) {
      ESP_LOGI(TAG, "dry-run measurement running; delaying the reboot");
      logged = true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  if (!nvs_store_set_tb_ota_pending(s_job.version)) {
    code = "set_boot_failed";
    goto fail;
  }
  if (esp_ota_set_boot_partition(next) != ESP_OK) {
    nvs_store_clear_tb_ota_pending();
    code = "set_boot_failed";
    goto fail;
  }
  report_state("UPDATING", NULL, false);
  uplink_note_settings_changed();
  nvs_store_set_restart_detail("TB_OTA_REBOOT");
  session_invalidate_all();
  /* Success: the OTA lock and the busy flag stay set until the reboot, so neither the web upload nor a second
   * ThingsBoard update can touch the slot that was just selected. */
  vTaskDelay(pdMS_TO_TICKS(1500)); /* let the queued telemetry out */
  esp_restart();

fail:
  if (hash_open) {
    psa_hash_abort(&op);
  }
  if (begun) {
    esp_ota_abort(handle);
  }
  report_state("FAILED", code, false);
  if (tb_fw_error_is_final(code)) {
    remember_failed(s_job.version);
  }
  portENTER_CRITICAL(&s_mux);
  atomic_store(&s_wait_idx, IDX_NONE);
  s_buf = NULL; /* the hook no longer writes into the buffer */
  portEXIT_CRITICAL(&s_mux);
  heap_caps_free(buf);
  s_task = NULL;
  atomic_store(&s_busy, false);
  ota_lock_release();
  vTaskDelete(NULL);
}

/* ---- the data hook (esp-mqtt task, must not block) ---- */

static void on_attributes(const char *data, int len) {
  tb_fw_info_t fw;
  memset(&fw, 0, sizeof fw);
  tb_fw_attr_t kind = tb_fw_parse_attrs(data, (size_t)len, &fw);
  if (kind == TB_FW_ATTR_INVALID) {
    if (strcmp(fw.title, TB_FW_TITLE) == 0) {
      report_state("FAILED", "bad_attributes", false);
    }
    return;
  }
  if (kind != TB_FW_ATTR_INFO) {
    return;
  }
  char failed[TB_FW_VERSION_MAX];
  portENTER_CRITICAL(&s_mux);
  memcpy(failed, s_failed_version, sizeof failed);
  portEXIT_CRITICAL(&s_mux);
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  const char *reason = NULL;
  tb_fw_decision_t dec = tb_fw_decide(&fw, esp_app_get_description()->version, failed, next ? next->size : 0,
                                      tb_ota_busy(), &reason);
  if (dec == TB_FW_FAIL) {
    report_state("FAILED", reason, false);
    remember_failed(fw.version);
    return;
  }
  if (dec != TB_FW_START) {
    return;
  }
  ESP_LOGI(TAG, "update offered: %s %s, %u bytes", fw.title, fw.version, (unsigned)fw.size);
  if (!ota_lock_claim()) {
    report_state("FAILED", "busy", false);
    return;
  }
  uint8_t *buf = heap_caps_malloc(TB_FW_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == NULL) {
    ota_lock_release();
    report_state("FAILED", "no_memory", false);
    return;
  }
  s_job = fw;
  atomic_store(&s_wait_idx, IDX_NONE);
  s_rx_active = false;
  s_buf = buf;
  atomic_store(&s_busy, true);
  if (xTaskCreate(tb_ota_task, "tb_ota", 6144, NULL, 2, NULL) != pdPASS) {
    s_buf = NULL;
    atomic_store(&s_busy, false);
    heap_caps_free(buf);
    ota_lock_release();
    report_state("FAILED", "no_memory", false);
  }
}

static void on_chunk(const tb_link_data_t *d, uint32_t req, uint32_t idx) {
  if (d->offset == 0) {
    s_rx_active = false;
  }
  if (atomic_load(&s_wait_req) != req || atomic_load(&s_wait_idx) != idx) {
    return;
  }
  const uint32_t want = tb_fw_chunk_len(s_job.size, idx);
  if (d->offset == 0) {
    if ((uint32_t)d->total != want) {
      bool wake = false;
      portENTER_CRITICAL(&s_mux);
      if (atomic_load(&s_wait_req) == req && atomic_load(&s_wait_idx) == idx) {
        atomic_store(&s_wait_idx, IDX_NONE);
        wake = true;
      }
      portEXIT_CRITICAL(&s_mux);
      if (wake) {
        xTaskNotify(s_task, NOTE_BAD_CHUNK, eSetValueWithOverwrite);
      }
      return;
    }
    s_rx_active = true;
  }
  if (!s_rx_active || d->offset < 0 || d->data_len < 0 || (uint32_t)d->total != want ||
      (uint32_t)d->offset + (uint32_t)d->data_len > want) {
    return;
  }
  bool done = false, copied = false;
  portENTER_CRITICAL(&s_mux);
  if (s_buf != NULL && atomic_load(&s_wait_req) == req && atomic_load(&s_wait_idx) == idx) {
    memcpy(s_buf + d->offset, d->data, (size_t)d->data_len);
    copied = true;
    if ((uint32_t)d->offset + (uint32_t)d->data_len == want) {
      /* Invalidate the awaited index first: a duplicate reply (from a retry) must not overwrite the buffer
       * while the task reads it. */
      atomic_store(&s_wait_idx, IDX_NONE);
      done = true;
    }
  }
  portEXIT_CRITICAL(&s_mux);
  if (copied && done) {
    s_rx_active = false;
    xTaskNotify(s_task, NOTE_OK, eSetValueWithOverwrite);
  }
}

void tb_ota_on_data(const tb_link_data_t *d) {
  if (d == NULL || d->topic == NULL) {
    return;
  }
  const size_t tl = (size_t)d->topic_len;
  const size_t attr_len = strlen(TB_FW_ATTR_TOPIC);
  const size_t resp_len = strlen(TB_FW_ATTR_RESP_PREFIX);
  if ((tl == attr_len && memcmp(d->topic, TB_FW_ATTR_TOPIC, attr_len) == 0) ||
      (tl > resp_len && memcmp(d->topic, TB_FW_ATTR_RESP_PREFIX, resp_len) == 0)) {
    if (d->offset == 0 && d->data_len == d->total) {
      on_attributes(d->data, d->data_len);
    }
    return;
  }
  if (!tb_ota_busy()) {
    return;
  }
  if (tl == strlen(TB_FW_ERROR_TOPIC) && memcmp(d->topic, TB_FW_ERROR_TOPIC, tl) == 0) {
    bool wake = false;
    portENTER_CRITICAL(&s_mux);
    if (atomic_load(&s_wait_idx) != IDX_NONE) {
      atomic_store(&s_wait_idx, IDX_NONE);
      wake = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (wake && s_task) {
      xTaskNotify(s_task, NOTE_SERVER_ERROR, eSetValueWithOverwrite);
    }
    return;
  }
  uint32_t req = 0, idx = 0;
  if (tb_fw_parse_chunk_topic(d->topic, tl, &req, &idx) && s_task != NULL) {
    on_chunk(d, req, idx);
  }
}
