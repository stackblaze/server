/*****************************************************************************

Copyright (c) 2024, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file page_server/page_server_client.cc
Page Server client implementation

This module implements the client-side RPC calls to the remote Page Server.
For now, this is a stub implementation that can be extended with actual
gRPC or custom protocol implementation.

Created 2024
*******************************************************/

#include "page_server.h"
#include "srv0srv.h"
#include "log0log.h"
#include "mach0data.h"
#include "os0file.h"
#include "log.h"
#include "fil0fil.h"
#include "my_sys.h"

#include <string.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/** Page Server connection state */
static std::atomic<bool> page_server_enabled{false};
static std::atomic<bool> page_server_initialized{false};
static char* page_server_address = nullptr;
static char* page_server_host = nullptr;
static uint page_server_port = 8080;
static int page_server_socket = -1;  // Reused connection
static mysql_mutex_t page_server_mutex;

/** Simple HTTP-based protocol implementation */
namespace {
  constexpr size_t MAX_RESPONSE_SIZE = 64 * 1024;  // 64KB max response
  constexpr int PAGE_SERVER_CONNECT_TIMEOUT = 5;  // 5 seconds
  constexpr int READ_TIMEOUT = 10;    // 10 seconds
  constexpr int MAX_RETRIES = 2;

  /** Base64 encoding table */
  static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  /** Encode binary data to base64 */
  static void base64_encode(const byte* data, size_t len, char* out, size_t out_len)
  {
    size_t i = 0, j = 0;
    for (i = 0; i < len && j + 4 < out_len; i += 3) {
      uint32_t b = (data[i] << 16) | ((i + 1 < len ? data[i + 1] : 0) << 8) |
                   (i + 2 < len ? data[i + 2] : 0);
      out[j++] = base64_chars[(b >> 18) & 0x3F];
      out[j++] = base64_chars[(b >> 12) & 0x3F];
      out[j++] = (i + 1 < len) ? base64_chars[(b >> 6) & 0x3F] : '=';
      out[j++] = (i + 2 < len) ? base64_chars[b & 0x3F] : '=';
    }
    out[j] = '\0';
  }

  /** Decode base64 to binary */
  static size_t base64_decode(const char* in, byte* out, size_t out_len)
  {
    size_t len = strlen(in);
    size_t i = 0, j = 0;
    uint32_t val = 0;
    int pad = 0;

    for (i = 0; i < len && j < out_len; i++) {
      char c = in[i];
      if (c == '=') {
        pad++;
        continue;
      }
      int idx = -1;
      if (c >= 'A' && c <= 'Z') idx = c - 'A';
      else if (c >= 'a' && c <= 'z') idx = c - 'a' + 26;
      else if (c >= '0' && c <= '9') idx = c - '0' + 52;
      else if (c == '+') idx = 62;
      else if (c == '/') idx = 63;
      else continue;

      val = (val << 6) | idx;
      if ((i + 1) % 4 == 0) {
        if (j < out_len) out[j++] = (val >> 16) & 0xFF;
        if (j < out_len && pad < 2) out[j++] = (val >> 8) & 0xFF;
        if (j < out_len && pad < 1) out[j++] = val & 0xFF;
        val = 0;
      }
    }
    return j;
  }

  /** Parse host:port address */
  static bool parse_address(const char* address, char** host, uint* port)
  {
    if (!address) return false;

    char* addr_copy = static_cast<char*>(my_malloc(PSI_INSTRUMENT_ME, strlen(address) + 1, MYF(0)));
    if (!addr_copy) return false;
    strcpy(addr_copy, address);

    char* colon = strchr(addr_copy, ':');
    if (colon) {
      *colon = '\0';
      *host = addr_copy;
      *port = static_cast<uint>(atoi(colon + 1));
      if (*port == 0) *port = 8080;
    } else {
      *host = addr_copy;
      *port = 8080;
    }
    return true;
  }

