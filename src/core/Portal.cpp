// Captive-portal DNS hijack: answer every A query with the AP's own address so
// a phone's connectivity probe lands on our portal instead of timing out.
//
// Arduino's DNSServer is gone (IDF 6 migration, phase 6). It was AsyncUDP-based
// and ran the hijack on the lwIP tcpip thread; there is no framework-neutral
// equivalent, and IDF only ships one as example code, so this is ~80 lines of
// BSD sockets that compile unchanged on both frameworks — the same choice as
// Platform/Fs/Http.
//
// A task, not a tcpip-thread callback: recvfrom with a receive timeout lets the
// loop notice the stop flag, and the task deletes ITSELF once the socket is
// closed. The revision before Arduino's DNSServer had a pump task with a
// cross-core vTaskDelete hazard; self-deletion is what avoids reintroducing it.
#include "Portal.h"
#include "Platform.h"

#include <cstring>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

namespace Portal {

namespace {

constexpr uint16_t DNS_PORT = 53;
constexpr size_t DNS_MAX = 512; // classic UDP DNS limit; longer queries are ignored

int s_sock = -1;
TaskHandle_t s_task = nullptr;
volatile bool s_stop = false;

// The AP netif's address. Keyed by ifkey rather than the Arduino WiFi object so
// this survives phase 6c; arduino-esp32 registers the softAP under this key.
uint32_t apAddr() {
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) return 0;
  esp_netif_ip_info_t ip = {};
  if (esp_netif_get_ip_info(ap, &ip) != ESP_OK) return 0;
  return ip.ip.addr; // network byte order already
}

// Builds a reply in place. Returns the reply length, or 0 to send nothing.
//
// Only standard queries carrying exactly one A question are answered; anything
// else is ignored rather than answered wrongly (an AAAA answered with an A
// record is worse than no answer — the client would cache a lie).
size_t buildReply(uint8_t* buf, size_t len, uint32_t addr) {
  if (len < 12 || len > DNS_MAX - 16) return 0;
  const bool isQuery = (buf[2] & 0x80) == 0;        // QR == 0
  const bool stdQuery = (buf[2] & 0x78) == 0;       // OPCODE == 0
  const uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
  if (!isQuery || !stdQuery || qdcount != 1) return 0;

  // Walk the QNAME labels to find the question's type/class.
  size_t p = 12;
  while (p < len && buf[p] != 0) {
    if (buf[p] & 0xC0) return 0; // compression pointer in a question: malformed
    p += buf[p] + 1;
  }
  if (p >= len) return 0;
  p += 1;                      // the root label
  if (p + 4 > len) return 0;   // QTYPE + QCLASS
  const uint16_t qtype = (uint16_t)((buf[p] << 8) | buf[p + 1]);
  const uint16_t qclass = (uint16_t)((buf[p + 2] << 8) | buf[p + 3]);
  p += 4;
  if (qtype != 1 || qclass != 1) return 0; // not A/IN

  // Header: response, authoritative, no recursion available, 1 answer.
  buf[2] = 0x84;
  buf[3] = 0x00;
  buf[6] = 0x00; buf[7] = 0x01; // ANCOUNT
  buf[8] = 0x00; buf[9] = 0x00; // NSCOUNT
  buf[10] = 0x00; buf[11] = 0x00; // ARCOUNT

  // Answer: pointer back to the question's name, A/IN, short TTL, 4-byte rdata.
  static const uint8_t kAnswer[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                                    0x00, 0x00, 0x00, 0x1E, 0x00, 0x04};
  memcpy(buf + p, kAnswer, sizeof(kAnswer));
  p += sizeof(kAnswer);
  memcpy(buf + p, &addr, 4);
  return p + 4;
}

void dnsTask(void*) {
  uint8_t buf[DNS_MAX];
  while (!s_stop) {
    sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    int n = recvfrom(s_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n <= 0) continue; // timeout (so the stop flag is seen) or a transient error
    const uint32_t addr = apAddr();
    if (!addr) continue; // AP went away mid-flight
    size_t reply = buildReply(buf, (size_t)n, addr);
    if (reply) sendto(s_sock, buf, reply, 0, (sockaddr*)&from, fromLen);
  }
  close(s_sock);
  s_sock = -1;
  s_task = nullptr; // published last: end() waits on this
  vTaskDelete(nullptr);
}

} // namespace

void begin() {
  if (s_task) return;
  s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (s_sock < 0) return;
  // A receive timeout is what makes the stop flag observable without a second
  // wakeup mechanism.
  timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in me = {};
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  me.sin_port = htons(DNS_PORT);
  if (bind(s_sock, (sockaddr*)&me, sizeof(me)) < 0) {
    close(s_sock);
    s_sock = -1;
    return;
  }
  s_stop = false;
  // Core 0 with the rest of the network stack; consumer code owns core 1
  // (ADR 0001). 3 KB is ample for a 512-byte buffer and no recursion.
  if (xTaskCreatePinnedToCore(dnsTask, "portal_dns", 3072, nullptr, 5, &s_task, 0) != pdPASS) {
    close(s_sock);
    s_sock = -1;
    s_task = nullptr;
  }
}

void end() {
  if (!s_task) return;
  s_stop = true;
  // The task closes the socket and clears s_task on its way out; the receive
  // timeout bounds how long that takes. Cap the wait so a wedged task can never
  // block the caller (this runs from the main loop on a NetworkUp event).
  for (int i = 0; i < 30 && s_task; ++i) Platform::delayMs(100);
}

} // namespace Portal
