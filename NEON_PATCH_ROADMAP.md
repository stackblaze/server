# Neon-Style InnoDB Patch Roadmap

## Overview

This document provides a precise roadmap for patching InnoDB to implement a Neon-style architecture where:
- **Local `.ibd` files are treated as cache, not source of truth**
- **Page reads/writes are redirected to a remote Page Server**
- **WAL (redo log) is streamed to the Page Server**
- **MySQL compute nodes become stateless**

---

## Architecture Overview

```
MySQL Compute Node (Stateless)
     |
     | Buffer Pool Miss → RPC: GetPage(space_id, page_no, lsn)
     ↓
Remote Page Server (Holds Page Images + MVCC page chains)
     |
     | Background merges
     ↓
Object Storage (S3 / MinIO / GCS)
```

---

## Patch Locations

### 1. Buffer Pool Read Path

**File:** `storage/innobase/buf/buf0rea.cc`

**Function:** `buf_read_page_low()` (line ~258)

**Current Flow:**
```cpp
buf_read_page_low() 
  → space->io(IORequest::READ_SYNC, ...)  // Line ~329
  → fil_space_t::io() 
  → os_aio() 
  → local disk read
```

**Patch Strategy:**
- Add conditional check: if Page Server is enabled, call `page_server_get_page()` instead of `space->io()`
- Location: Before line 329 in `buf_read_page_low()`

**Code Location:**
- **Line 258-349:** `buf_read_page_low()` function
- **Line 329:** `space->io(IORequest(IORequest::READ_SYNC), ...)` - **INTERCEPT HERE**

---

### 2. File I/O Layer

**File:** `storage/innobase/fil/fil0fil.cc`

**Function:** `fil_space_t::io()` (line ~2787)

**Current Flow:**
```cpp
fil_space_t::io()
  → os_aio()  // Line ~2874
  → local disk I/O
```

**Patch Strategy:**
- Intercept at the entry point of `fil_space_t::io()`
- For reads: redirect to Page Server if enabled
- For writes: optionally redirect (or let WAL streaming handle it)

**Code Location:**
- **Line 2787-2893:** `fil_space_t::io()` function
- **Line 2874:** `os_aio()` call - **INTERCEPT BEFORE THIS**

---

### 3. WAL (Redo Log) Write Path

**File:** `storage/innobase/log/log0log.cc`

**Function:** `log_write_up_to()` (line ~1155)
**Function:** `log_write_buf()` (line ~764)

**Current Flow:**
```cpp
log_write_up_to()
  → log_write_persist()
  → log_write_buf()
  → os_file_write() (local disk)
```

**Patch Strategy:**
- After `log_write_buf()` completes, stream the WAL data to Page Server
- Add async WAL streaming to avoid blocking MySQL writes

**Code Location:**
- **Line 1155:** `log_write_up_to()` function entry
- **Line 764:** `log_write_buf()` function - **ADD STREAMING AFTER THIS**
- **Line 1103:** `log_write_buf(write_buf, length, offset)` call

---

### 4. Doublewrite Buffer (Disable/Redirect)

**File:** `storage/innobase/buf/buf0dblwr.cc`

**Current Behavior:**
- Doublewrite buffer ensures atomic page writes
- In Neon-style architecture, this is handled by the Page Server

**Patch Strategy:**
- Option 1: Disable doublewrite when Page Server is enabled
- Option 2: Redirect doublewrite writes to Page Server

**Code Location:**
- Search for `buf_dblwr` usage in `buf0dblwr.cc`
- Check `buf0buf.cc` for doublewrite buffer initialization

---

## Implementation Steps

### Phase 1: Remote Page I/O Abstraction

1. **Create:** `storage/innobase/include/page_server.h`
   - Define RPC client interface
   - Page Server connection management
   - GetPage/WritePage RPC stubs

2. **Create:** `storage/innobase/page_server/page_server_client.cc`
   - Implement RPC client using gRPC or custom protocol
   - Connection pooling
   - Retry logic

### Phase 2: Buffer Pool Integration

1. **Modify:** `storage/innobase/buf/buf0rea.cc`
   - Add Page Server check in `buf_read_page_low()`
   - Replace `space->io()` with `page_server_get_page()` when enabled

