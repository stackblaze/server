# Page Server Implementation

This is a minimal Page Server implementation for the Neon-style InnoDB architecture.

## Overview

The Page Server is a remote service that:
- Stores page images at different LSN versions
- Receives and applies WAL (redo log) records
- Serves pages to stateless MySQL compute nodes on demand

## Architecture

```
MySQL Compute Node
     |
     | RPC: GetPage(space_id, page_no, lsn)
     ↓
Page Server (this service)
     |
     | Background: Apply WAL, merge pages
     ↓
Object Storage (S3/MinIO - future)
```

## Building

```bash
cd page-server
chmod +x build.sh
./build.sh
```

Or manually:
```bash
go build -o page-server main.go
```

## Running

```bash
./page-server -port 8080
```

## Protocol

The Page Server uses **HTTP/JSON** for simplicity and fast iteration. See `API.md` for complete API documentation.

**Endpoints:**
- `POST /api/v1/get_page` - Fetch a page
- `POST /api/v1/stream_wal` - Stream WAL record
- `GET /api/v1/ping` - Health check

## Current Implementation Status

This is a **minimal prototype** that demonstrates:
- ✅ HTTP/JSON server
- ✅ GetPage endpoint
- ✅ StreamWAL endpoint
- ✅ Ping health check
- ✅ Base64 encoding for binary data

**Not yet implemented:**
- ❌ Persistent storage (currently in-memory only)
- ❌ WAL application to pages
- ❌ Page versioning with LSN
- ❌ Object storage backend (S3/MinIO)
- ❌ Page cache/eviction
- ❌ Authentication/authorization
- ❌ Production-grade error handling

## Next Steps

1. Implement persistent page storage
2. Implement WAL application logic
3. Add object storage backend
4. Add authentication
5. Add monitoring/metrics
6. Performance optimization

## Testing

```bash
# Start server
./page-server -port 8080

# In another terminal, test with curl
# Ping
curl http://localhost:8080/api/v1/ping

# Get page
curl -X POST http://localhost:8080/api/v1/get_page \
  -H "Content-Type: application/json" \
  -d '{"space_id":1,"page_no":42,"lsn":1000}'

# Stream WAL
curl -X POST http://localhost:8080/api/v1/stream_wal \
  -H "Content-Type: application/json" \
  -d '{"lsn":1000,"wal_data":"SGVsbG8gV29ybGQ="}'
```

See `API.md` for detailed API documentation.

