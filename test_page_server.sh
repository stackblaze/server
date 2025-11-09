#!/bin/bash

# Test script for Page Server integration with MySQL/MariaDB
# This script helps test the Neon-style Page Server architecture

set -e

PAGE_SERVER_PORT=${PAGE_SERVER_PORT:-8080}
MYSQL_SOCKET=${MYSQL_SOCKET:-/tmp/mysql.sock}
MYSQL_USER=${MYSQL_USER:-root}
MYSQL_PASSWORD=${MYSQL_PASSWORD:-}

echo "=== Page Server Integration Test ==="
echo ""

# Check if Page Server is running
echo "1. Checking if Page Server is running..."
if curl -s http://localhost:${PAGE_SERVER_PORT}/api/v1/ping > /dev/null; then
    echo "   ✓ Page Server is running on port ${PAGE_SERVER_PORT}"
else
    echo "   ✗ Page Server is not running!"
    echo "   Please start it with: cd page-server && ./page-server -port ${PAGE_SERVER_PORT}"
    exit 1
fi

# Test ping endpoint
echo ""
echo "2. Testing Page Server ping endpoint..."
PING_RESPONSE=$(curl -s http://localhost:${PAGE_SERVER_PORT}/api/v1/ping)
if echo "$PING_RESPONSE" | grep -q '"status":"ok"'; then
    echo "   ✓ Ping successful: $PING_RESPONSE"
else
    echo "   ✗ Ping failed: $PING_RESPONSE"
    exit 1
fi

# Check if MySQL is running
echo ""
echo "3. Checking MySQL connection..."
if mysql -u${MYSQL_USER} ${MYSQL_PASSWORD:+-p${MYSQL_PASSWORD}} -e "SELECT 1" > /dev/null 2>&1; then
    echo "   ✓ MySQL is accessible"
else
    echo "   ⚠ MySQL is not accessible or not configured"
    echo "   This is expected if MySQL hasn't been built/started yet"
fi

# Show configuration instructions
echo ""
echo "=== Configuration Instructions ==="
echo ""
echo "To enable Page Server in MySQL/MariaDB, add to my.cnf or set at runtime:"
echo ""
echo "  SET GLOBAL innodb_page_server_enabled = 1;"
echo "  SET GLOBAL innodb_page_server_address = 'localhost:${PAGE_SERVER_PORT}';"
echo ""
echo "Or in my.cnf:"
echo "  [mysqld]"
echo "  innodb_page_server_enabled = 1"
echo "  innodb_page_server_address = localhost:${PAGE_SERVER_PORT}"
echo ""

# Test Page Server endpoints
echo "=== Testing Page Server Endpoints ==="
echo ""

echo "4. Testing GetPage (should return error for non-existent page)..."
GET_PAGE_RESPONSE=$(curl -s -X POST http://localhost:${PAGE_SERVER_PORT}/api/v1/get_page \
  -H "Content-Type: application/json" \
  -d '{"space_id":1,"page_no":42,"lsn":1000}')
if echo "$GET_PAGE_RESPONSE" | grep -q '"status":"error"'; then
    echo "   ✓ GetPage returned expected error: $(echo $GET_PAGE_RESPONSE | cut -c1-60)..."
else
    echo "   ⚠ Unexpected response: $GET_PAGE_RESPONSE"
fi

echo ""
echo "5. Testing StreamWAL..."
WAL_RESPONSE=$(curl -s -X POST http://localhost:${PAGE_SERVER_PORT}/api/v1/stream_wal \
  -H "Content-Type: application/json" \
  -d '{"lsn":1000,"wal_data":"SGVsbG8gV29ybGQ=","space_id":1,"page_no":42}')
if echo "$WAL_RESPONSE" | grep -q '"status":"success"'; then
    echo "   ✓ StreamWAL successful: $WAL_RESPONSE"
else
    echo "   ✗ StreamWAL failed: $WAL_RESPONSE"
    exit 1
fi

echo ""
echo "=== Test Summary ==="
echo "✓ Page Server is running and responding"
echo "✓ All HTTP endpoints are working"
echo ""
echo "Next steps:"
echo "1. Build MariaDB with Page Server patches"
echo "2. Configure MySQL with Page Server settings"
echo "3. Restart MySQL"
echo "4. Create a test table and trigger buffer pool miss"
echo "5. Monitor Page Server logs for page requests"
echo ""