2. **Modify:** `storage/innobase/fil/fil0fil.cc`
   - Add Page Server check in `fil_space_t::io()`
   - Redirect reads to Page Server

### Phase 3: WAL Streaming

1. **Modify:** `storage/innobase/log/log0log.cc`
   - Add WAL streaming after `log_write_buf()`
   - Implement async WAL streaming queue

2. **Create:** `storage/innobase/page_server/wal_streamer.cc`
   - Background thread for WAL streaming
   - Batch WAL records for efficiency

### Phase 4: Configuration & Initialization

1. **Add:** System variables for Page Server configuration
   - `innodb_page_server_enabled`
   - `innodb_page_server_address`
   - `innodb_page_server_port`

2. **Modify:** `storage/innobase/srv/srv0start.cc`
   - Initialize Page Server connection on startup
   - Validate configuration

---

## File-by-File Patch Summary

| File | Function | Line | Action |
|------|----------|------|--------|
| `buf/buf0rea.cc` | `buf_read_page_low()` | ~329 | Replace `space->io()` with Page Server call |
| `fil/fil0fil.cc` | `fil_space_t::io()` | ~2787 | Add Page Server interception |
| `log/log0log.cc` | `log_write_buf()` | ~764 | Add WAL streaming after write |
| `log/log0log.cc` | `log_write_up_to()` | ~1155 | Ensure WAL streaming is triggered |
| `buf/buf0dblwr.cc` | Various | TBD | Disable/redirect doublewrite |

---

## RPC Protocol Design

See `PAGE_SERVER_RPC_API.md` for detailed protocol specification.

**Key RPC Methods:**
- `GetPage(space_id, page_no, lsn)` → Returns page data
- `StreamWAL(lsn, data)` → Streams WAL records
- `GetPageVersions(space_id, page_no, lsn_range)` → For MVCC

---

## Testing Strategy

1. **Unit Tests:**
   - Mock Page Server for testing I/O redirection
   - Test buffer pool miss path

2. **Integration Tests:**
   - Full MySQL instance with Page Server
   - Verify page reads from remote server
   - Verify WAL streaming

3. **Performance Tests:**
   - Latency impact of remote page reads
   - WAL streaming throughput

---

## Configuration Variables

Add to `storage/innobase/srv/srv0srv.h`:

```cpp
extern bool srv_page_server_enabled;
extern char* srv_page_server_address;
extern uint srv_page_server_port;
```

Add to `sql/sys_vars.cc`:

```cpp
static MYSQL_SYSVAR_BOOL(page_server_enabled, srv_page_server_enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable Neon-style Page Server (redirects page I/O to remote server)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(page_server_address, srv_page_server_address,
  PLUGIN_VAR_RQCMDARG,
  "Page Server address (hostname or IP)",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_UINT(page_server_port, srv_page_server_port,
  PLUGIN_VAR_RQCMDARG,
  "Page Server port",
  NULL, NULL, 8080, 1, 65535, 0);
```

---

## Critical Considerations

1. **LSN (Log Sequence Number) Tracking:**
   - Page Server must track LSN for each page version
   - GetPage requests must include LSN for consistency

2. **Crash Recovery:**
   - Local disk is disposable
   - Recovery must fetch pages from Page Server
   - Modify `log0recv.cc` to fetch pages remotely

3. **Performance:**
   - Page Server must be low-latency (ideally <1ms)
   - Consider local SSD cache for hot pages
   - Batch page requests when possible

4. **Consistency:**
   - Page Server must apply WAL before serving pages
   - Ensure read-after-write consistency

---

## Next Steps

1. Review and approve this roadmap
2. Implement Phase 1 (Remote Page I/O Abstraction)
3. Implement Phase 2 (Buffer Pool Integration)
4. Implement Phase 3 (WAL Streaming)
5. Implement Phase 4 (Configuration)
6. Testing and optimization

---

## References

- Amazon Aurora architecture (similar approach)
- Alibaba PolarDB architecture
- Neon PostgreSQL architecture
- InnoDB source code documentation

