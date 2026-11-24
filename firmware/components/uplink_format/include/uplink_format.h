#pragma once

/*
 * ThingsBoard-format payload builders for the uplink layer (spec 5.8). Pure C:
 * no transport, no clock, no FreeRTOS. Every builder returns the JSON length
 * written (NUL-terminated) or -1 if it does not fit / out of memory.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_rules.h"
#include "cJSON.h"
#include "telemetry_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_FW_TYPE "fishpump"
#define UPLINK_CONTRACT_VERSION 2
#define UPLINK_TELEMETRY_BUF 2048
#define UPLINK_EVENT_BUF 768
#define UPLINK_IDENTITY_BUF 8192
#define UPLINK_RULES_BUF 12288

/* One flat object: every trusted registry signal plus alarm_active, alarm_critical,
 * alarm_unacked and alarm_max_severity. Untrusted signals are omitted. */
int uplink_format_telemetry(const telemetry_snapshot_t *snap, uint32_t now_ms, const alarm_counts_t *counts,
                            char *buf, size_t size);

/* ts_unix_ms == 0 -> plain {"alarm_event":{...}}; otherwise {"ts":...,"values":{"alarm_event":{...}}}. */
int uplink_format_event(const alarm_event_t *evt, uint32_t boot_id, int64_t ts_unix_ms, char *buf, size_t size);

/* {"fw_type","fw_version","contract_version","signals":[...]} */
int uplink_format_identity(const char *fw_version, char *buf, size_t size);

/* {"alarm_rules":[...]} */
int uplink_format_rules(const alarm_rule_set_t *set, char *buf, size_t size);

/* Wall-clock time of an event recorded at event_uptime_ms. Returns 0 when now_unix_ms == 0.
 * Unsigned subtraction, so it survives the 49.7-day uptime wrap. */
int64_t uplink_event_ts(int64_t now_unix_ms, uint32_t now_uptime_ms, uint32_t event_uptime_ms);

typedef struct {
    uint32_t last_forwarded_seq;
    uint32_t dropped;
} uplink_forward_t;

/* Call before fetching events: if the oldest event still held is newer than
 * last_forwarded_seq + 1, the gap is added to dropped and last_forwarded_seq jumps
 * to oldest_seq - 1. oldest_seq 0 means an empty ring. Returns the gap. */
uint32_t uplink_forward_account_gap(uplink_forward_t *f, uint32_t oldest_seq);

/* Events wait for the wall clock (so they carry a ts) unless that would let the ring overwrite them, or
 * the wait has lasted too long: hold while the clock is unset, pending + UPLINK_CLOCK_HOLD_MARGIN < ring_len
 * and held_ms (time since the hold started) < UPLINK_CLOCK_HOLD_MAX_MS. */
#define UPLINK_CLOCK_HOLD_MARGIN 8u
#define UPLINK_CLOCK_HOLD_MAX_MS 120000u
bool uplink_forward_hold_for_clock(bool clock_set, uint32_t pending, uint32_t ring_len, uint32_t held_ms);

/* Hold/latch state, owned by the uplink task. active: events are being held, started at start_ms.
 * released: the hold ended with the clock still unset (ring margin or time cap), so events go out without
 * ts until the clock is set. Both survive sink drops: the time a sink is down counts toward the cap, so a
 * flapping link cannot restart the 120 s hold. Only the clock arriving resets them. */
typedef struct {
    bool active;
    bool released;
    uint32_t start_ms;
} uplink_clock_hold_t;

typedef enum {
    UPLINK_CLOCK_IDLE = 0,    /* nothing to send now (sink not ready, or nothing pending) */
    UPLINK_CLOCK_HOLD,        /* keep the events in the ring */
    UPLINK_CLOCK_SEND_TS,     /* send; the clock is set */
    UPLINK_CLOCK_SEND_NO_TS   /* send without ts: the hold is over and the clock is still unset */
} uplink_clock_decision_t;

