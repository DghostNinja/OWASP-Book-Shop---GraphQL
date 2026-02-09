#!/bin/bash

echo "=========================================="
echo "  Vulnerable GraphQL API - Test Suite  "
echo "=========================================="
echo ""

API_URL="${API_URL:-http://localhost:4000/graphql}"
PASS_COUNT=0
FAIL_COUNT=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Helper function to create test files
create_test_file() {
    local filename="$1"
    local content="$2"
    echo "$content" > "$filename"
}

# Cleanup function
cleanup() {
    rm -f /tmp/test_*.json 2>/dev/null
}
trap cleanup EXIT

echo "API URL: $API_URL"
echo ""

#==========================================
# TEST 1: Server Health Check
#==========================================
echo "1. Testing Server Health..."
RESPONSE=$(curl -s "${API_URL}/" 2>/dev/null)
if echo "$RESPONSE" | grep -q "GraphQL Playground"; then
    echo -e "   ${GREEN}✓${NC} Playground accessible"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} Playground not accessible"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 2: User Registration
#==========================================
echo "2. Testing User Registration..."
create_test_file /tmp/test_register.json '{"query":"mutation { register(username: \"testuser\", firstName: \"Test\", lastName: \"User\", password: \"testpass123\") { success message token user { id username } } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_register.json)
if echo "$RESPONSE" | grep -q '"success":true'; then
    echo -e "   ${GREEN}✓${NC} Registration successful"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} Registration failed"
    echo "   Response: $RESPONSE"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 3: User Login
#==========================================
echo "3. Testing User Login (admin)..."
create_test_file /tmp/test_login.json '{"query":"mutation { login(username: \"admin\", password: \"password123\") { success token user { id username role } } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_login.json)

if echo "$RESPONSE" | grep -q '"success":true'; then
    echo -e "   ${GREEN}✓${NC} Login successful"
    ((PASS_COUNT++))
    ADMIN_TOKEN=$(echo "$RESPONSE" | grep -oP '"token":"[^"]+' | cut -d'"' -f4)
    echo "   Token: ${ADMIN_TOKEN:0:30}..."
else
    echo -e "   ${RED}✗${NC} Login failed"
    echo "   Response: $RESPONSE"
    ((FAIL_COUNT++))
    ADMIN_TOKEN=""
fi
echo ""

#==========================================
# TEST 4: Books Query (No Auth)
#==========================================
echo "4. Testing Books Query (No Auth)..."
create_test_file /tmp/test_books.json '{"query":"query { books { id title price stockQuantity } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_books.json)
if echo "$RESPONSE" | grep -q '"books":'; then
    BOOK_COUNT=$(echo "$RESPONSE" | grep -o '"id"' | wc -l)
    echo -e "   ${GREEN}✓${NC} Books query works ($BOOK_COUNT books found)"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} Books query failed"
    echo "   Response: $RESPONSE"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 5: Get User Info (me query)
#==========================================
echo "5. Testing 'me' Query (With Auth)..."
if [ -n "$ADMIN_TOKEN" ]; then
    create_test_file /tmp/test_me.json '{"query":"query { me { id username role } }"}'
    RESPONSE=$(curl -s -X POST "$API_URL" \
        -H 'Content-Type: application/json' \
        -H "Authorization: Bearer $ADMIN_TOKEN" \
        --data-binary @/tmp/test_me.json)
    if echo "$RESPONSE" | grep -q '"username":"admin"'; then
        echo -e "   ${GREEN}✓${NC} 'me' query works with auth"
        ((PASS_COUNT++))
    else
        echo -e "   ${RED}✗${NC} 'me' query failed"
        echo "   Response: $RESPONSE"
        ((FAIL_COUNT++))
    fi
else
    echo -e "   ${YELLOW}⊘${NC} Skipped (no token)"
fi
echo ""

