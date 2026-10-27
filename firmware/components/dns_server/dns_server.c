#include "dns_server.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "dns_server";
static TaskHandle_t s_dns_task = NULL;
static int s_dns_sock = -1;
static volatile bool s_dns_run = false;

#define DNS_PORT 53
#define DNS_MAX_QUERY_SIZE 512

/* DNS header structure (network byte order) */
typedef struct __attribute__((packed)) {
  uint16_t id;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
} dns_header_t;

/* Build a DNS response: echo back the question + add A-record answer with given
 * IP */
static void build_dns_response(const uint8_t *query, size_t query_len,
                               uint8_t *resp, size_t resp_capacity,
                               size_t *resp_len, uint32_t answer_ip) {
  const size_t answer_len = 16;

  if (!query || !resp || !resp_len || query_len < sizeof(dns_header_t) ||
      resp_capacity < sizeof(dns_header_t) + answer_len) {
    if (resp_len) {
      *resp_len = 0;
    }
    return;
  }

  *resp_len = 0;
  if (query_len > resp_capacity) {
    *resp_len = 0;
    return;
  }

  const dns_header_t *req_hdr = (const dns_header_t *)query;
  dns_header_t *resp_hdr = (dns_header_t *)resp;

  /* Copy header, set response flag + no error */
  memcpy(resp_hdr, req_hdr, sizeof(dns_header_t));
  resp_hdr->flags =
      htons(0x8580); /* QR=1, Opcode=0, AA=0, TC=0, RD=1, RA=1, RCODE=0 */
  resp_hdr->ancount = htons(1); /* One answer record */

  /* Copy the question section verbatim */
  size_t offset = sizeof(dns_header_t);
  const uint8_t *qname_start = query + offset;

  /* Find end of QNAME (compressed labels end with 0x00) */
  const uint8_t *p = qname_start;
  while (offset < query_len && *p != 0) {
    if ((*p & 0xc0) != 0 || offset + (size_t)(*p) + 1 >= query_len) {
      *resp_len = 0;
      return;
    }
    offset += (*p) + 1;
    p = query + offset;
  }
  if (offset >= query_len) {
    *resp_len = 0;
    return;
  }
  offset += 1; /* skip the 0x00 terminator */

  /* Copy QTYPE and QCLASS (4 bytes after QNAME) */
  if (offset + 4 > query_len) {
    *resp_len = 0;
    return;
  }
  offset += 4;

  size_t question_len = (size_t)((query + offset) - qname_start);
  if (sizeof(dns_header_t) + question_len + answer_len > resp_capacity) {
    *resp_len = 0;
    return;
  }
  memcpy(resp + sizeof(dns_header_t), qname_start, question_len);

  /* Now append the answer record */
  size_t ans_offset = sizeof(dns_header_t) + question_len;

  /* Answer: NAME pointer to the question (compressed) */
  resp[ans_offset++] = 0xc0; /* pointer to QNAME */
  resp[ans_offset++] = 0x0c; /* offset 12 (start of question) */

  /* TYPE (A record) */
  resp[ans_offset++] = 0x00;
  resp[ans_offset++] = 0x01;

  /* CLASS (IN) */
  resp[ans_offset++] = 0x00;
  resp[ans_offset++] = 0x01;

  /* TTL (60 seconds) */
  resp[ans_offset++] = 0x00;
  resp[ans_offset++] = 0x00;
  resp[ans_offset++] = 0x00;
  resp[ans_offset++] = 0x3c;

  /* RDLENGTH (4 bytes) */
  resp[ans_offset++] = 0x00;
  resp[ans_offset++] = 0x04;

  /* RDATA (IPv4 address) */
  memcpy(resp + ans_offset, &answer_ip, 4);
  ans_offset += 4;

  *resp_len = ans_offset;

  ESP_LOGD(TAG, "DNS response built, %u bytes", (unsigned)*resp_len);
}

static void dns_server_task(void *pvParameter) {
  ESP_LOGI(TAG, "DNS server task started");

  s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_dns_sock < 0) {
    ESP_LOGE(TAG, "Failed to create DNS socket");
    s_dns_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  /* Allow address reuse */
  int opt = 1;
  setsockopt(s_dns_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  if (bind(s_dns_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind DNS socket on port %d", DNS_PORT);
    close(s_dns_sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "DNS server listening on port %d", DNS_PORT);

  /* Set receive timeout so recvfrom doesn't block forever */
  struct timeval tv = {
      .tv_sec = 0,
      .tv_usec = 500000 // 500ms
  };
  setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  uint8_t query_buf[DNS_MAX_QUERY_SIZE];
  uint8_t resp_buf[DNS_MAX_QUERY_SIZE];
  struct sockaddr_in client_addr;
  socklen_t client_len;

  /* Cache AP IP once outside loop */
  ip4_addr_t ap_ip;
  ip4addr_aton("192.168.4.1", &ap_ip);

  while (s_dns_run) {
    client_len = sizeof(client_addr);
    int len = recvfrom(s_dns_sock, query_buf, sizeof(query_buf), 0,
                       (struct sockaddr *)&client_addr, &client_len);
    if (len <= 0) {
      continue;
    }

    if (len < (int)sizeof(dns_header_t))
      continue;

    /* Only respond to standard queries (QR=0) */
    dns_header_t *hdr = (dns_header_t *)query_buf;
    if (ntohs(hdr->flags) & 0x8000)
      continue;

    size_t resp_len = 0;
    build_dns_response(query_buf, (size_t)len, resp_buf, sizeof(resp_buf),
                       &resp_len, ap_ip.addr);

    if (resp_len > 0) {
      sendto(s_dns_sock, resp_buf, resp_len, 0, (struct sockaddr *)&client_addr,
             client_len);
    }
  }

  ESP_LOGI(TAG, "DNS server task exiting");
  if (s_dns_sock >= 0) {
    close(s_dns_sock);
    s_dns_sock = -1;
  }
  s_dns_task = NULL;
  vTaskDelete(NULL);
}

bool dns_server_start(void) {
  if (s_dns_task) {
    ESP_LOGW(TAG, "DNS server already running");
    return true;
  }

  s_dns_run = true;
  BaseType_t ret =
      xTaskCreatePinnedToCore(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task, 0);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create DNS server task");
    return false;
  }

  return true;
}

bool dns_server_is_running(void) { return s_dns_task != NULL; }

void dns_server_stop(void) {
  if (s_dns_task) {
    s_dns_run = false;
    while (s_dns_task != NULL) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  ESP_LOGI(TAG, "DNS server stopped");
}