  /** Connect to Page Server */
  static int http_connect(const char* host, uint port)
  {
    struct sockaddr_in server_addr;
    struct hostent* server;
    int sockfd;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    server = gethostbyname(host);
    if (!server) {
      close(sockfd);
      return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(static_cast<uint16_t>(port));

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = PAGE_SERVER_CONNECT_TIMEOUT;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
      close(sockfd);
      return -1;
    }

    return sockfd;
  }

  /** Send HTTP request and read response */
  static dberr_t http_request(const char* method, const char* path,
                              const char* body, char* response, size_t response_size)
  {
    int sockfd = page_server_socket;
    bool reconnect = false;

    // Reconnect if socket is invalid
    if (sockfd < 0 || !page_server_host) {
      reconnect = true;
    }

    for (int retry = 0; retry <= MAX_RETRIES; retry++) {
      if (reconnect || sockfd < 0) {
        if (sockfd >= 0) close(sockfd);
        sockfd = http_connect(page_server_host, page_server_port);
        if (sockfd < 0) {
          if (retry < MAX_RETRIES) {
            usleep(100000 * (1 << retry));  // Exponential backoff
            continue;
          }
          return DB_ERROR;
        }
        page_server_socket = sockfd;
      }

      // Build HTTP request
      // Allocate request buffer dynamically to avoid large stack frame
      char* request = static_cast<char*>(my_malloc(PSI_INSTRUMENT_ME, 4096, MYF(0)));
      if (!request) return DB_ERROR;
      int len = snprintf(request, 4096,
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        method, path, page_server_host, page_server_port,
        body ? strlen(body) : 0, body ? body : "");

      if (send(sockfd, request, len, 0) < 0) {
        my_free(request);
        reconnect = true;
        if (retry < MAX_RETRIES) {
          usleep(100000 * (1 << retry));
          continue;
        }
        return DB_ERROR;
      }
      my_free(request);

      // Read response
      size_t total = 0;
      char buffer[4096];
      while (total < response_size - 1) {
        ssize_t n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
          reconnect = true;
          break;
        }
        buffer[n] = '\0';
        if (total + n >= response_size - 1) {
          memcpy(response + total, buffer, response_size - total - 1);
          total = response_size - 1;
          break;
        }
        memcpy(response + total, buffer, n);
        total += n;
        response[total] = '\0';

        // Check if we have complete HTTP response
        if (strstr(response, "\r\n\r\n")) break;
      }

      if (total > 0) {
        response[total] = '\0';
        // Check for HTTP 200
        if (strstr(response, "HTTP/1.1 200") || strstr(response, "HTTP/1.0 200")) {
          return DB_SUCCESS;
        }
      }

      if (retry < MAX_RETRIES) {
        reconnect = true;
        usleep(100000 * (1 << retry));
      }
    }

    return DB_ERROR;
  }

  /** Extract JSON value (simple parser) */
  static bool json_get_string(const char* json, const char* key, char* value, size_t value_len)
  {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return false;
    pos = strchr(pos, ':');
    if (!pos) return false;
    pos++;  // Skip ':'
    while (*pos == ' ' || *pos == '\t') pos++;  // Skip whitespace
    if (*pos != '"') return false;
    pos++;  // Skip opening quote
    size_t i = 0;
    while (*pos && *pos != '"' && i < value_len - 1) {
      if (*pos == '\\') {
        pos++;
        if (*pos == 'n') value[i++] = '\n';
        else if (*pos == 'r') value[i++] = '\r';
        else if (*pos == 't') value[i++] = '\t';
        else value[i++] = *pos;
      } else {
        value[i++] = *pos;
      }
      pos++;
    }
    value[i] = '\0';
    return true;
  }

  /** Extract JSON number */
  static bool json_get_uint64(const char* json, const char* key, uint64_t* value)
  {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return false;
    pos = strchr(pos, ':');
    if (!pos) return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    *value = strtoull(pos, nullptr, 10);
    return true;
  }

  /** Extract JSON body from HTTP response */
  static const char* extract_json_body(const char* http_response)
  {
    const char* body = strstr(http_response, "\r\n\r\n");
    if (body) return body + 4;
    body = strstr(http_response, "\n\n");
    if (body) return body + 2;
    return http_response;  // Assume entire response is JSON
  }

  /** RPC call to get page */