#==========================================
# TEST 6: SSRF Vulnerability
#==========================================
echo "6. Testing SSRF Vulnerability (_fetchExternalResource)..."
create_test_file /tmp/test_ssrf.json '{"query":"query { _fetchExternalResource(url: \"http://example.com\") }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_ssrf.json)
if echo "$RESPONSE" | grep -q '_fetchExternalResource'; then
    echo -e "   ${GREEN}✓${NC} SSRF endpoint accessible"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} SSRF endpoint failed"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 7: BOLA Vulnerability
#==========================================
echo "7. Testing BOLA Vulnerability (_internalUserSearch)..."
create_test_file /tmp/test_bola.json '{"query":"query { _internalUserSearch(username: \"\") { id username passwordHash } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_bola.json)
USER_COUNT=$(echo "$RESPONSE" | grep -o '"username"' | wc -l)
if [ "$USER_COUNT" -gt 0 ]; then
    echo -e "   ${YELLOW}⚠${NC} BOLA vulnerability exposed ($USER_COUNT users leaked)"
    echo "   Sample: $(echo "$RESPONSE" | head -c 150)..."
    ((PASS_COUNT++))  # This is intentional
else
    echo -e "   ${GREEN}✓${NC} BOLA protected"
    ((FAIL_COUNT++))  # This should leak
fi
echo ""

#==========================================
# TEST 8: SQL Injection
#==========================================
echo "8. Testing SQL Injection (_searchAdvanced)..."
create_test_file /tmp/test_sql.json '{"query":"query { _searchAdvanced(query: \"SQL\") { id title } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_sql.json)
if echo "$RESPONSE" | grep -q '"_searchAdvanced":'; then
    echo -e "   ${GREEN}✓${NC} SQL Injection endpoint accessible"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} SQL Injection endpoint failed"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 9: Admin Endpoints (No Auth)
#==========================================
echo "9. Testing Admin Endpoints (No Auth Required)..."

# _adminStats
create_test_file /tmp/test_admin_stats.json '{"query":"query { _adminStats { userCount bookCount orderCount } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_admin_stats.json)
if echo "$RESPONSE" | grep -q '"userCount":'; then
    echo -e "   ${GREEN}✓${NC} _adminStats exposed (intentional)"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} _adminStats protected (unexpected)"
    ((FAIL_COUNT++))
fi

# _adminAllOrders
create_test_file /tmp/test_admin_orders.json '{"query":"query { _adminAllOrders { id orderNumber totalAmount } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_admin_orders.json)
if echo "$RESPONSE" | grep -q '"_adminAllOrders"'; then
    echo -e "   ${GREEN}✓${NC} _adminAllOrders exposed (intentional)"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} _adminAllOrders protected (unexpected)"
    ((FAIL_COUNT++))
fi

# _adminAllPayments
create_test_file /tmp/test_admin_payments.json '{"query":"query { _adminAllPayments { id amount transactionId } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_admin_payments.json)
if echo "$RESPONSE" | grep -q '"_adminAllPayments"'; then
    echo -e "   ${GREEN}✓${NC} _adminAllPayments exposed (intentional)"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} _adminAllPayments protected (unexpected)"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 10: Complex Nested Query
#==========================================
echo "10. Testing Complex Nested Query..."
create_test_file /tmp/test_complex.json '{"query":"query { books { id title price author { id name } reviews { id rating comment } } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_complex.json)
if echo "$RESPONSE" | grep -q '"books":'; then
    echo -e "   ${GREEN}✓${NC} Complex nested query works"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} Complex nested query failed"
    echo "   Response: $RESPONSE"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 11: GraphQL Introspection
#==========================================
echo "11. Testing GraphQL Introspection..."
create_test_file /tmp/test_introspection.json '{"query":"query { __schema { queryType { name } } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_introspection.json)
if echo "$RESPONSE" | grep -q '"__schema":'; then
    echo -e "   ${GREEN}✓${NC} Introspection works"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} Introspection failed"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# TEST 12: Mass Assignment (if logged in)
