# Page Server HTTP API Documentation

## Overview

The Page Server provides a simple HTTP/JSON API for remote page storage and WAL streaming.

## Base URL

```
http://<host>:<port>/api/v1
```

Default port: `8080`

## Endpoints

### 1. Get Page

Fetch a single page from the Page Server.

**Endpoint:** `POST /api/v1/get_page`

**Request:**
```json
{
  "space_id": 1,
  "page_no": 42,
  "lsn": 1000
}
```

**Response (Success):**
```json
{
  "status": "success",
  "page_data": "base64_encoded_page_data",
  "page_lsn": 1000
}
```

**Response (Error):**
```json
{
  "status": "error",
  "error": "Page not found: space=1 page=42"
}
```

**Example with curl:**
```bash
curl -X POST http://localhost:8080/api/v1/get_page \
  -H "Content-Type: application/json" \
  -d '{"space_id":1,"page_no":42,"lsn":1000}'
```

---

### 2. Stream WAL

Stream a WAL (redo log) record to the Page Server.

**Endpoint:** `POST /api/v1/stream_wal`

**Request:**
```json
{
  "lsn": 1000,
  "wal_data": "base64_encoded_wal_data",
  "space_id": 1,
  "page_no": 42
}
```

**Response:**
```json
{
  "status": "success",
  "last_applied_lsn": 1000
}
```

**Example with curl:**
```bash
curl -X POST http://localhost:8080/api/v1/stream_wal \
  -H "Content-Type: application/json" \
  -d '{"lsn":1000,"wal_data":"SGVsbG8gV29ybGQ=","space_id":1,"page_no":42}'
```

---

### 3. Ping (Health Check)

Check if the Page Server is running.

**Endpoint:** `GET /api/v1/ping`

**Response:**
```json
{
  "status": "ok",
  "version": "1.0.0"
}
```

**Example with curl:**
```bash
curl http://localhost:8080/api/v1/ping
```

---

## Data Encoding

- **Page Data**: Binary page data is base64-encoded in JSON responses
- **WAL Data**: WAL records are base64-encoded in JSON requests
- **LSN**: Log Sequence Number (uint64)

## Error Handling

All endpoints return HTTP status codes:
- `200 OK` - Success
- `400 Bad Request` - Invalid request format
- `404 Not Found` - Page not found (for GetPage)
- `405 Method Not Allowed` - Wrong HTTP method
- `500 Internal Server Error` - Server error

Error responses include a JSON body with `status: "error"` and an `error` field describing the issue.

## Current Limitations

- In-memory storage only (not persistent)
- WAL application not yet implemented
- No authentication/authorization
- No versioning (always returns latest page)

## Future Enhancements

- Persistent storage backend (S3/MinIO)
- WAL application to pages
- Page versioning with LSN
- Batch operations
- Authentication/authorization
- TLS/HTTPS support

