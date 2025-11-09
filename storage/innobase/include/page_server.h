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
@file page_server.h
Page Server client interface for Neon-style architecture

This module provides the interface for redirecting InnoDB page I/O
to a remote Page Server, enabling stateless MySQL compute nodes.

Created 2024
*******************************************************/

#ifndef PAGE_SERVER_H
#define PAGE_SERVER_H

#include "univ.i"
#include "page0types.h"
#include "log0types.h"
#include "db0err.h"

/* Forward declarations */
struct fil_node_t;

/** Page Server client state */
class PageServerClient {
public:
  /** Initialize Page Server client
  @param[in] address  Page Server address (hostname:port)
  @return true if initialization successful */
  static bool init(const char* address);

  /** Shutdown Page Server client */
  static void shutdown();

  /** Check if Page Server is enabled and connected
  @return true if Page Server is active */
  static bool is_enabled();

  /** Fetch a page from Page Server
  @param[in] space_id  Tablespace ID
  @param[in] page_no   Page number
  @param[in] lsn       Log Sequence Number (for consistency)
  @param[out] buf      Buffer to store page data (must be at least page_size)
  @param[in] page_size Page size (typically 16KB)
  @param[out] page_lsn LSN of the fetched page version
  @return DB_SUCCESS on success, error code on failure */
  static dberr_t get_page(
    uint32_t space_id,
    uint32_t page_no,
    lsn_t lsn,
    void* buf,
    size_t page_size,
    lsn_t* page_lsn);

  /** Stream WAL record to Page Server
  @param[in] lsn       Log Sequence Number
  @param[in] wal_data  WAL record data
  @param[in] wal_len   Length of WAL data
  @return DB_SUCCESS on success */
  static dberr_t stream_wal(
    lsn_t lsn,
    const byte* wal_data,
    size_t wal_len);

  /** Batch fetch multiple pages
  @param[in] requests  Array of page requests
  @param[in] num_reqs  Number of requests
  @param[out] results   Array of page data buffers
  @return number of successfully fetched pages */
  static size_t get_pages_batch(
    const struct PageRequest* requests,
    size_t num_reqs,
    struct PageResult* results);

  /** Health check / ping Page Server
  @return true if Page Server is reachable */
  static bool ping();
};

/** Page request structure for batch operations */
struct PageRequest {
  uint32_t space_id;
  uint32_t page_no;
  lsn_t lsn;
};

/** Page result structure for batch operations */
struct PageResult {
  dberr_t err;
  lsn_t page_lsn;
  void* page_data;  // Caller must allocate buffer
};

#endif /* PAGE_SERVER_H */

