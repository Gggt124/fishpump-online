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
#include "telemetry_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_FW_TYPE "fishpump"
#define UPLINK_CONTRACT_VERSION 1
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

#ifdef __cplusplus
}
#endif