typedef enum {
    UPLINK_CLOCK_EV_NONE = 0,
    UPLINK_CLOCK_EV_HOLD_STARTED,
    UPLINK_CLOCK_EV_ENDED_CLOCK,   /* the clock was set while holding */
    UPLINK_CLOCK_EV_ENDED_EMPTY,   /* nothing pending any more while holding */
    UPLINK_CLOCK_EV_RELEASED_MARGIN,
    UPLINK_CLOCK_EV_RELEASED_CAP
} uplink_clock_hold_event_t;

/* pending: events waiting (0 = none). now_ms is uptime; elapsed time uses unsigned subtraction.
 * *event reports a state transition for the caller to log once (may be NULL). */
uplink_clock_decision_t uplink_clock_hold_step(uplink_clock_hold_t *st, bool clock_set, bool sink_ready,
                                               uint32_t pending, uint32_t ring_len, uint32_t now_ms,
                                               uplink_clock_hold_event_t *event);

/* ---- ThingsBoard state (client attributes + status telemetry), milestone 3 ---- */
#define UPLINK_STATE_ATTR_MAX 8
#define UPLINK_STATE_KEY_MAX 24
#define UPLINK_STATE_ATTR_MAX_BYTES 4096
#define UPLINK_STATE_RECHECK_MS 60000u
#define UPLINK_STATUS_INTERVAL_MS 30000u
#define UPLINK_STATUS_MIN_GAP_MS 2000u
#define UPLINK_STATUS_POLL_MS 2000u
#define UPLINK_STATUS_OFFLINE_POLL_MS 30000u

/* FNV-1a 32-bit. */
uint32_t uplink_hash32(const char *data, size_t len);

/* Per-key "last sent" hash of each attribute group. */
typedef struct {
    char key[UPLINK_STATE_KEY_MAX];
    uint32_t hash;
} uplink_attr_slot_t;

typedef struct {
    uplink_attr_slot_t slot[UPLINK_STATE_ATTR_MAX];
    size_t n;
} uplink_attr_cache_t;

void uplink_attr_cache_reset(uplink_attr_cache_t *c);
/* True for an unknown key or a different hash. */
bool uplink_attr_cache_changed(const uplink_attr_cache_t *c, const char *key, uint32_t hash);
/* False when the key is too long (>= UPLINK_STATE_KEY_MAX) or the table is full. */
bool uplink_attr_cache_commit(uplink_attr_cache_t *c, const char *key, uint32_t hash);

bool uplink_state_rebuild_due(bool dirty, bool ever, uint32_t since_build_ms);
bool uplink_status_poll_due(bool connected, bool ever, uint32_t since_poll_ms);
bool uplink_status_send_due(bool ever, uint32_t since_sent_ms, bool watched_changed);

/* Deep copy of obj minus the listed top-level keys. NULL if obj is not an object or out of memory. */
cJSON *uplink_json_without(const cJSON *obj, const char *const *drop, size_t ndrop);

/* Takes ownership of reply. A cJSON Raw node (some device_api replies) is parsed into a real tree and the raw
 * node deleted; NULL if the parse fails. Any other node is returned as is. NULL in, NULL out. */
cJSON *uplink_json_materialize(cJSON *reply);

typedef enum { UPLINK_SRC_PUMP = 0, UPLINK_SRC_COOLING, UPLINK_SRC_STATUS, UPLINK_SRC_COUNT } uplink_status_src_t;

typedef struct {
    uint8_t src;
    const char *path; /* dot-separated, into the method reply */
    const char *key;
    bool watched;
} uplink_status_field_t;

extern const uplink_status_field_t uplink_status_fields[];
extern const size_t uplink_status_field_count;

/* Adds each field found (number, bool or string) to out under its key. NULL source or missing path: skipped.
 * Returns the number of keys added. *watched_hash (may be NULL): FNV-1a over "key=<value>;" of each watched
 * field in table order ("key=?;" when missing). */
int uplink_status_flatten(const cJSON *const srcs[UPLINK_SRC_COUNT], cJSON *out, uint32_t *watched_hash);

#ifdef __cplusplus
}
#endif