dberr_t rpc_get_page(
  uint32_t space_id,
  uint32_t page_no,
  lsn_t lsn,
  void* buf,
  size_t page_size,
  lsn_t* page_lsn)
  {
    char request_body[512];
    snprintf(request_body, sizeof(request_body),
      "{\"space_id\":%u,\"page_no\":%u,\"lsn\":%llu}",
      space_id, page_no, static_cast<unsigned long long>(lsn));

    // Allocate response buffer dynamically to avoid large stack frame
    char* response = static_cast<char*>(my_malloc(PSI_INSTRUMENT_ME, MAX_RESPONSE_SIZE, MYF(0)));
    if (!response) return DB_ERROR;

    dberr_t err = http_request("POST", "/api/v1/get_page", request_body, response, MAX_RESPONSE_SIZE);
    if (err != DB_SUCCESS) {
      my_free(response);
      return err;
    }

    const char* json = extract_json_body(response);
    char status[64];
    if (!json_get_string(json, "status", status, sizeof(status))) {
      my_free(response);
      return DB_ERROR;
    }

    if (strcmp(status, "success") != 0) {
      my_free(response);
      return DB_ERROR;
    }

    // Allocate base64 buffer dynamically
    char* page_data_b64 = static_cast<char*>(my_malloc(PSI_INSTRUMENT_ME, MAX_RESPONSE_SIZE, MYF(0)));
    if (!page_data_b64) {
      my_free(response);
      return DB_ERROR;
    }

    bool got_data = json_get_string(json, "page_data", page_data_b64, MAX_RESPONSE_SIZE);
    if (!got_data) {
      my_free(page_data_b64);
      my_free(response);
      return DB_ERROR;
    }

    size_t decoded_len = base64_decode(page_data_b64, static_cast<byte*>(buf), page_size);
    my_free(page_data_b64);
    my_free(response);

    if (decoded_len == 0 || decoded_len > page_size) {
      return DB_ERROR;
    }

    uint64_t resp_lsn;
    if (json_get_uint64(json, "page_lsn", &resp_lsn)) {
      *page_lsn = resp_lsn;
    } else {
      *page_lsn = lsn;
    }

    return DB_SUCCESS;
  }

  /** Stream WAL record to Page Server */
  dberr_t rpc_stream_wal(
    lsn_t lsn,
    const byte* wal_data,
    size_t wal_len)
  {
    // Base64 encode WAL data
    size_t b64_len = ((wal_len + 2) / 3) * 4 + 1;
    char* wal_b64 = static_cast<char*>(my_malloc(PSI_INSTRUMENT_ME, b64_len, MYF(0)));
    if (!wal_b64) return DB_ERROR;

    base64_encode(wal_data, wal_len, wal_b64, b64_len);

    char request_body[8192];
    snprintf(request_body, sizeof(request_body),
      "{\"lsn\":%llu,\"wal_data\":\"%s\"}",
      static_cast<unsigned long long>(lsn), wal_b64);

    char response[1024];
    dberr_t err = http_request("POST", "/api/v1/stream_wal", request_body, response, sizeof(response));

    my_free(wal_b64);
    return err;
  }
}

