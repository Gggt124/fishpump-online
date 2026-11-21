#include "tb_ota.h"

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dry_run_guard.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
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
static bool report_state(const char *state, const char *error, bool with_current) {
  if (state == NULL) {
    ESP_LOGI(TAG, "reporting the current firmware title and version");
  } else if (error) {
    ESP_LOGW(TAG, "state %s, error %s", state, error);
  } else {
    ESP_LOGI(TAG, "state %s", state);
  }
  char json[192];
  int n = tb_fw_state_json(json, sizeof json, state, error, with_current ? TB_FW_TITLE : NULL,
                           with_current ? esp_app_get_description()->version : NULL);
  return n > 0 && tb_link_publish(TB_TELEMETRY_TOPIC, json, n, 1) >= 0;
}

/* ---- the update task ---- */

/* Ends the wait for chunk idx after the attempts ran out. Under s_mux: if the wait is still armed the hook can no
 * longer notify (it checks the same state under the same lock), so the timeout stands. Otherwise the hook already
 * committed to a notify (it sends it inside the critical section), so that note is collected. */
static const char *end_wait_timeout(uint32_t idx) {
  bool armed = false;
  portENTER_CRITICAL(&s_mux);
  if (atomic_load(&s_wait_idx) == idx) {
    atomic_store(&s_wait_idx, IDX_NONE);
    armed = true;
  }
  portEXIT_CRITICAL(&s_mux);
  if (armed) {
    return "chunk_timeout";
  }
  uint32_t note = 0;
  if (xTaskNotifyWait(0, ULONG_MAX, &note, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (note == NOTE_OK) {
      return NULL;
    }
    return note == NOTE_SERVER_ERROR ? "server_error" : "bad_chunk";
  }
  return "chunk_timeout";
}

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
        return end_wait_timeout(idx);
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
  return end_wait_timeout(idx);
}

static void tb_ota_task(void *arg) {
  (void)arg;
  const char *code = NULL;
  bool begun = false;
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

  portENTER_CRITICAL(&s_mux);
  s_task = xTaskGetCurrentTaskHandle(); /* valid before the first request is armed */
  portEXIT_CRITICAL(&s_mux);
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

  for (uint32_t idx = 0; idx < count; idx++) {
    code = fetch_chunk(req, idx);
    if (code) {
      goto fail;
    }
    uint32_t len = tb_fw_chunk_len(size, idx);
    if (psa_hash_update(&op, buf, len) != PSA_SUCCESS) {
      code = "write_failed";
      goto fail;
    }
    esp_err_t werr = esp_ota_write(handle, buf, len);
    if (werr != ESP_OK) {
      /* The first write checks the image magic byte: a file that is not a firmware image is final. */
      code = (werr == ESP_ERR_OTA_VALIDATE_FAILED) ? "invalid_image" : "write_failed";
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
  {
    /* The image's own version must be the package version, or the next boot could never match the pending one. */
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(next, &desc) != ESP_OK || !tb_fw_version_equal(desc.version, s_job.version)) {
      code = "version_mismatch";
      goto fail;
    }
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
  psa_hash_abort(&op); /* valid on an INIT, finished or already aborted operation */
  if (begun) {
    esp_ota_abort(handle);
  }
  report_state("FAILED", code, false);
  if (tb_fw_error_is_final(code)) {
    remember_failed(s_job.version);
  }
  /* Disarm the wait and drop the task handle in one critical section: the hook notifies only inside it. */
  portENTER_CRITICAL(&s_mux);
  atomic_store(&s_wait_idx, IDX_NONE);
  s_buf = NULL; /* the hook no longer writes into the buffer */
  s_task = NULL;
  portEXIT_CRITICAL(&s_mux);
  heap_caps_free(buf);
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
      portENTER_CRITICAL(&s_mux);
      TaskHandle_t task = s_task;
      if (task != NULL && atomic_load(&s_wait_req) == req && atomic_load(&s_wait_idx) == idx) {
        atomic_store(&s_wait_idx, IDX_NONE);
        xTaskNotify(task, NOTE_BAD_CHUNK, eSetValueWithOverwrite);
      }
      portEXIT_CRITICAL(&s_mux);
      return;
    }
    s_rx_active = true;
  }
  if (!s_rx_active || d->offset < 0 || d->data_len < 0 || (uint32_t)d->total != want ||
      (uint32_t)d->offset + (uint32_t)d->data_len > want) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  TaskHandle_t task = s_task;
  if (task != NULL && s_buf != NULL && atomic_load(&s_wait_req) == req && atomic_load(&s_wait_idx) == idx) {
    memcpy(s_buf + d->offset, d->data, (size_t)d->data_len);
    if ((uint32_t)d->offset + (uint32_t)d->data_len == want) {
      /* Invalidate the awaited index first: a duplicate reply (from a retry) must not overwrite the buffer
       * while the task reads it. */
      atomic_store(&s_wait_idx, IDX_NONE);
      s_rx_active = false;
      xTaskNotify(task, NOTE_OK, eSetValueWithOverwrite); /* inside the lock: the task cannot be gone */
    }
  }
  portEXIT_CRITICAL(&s_mux);
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
    portENTER_CRITICAL(&s_mux);
    TaskHandle_t task = s_task;
    if (task != NULL && atomic_load(&s_wait_idx) != IDX_NONE) {
      atomic_store(&s_wait_idx, IDX_NONE);
      xTaskNotify(task, NOTE_SERVER_ERROR, eSetValueWithOverwrite);
    }
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  uint32_t req = 0, idx = 0;
  if (tb_fw_parse_chunk_topic(d->topic, tl, &req, &idx)) {
    on_chunk(d, req, idx);
  }
}