#==========================================
echo "12. Testing Mass Assignment (updateProfile)..."
if [ -n "$ADMIN_TOKEN" ]; then
    create_test_file /tmp/test_mass_assign.json '{"query":"mutation { updateProfile(firstName: \"Hacked\", role: \"admin\") { id username role firstName } }"}'
    RESPONSE=$(curl -s -X POST "$API_URL" \
        -H 'Content-Type: application/json' \
        -H "Authorization: Bearer $ADMIN_TOKEN" \
        --data-binary @/tmp/test_mass_assign.json)
    if echo "$RESPONSE" | grep -q '"role":"admin"'; then
        echo -e "   ${YELLOW}⚠${NC} Mass Assignment vulnerability (role changed to admin!)"
        ((PASS_COUNT++))  # Intentional vulnerability
    elif echo "$RESPONSE" | grep -q '"firstName":"Hacked"'; then
        echo -e "   ${GREEN}✓${NC} Profile updated but role protected"
        ((PASS_COUNT++))
    else
        echo -e "   ${YELLOW}⊘${NC} Update may have failed"
        echo "   Response: $RESPONSE"
    fi
else
    echo -e "   ${YELLOW}⊘${NC} Skipped (no token)"
fi
echo ""

#==========================================
# TEST 13: IDOR - Cancel Order
#==========================================
echo "13. Testing IDOR (cancelOrder)..."
create_test_file /tmp/test_login_user.json '{"query":"mutation { login(username: \"user\", password: \"password123\") { token } }"}'
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/test_login_user.json)
USER_TOKEN=$(echo "$RESPONSE" | grep -oP '"token":"[^"]+' | cut -d'"' -f4)

if [ -n "$USER_TOKEN" ]; then
    create_test_file /tmp/test_cancel_order.json '{"query":"mutation { cancelOrder(orderId: \"order-nonexistent-123\") { success message } }"}'
    RESPONSE=$(curl -s -X POST "$API_URL" \
        -H 'Content-Type: application/json' \
        -H "Authorization: Bearer $USER_TOKEN" \
        --data-binary @/tmp/test_cancel_order.json)
    if echo "$RESPONSE" | grep -q '"Order not found"' || echo "$RESPONSE" | grep -q '"success":false'; then
        echo -e "   ${GREEN}✓${NC} IDOR protected (order not found)"
        ((PASS_COUNT++))
    else
        echo -e "   ${YELLOW}⚠${NC} IDOR may be possible"
        echo "   Response: $RESPONSE"
    fi
else
    echo -e "   ${YELLOW}⊘${NC} Skipped (could not login)"
fi
echo ""

#==========================================
# TEST 14: CORS Headers
#==========================================
echo "14. Testing CORS Headers..."
RESPONSE=$(curl -s -X POST "$API_URL" \
    -H 'Content-Type: application/json' \
    -H 'Origin: http://example.com' \
    --data-binary @/tmp/test_books.json \
    -D - 2>/dev/null | grep -i "access-control-allow-origin")
if echo "$RESPONSE" | grep -q "Access-Control-Allow-Origin"; then
    echo -e "   ${GREEN}✓${NC} CORS headers present"
    ((PASS_COUNT++))
else
    echo -e "   ${RED}✗${NC} CORS headers missing"
    ((FAIL_COUNT++))
fi
echo ""

#==========================================
# Summary
#==========================================
echo "=========================================="
echo "  Test Summary                          "
echo "=========================================="
echo -e "  ${GREEN}Passed:${NC} $PASS_COUNT"
echo -e "  ${RED}Failed:${NC} $FAIL_COUNT"
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}=========================================="
    echo "  ALL TESTS PASSED - READY FOR DEPLOYMENT!"
    echo -e "==========================================${NC}"
    exit 0
else
    echo -e "${YELLOW}=========================================="
    echo "  SOME TESTS FAILED - REVIEW OUTPUT ABOVE"
    echo -e "==========================================${NC}"
    exit 1
fi