bool PageServerClient::init(const char* address)
{
  mysql_mutex_init(0, &page_server_mutex, MY_MUTEX_INIT_FAST);

  if (!address || strlen(address) == 0) {
    page_server_enabled = false;
    page_server_initialized = true;
    return true;  // Disabled is valid
  }

  // Store address
  size_t addr_len = strlen(address) + 1;
  page_server_address = static_cast<char*>(my_malloc(PSI_INSTRUMENT_ME, addr_len, MYF(0)));
  if (!page_server_address) {
    return false;
  }
  strcpy(page_server_address, address);

  // Parse host:port
  if (!parse_address(address, &page_server_host, &page_server_port)) {
    my_free(page_server_address);
    page_server_address = nullptr;
    return false;
  }

  page_server_enabled = true;
  page_server_initialized = true;

  // Test connection with ping
  if (!ping()) {
    sql_print_warning("InnoDB: Page Server ping failed, disabling");
    page_server_enabled = false;
    if (page_server_host != page_server_address) {
      my_free(page_server_host);
    }
    my_free(page_server_address);
    page_server_address = nullptr;
    page_server_host = nullptr;
    return false;
  }

  sql_print_information("InnoDB: Page Server client initialized: %s", address);
  return true;
}

void PageServerClient::shutdown()
{
  mysql_mutex_lock(&page_server_mutex);

  if (page_server_socket >= 0) {
    close(page_server_socket);
    page_server_socket = -1;
  }

  if (page_server_address) {
    my_free(page_server_address);
    page_server_address = nullptr;
  }

  if (page_server_host && page_server_host != page_server_address) {
    ut_free(page_server_host);
    page_server_host = nullptr;
  }

  page_server_enabled = false;
  page_server_initialized = false;

  mysql_mutex_unlock(&page_server_mutex);
  mysql_mutex_destroy(&page_server_mutex);
}

bool PageServerClient::is_enabled()
{
  return page_server_enabled.load() && page_server_initialized.load();
}

dberr_t PageServerClient::get_page(
  uint32_t space_id,
  uint32_t page_no,
  lsn_t lsn,
  void* buf,
  size_t page_size,
  lsn_t* page_lsn)
{
  if (!is_enabled()) {
    return DB_ERROR;
  }

  mysql_mutex_lock(&page_server_mutex);

  dberr_t err = rpc_get_page(space_id, page_no, lsn, buf, page_size, page_lsn);

  mysql_mutex_unlock(&page_server_mutex);

  if (err != DB_SUCCESS) {
    // Log error for debugging
    sql_print_information(
      "InnoDB: Page Server get_page failed: space=%u page=%u lsn=" LSN_PF,
      space_id, page_no, lsn);
  }

  return err;
}

dberr_t PageServerClient::stream_wal(
  lsn_t lsn,
  const byte* wal_data,
  size_t wal_len)
{
  if (!is_enabled()) {
    return DB_SUCCESS;  // Silently ignore if disabled
  }

  mysql_mutex_lock(&page_server_mutex);

  dberr_t err = rpc_stream_wal(lsn, wal_data, wal_len);

  mysql_mutex_unlock(&page_server_mutex);

  return err;
}

size_t PageServerClient::get_pages_batch(
  const PageRequest* requests,
  size_t num_reqs,
  PageResult* results)
{
  if (!is_enabled() || !requests || !results) {
    return 0;
  }

  mysql_mutex_lock(&page_server_mutex);

  size_t success_count = 0;

  // TODO: Implement batch RPC call
  // For now, fall back to individual calls
  for (size_t i = 0; i < num_reqs; i++) {
    lsn_t page_lsn;
    results[i].err = rpc_get_page(
      requests[i].space_id,
      requests[i].page_no,
      requests[i].lsn,
      results[i].page_data,
      srv_page_size,
      &page_lsn);
    results[i].page_lsn = page_lsn;

    if (results[i].err == DB_SUCCESS) {
      success_count++;
    }
  }

  mysql_mutex_unlock(&page_server_mutex);

  return success_count;
}

bool PageServerClient::ping()
{
  if (!page_server_initialized.load()) {
    return false;
  }

  if (!page_server_enabled.load()) {
    return true;  // Disabled is considered "healthy"
  }

  if (!page_server_host) {
    return false;
  }

  char response[1024];
  dberr_t err = http_request("GET", "/api/v1/ping", nullptr, response, sizeof(response));
  return (err == DB_SUCCESS);
}