/* ---- boot gating: mark valid only after the image has reached ThingsBoard ---- */

static atomic_int s_boot_state = TB_FW_BOOT_NONE; /* tb_fw_boot_t */
static esp_timer_handle_t s_verify_timer = NULL;
static uint32_t s_attr_req_n = 0; /* tb_link task only */

static void verify_timeout_cb(void *arg) {
  (void)arg;
  ESP_LOGE(TAG, "OTA from ThingsBoard: no ThingsBoard connect within 5 min, rolling back");
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

bool tb_ota_boot_holds_valid(void) { return atomic_load(&s_boot_state) == TB_FW_BOOT_VERIFY; }

void tb_ota_boot_init(void) {
  char pending[TB_FW_VERSION_MAX];
  bool has_pending = nvs_store_get_tb_ota_pending(pending, sizeof pending);
  esp_ota_img_states_t st;
  bool pending_verify =
      esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  const char *running = esp_app_get_description()->version;
  tb_fw_boot_t b = tb_fw_boot_state(has_pending ? pending : NULL, running, pending_verify);
  atomic_store(&s_boot_state, (int)b);
  ESP_LOGI(TAG, "boot state %d (pending %s, running %s, pending_verify %d)", (int)b, has_pending ? pending : "none",
           running, (int)pending_verify);
  if (b == TB_FW_BOOT_ROLLED_BACK && has_pending) {
    /* The rolled-back version must not be fetched again at once (reboot loop): final until the next reboot. */
    remember_failed(pending);
  }
  if (b == TB_FW_BOOT_VERIFY) {
    const esp_timer_create_args_t args = {.callback = verify_timeout_cb, .name = "tb_ota_verify"};
    esp_err_t e = esp_timer_create(&args, &s_verify_timer);
    if (e == ESP_OK) {
      e = esp_timer_start_once(s_verify_timer, (uint64_t)TB_FW_VERIFY_TIMEOUT_MS * 1000u);
    }
    if (e != ESP_OK) {
      ESP_LOGE(TAG, "rollback timer failed (%s), rolling back now", esp_err_to_name(e));
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
}

void tb_ota_on_session(void) {
  tb_fw_boot_t b = (tb_fw_boot_t)atomic_load(&s_boot_state);
  if (b != TB_FW_BOOT_NONE) {
    bool resolved = true;
    if (b == TB_FW_BOOT_VERIFY) {
      if (s_verify_timer) {
        esp_timer_stop(s_verify_timer);
      }
      esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
      if (e != ESP_OK) {
        ESP_LOGW(TAG, "mark valid failed (%s), the rollback stays armed", esp_err_to_name(e));
        if (s_verify_timer) {
          esp_timer_start_once(s_verify_timer, (uint64_t)TB_FW_VERIFY_TIMEOUT_MS * 1000u);
        }
        resolved = false;
      } else {
        ESP_LOGI(TAG, "OTA from ThingsBoard: connected, image marked valid");
        /* Valid now: a failed report is retried as UPDATED, without marking again. */
        atomic_store(&s_boot_state, TB_FW_BOOT_UPDATED);
        b = TB_FW_BOOT_UPDATED;
      }
    }
    if (resolved) {
      bool ok = b == TB_FW_BOOT_UPDATED ? report_state("UPDATED", NULL, true)
                                        : report_state("FAILED", "rolled_back", false);
      if (ok) {
        if (!nvs_store_clear_tb_ota_pending()) {
          ESP_LOGW(TAG, "could not clear the pending version; the next boot resolves it again");
        }
        atomic_store(&s_boot_state, TB_FW_BOOT_NONE);
        uplink_note_settings_changed();
      } else {
        ESP_LOGW(TAG, "boot result not sent, it is reported again at the next connect");
      }
    }
  }
  report_state(NULL, NULL, true);
  char topic[sizeof TB_FW_ATTR_REQ_PREFIX + 12];
  int n = snprintf(topic, sizeof topic, TB_FW_ATTR_REQ_PREFIX "%" PRIu32, ++s_attr_req_n);
  if (n > 0 && (size_t)n < sizeof topic) {
    tb_link_publish(topic, TB_FW_ATTR_REQUEST_BODY, (int)strlen(TB_FW_ATTR_REQUEST_BODY), 1);
  }
}
