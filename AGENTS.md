# AGENTS.md - Codebase Guide for AI Agents

## Build Commands

### Primary Build
```bash
# Build the full server with PostgreSQL, JWT, and CURL support
g++ -std=c++17 -o bookstore-server src/main.cpp -lpq -ljwt -lcurl -lssl -lcrypto

# Or use the build script
./build.sh
```

### Database Setup
```bash
# Initialize PostgreSQL database (requires postgres access)
sudo -u postgres psql -f scripts/init_database.sql

# Database name: bookstore_db
# User: bookstore_user, Password: bookstore_password
```

### Running the Server
```bash
./bookstore-server

# Server runs on port 4000
# GraphQL Playground: http://localhost:4000/
# GraphQL Endpoint: http://localhost:4000/graphql
```

### Testing Commands

```bash
# Run full test suite (requires server running)
./test_api.sh

# Test with custom API URL
API_URL=http://localhost:4000/graphql ./test_api.sh

# Test Docker container
chmod +x test_api.sh
API_URL=http://localhost:4001/graphql ./test_api.sh
```

### Test File Format (Avoid Bash Escaping)
Always use files for JSON payloads to avoid bash escaping issues:

```bash
# Create test file
cat > /tmp/test_login.json << 'EOF'
{"query":"mutation { login(username: \"admin\", password: \"password123\") { success token } }"}
EOF

# Use file input
curl -X POST http://localhost:4000/graphql \
  -H 'Content-Type: application/json' \
  --data-binary @/tmp/test_login.json
```

### Test Results
The test script checks:
1. ✓ Server health / GraphQL Playground
2. ✓ User registration
3. ✓ User login
4. ✓ Books query (no auth)
5. ✓ 'me' query (with auth)
6. ✓ SSRF endpoint
7. ✓ BOLA vulnerability (intentional)
8. ✓ SQL Injection endpoint
9. ✓ Admin endpoints (intentional - no auth)
10. ✓ Complex nested queries
11. ✓ GraphQL introspection
12. ✓ Mass Assignment vulnerability
13. ✓ IDOR vulnerabilities
14. ✓ CORS headers

**Exit Codes:**
- `0` = All tests passed
- `1` = Some tests failed

### GitHub Actions
The workflow (`.github/workflows/docker-build.yml`) runs the full test suite against the Docker container before pushing to Docker Hub and triggering Render deployment.

### Clean Up
```bash
# Kill server on port 4000
pkill -f bookstore-server

# Or kill port 4000
lsof -ti:4000 | xargs kill -9
```

### Docker Deployment
```bash
# Build and run with docker-compose
docker-compose up --build

# Server will be available at http://localhost:4000
# Database runs automatically in container

# Stop containers
docker-compose down

# Rebuild after code changes
docker-compose up --build --force-recreate
```

### Environment Variables
The following environment variables can be set to configure the server:
- `PORT` - Server port (default: 4000)
- `JWT_SECRET` - JWT signing secret (default: hardcoded weak secret)
- `DB_CONNECTION_STRING` - PostgreSQL connection string

Example:
```bash
export PORT=4000
export JWT_SECRET="your-secret-here"
export DB_CONNECTION_STRING="dbname=bookstore_db user=bookstore_user password=bookstore_password host=localhost port=5432"
./bookstore-server
```

### GitHub Actions
The repository includes a GitHub Actions workflow (`.github/workflows/docker-build.yml`) that:
- Builds the Docker image on push to main/master branches
- Builds and tags releases on version tags
- Pushes to Docker Hub (requires `DOCKER_USERNAME` and `DOCKER_PASSWORD` secrets)

Required GitHub Secrets:
- `DOCKER_USERNAME` - Docker Hub username
- `DOCKER_PASSWORD` - Docker Hub password/access token

Push to Docker Hub happens automatically on:
- Push to `main` or `master` branch
- New version tags (e.g., `v1.0.0`)

## Code Style Guidelines

### Language & Standards
- Language: C++17
- Compiler: g++
- No external web frameworks (raw socket HTTP server)

### Naming Conventions
- **Defines**: `UPPER_CASE_WITH_UNDERSCORES` (e.g., `PORT`, `BUFFER_SIZE`, `JWT_SECRET`)
- **Structs**: `PascalCase` (e.g., `User`, `Book`, `Order`)
- **Struct Members**: `camelCase` (e.g., `username`, `passwordHash`, `firstName`)
- **Functions**: `camelCase` (e.g., `loadUsersCache`, `handleQuery`, `escapeJson`)
- **Variables**: `camelCase` (e.g., `cartId`, `targetOrderId`, `paramValues`)
- **Global Variables**: `camelCase` with descriptive names (e.g., `usersCache`, `booksCache`)

### Import/Include Order
System headers first, then third-party libs:
```cpp
#include <iostream>
#include <string>
#include <vector>
#include <regex>
#include <ctime>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <postgresql/libpq-fe.h>
#include <jwt.h>
#include <curl/curl.h>
```

### Type Usage
- Use `string` for text fields
- Use `int` for numeric IDs (books, reviews, cart items)
- Use `string` for UUID IDs (users, orders, webhooks)
- Use `double` for prices/monetary values
- Use `bool` for flags
- Use `map<K, V>` for caches keyed by ID
- Use `vector<T>` for lists

### Function Return Types
- `string` for JSON responses
- `bool` for success/failure operations
- `T*` for pointer returns (e.g., `User* getUserByUsername()`)
- `void` for procedures with no return

### Error Handling
- Database operations: Check `PQresultStatus(res)` against `PGRES_TUPLES_OK` or `PGRES_COMMAND_OK`
- JWT operations: Check return codes, return empty objects on failure
- CURL operations: Check `CURLcode res == CURLE_OK`
- Always `PQclear(res)` after use
- Return meaningful error messages in JSON responses

### JSON Building Pattern
Use `stringstream` for building responses:
```cpp
stringstream ss;
ss << "{";
ss << "\"id\":\"" << id << "\",";
ss << "\"username\":\"" << escapeJson(username) << "\"";
ss << "}";
return ss.str();
```

### Database Query Pattern
Use parameterized queries (when not intentionally vulnerable):
```cpp
string sql = "SELECT id, name FROM table WHERE id = $1";
const char* paramValues[1] = {value.c_str()};
PGresult* res = PQexecParams(dbConn, sql.c_str(), 1, nullptr, paramValues, nullptr, nullptr, 0);
// Process result
PQclear(res);
```

### Debug Output
- Use `cerr << "[DEBUG] message"` for debug logging
- Debug messages should help track flow and identify issues
- Example: `cerr << "[DEBUG] Insert successful" << endl;`

### Project Structure
```
src/main.cpp           # Main server (~1700 lines)
scripts/init_database.sql  # Database schema and seed data
build.sh              # Build script
test_api.sh           # Integration tests
docker-compose.yml     # Docker deployment
Dockerfile            # Container image
```

### Vulnerability Implementation Notes
- Vulnerabilities are INTENTIONAL and realistic
- Follow existing patterns for new vulnerabilities
- No "backdoors" - use subtle implementation errors
- Use `{"name": "_internalName"}` pattern for internal/debug endpoints
- Document vulnerabilities in VULNERABILITIES.md

### Important Constants
```cpp
PORT 4000
JWT_SECRET "CHANGE_ME_IN_PRODUCTION_real_jwt_secret_key_2024"
DB_CONN "dbname=bookstore_db user=bookstore_user password=bookstore_password host=localhost port=5432"
```

### Default Credentials
| Username | Password | Role |
|----------|----------|-------|
| admin    | password123 | admin |
| staff    | password123 | staff |
| user     | password123 | user |

### GraphQL Request Handling
1. Parse Authorization header to extract JWT token
2. Verify JWT with `verifyJWT()`, get User object
3. Parse query string from POST body
4. Check for "mutation" or "query" keyword
5. Route to `handleMutation()` or `handleQuery()`
6. Return JSON response wrapped in `{"data":{...}}`

### Adding New Queries/Mutations
1. Add struct for new data type if needed
2. Add cache variable: `map<K, T> newCache;`
3. Add `loadNewCache()` function
4. Call `loadNewCache()` in `main()` after db connect
5. Add handler in `handleQuery()` or `handleMutation()`
6. Add introspection entry in `__schema` query handler (NO descriptions!)
7. Add JSON builder function if needed: `TToJson(const T& t)`

### Comment Policy
- DO NOT add code comments
- Keep code concise and self-explanatory
- Comments in documentation only (README, VULNERABILITIES.md)

### Available Queries
| Query | Description | Auth Required |
|-------|-------------|---------------|
| `me` | Get current authenticated user | Yes |
| `books` | List all books with optional search and category filter | No |
| `book(id)` | Get a specific book by ID | No |
| `cart` | Get user's shopping cart | Yes |
| `orders` | Get user's orders | Yes |
| `bookReviews(bookId)` | Get reviews for a specific book | No |
| `myReviews` | Get current user's reviews | Yes |
| `webhooks` | Get user's registered webhooks | Yes |
| `_internalUserSearch(username)` | Search users by username pattern (BOLA) | No |
| `_fetchExternalResource(url)` | Fetch external resource by URL (SSRF) | No |
| `_searchAdvanced(query)` | Advanced search with SQL injection vulnerability | No |
| `_adminStats` | Admin statistics (no auth required!) | No |
| `_adminAllOrders` | All orders (no auth required!) | No |
| `_adminAllPayments` | All payment transactions (no auth required!) | No |

### Available Mutations
| Mutation | Description | Auth Required |
|----------|-------------|---------------|
| `register(username, firstName, lastName, password)` | Register a new user | No |
| `login(username, password)` | Login and get JWT token | No |
| `updateProfile(...)` | Update user profile (mass assignment vulnerability) | Yes |
| `addToCart(bookId, quantity)` | Add item to shopping cart | Yes |
| `removeFromCart(bookId)` | Remove item from shopping cart | Yes |
| `createOrder()` | Create order from cart | Yes |
| `cancelOrder(orderId)` | Cancel an order (IDOR vulnerability) | Yes |
| `createReview(bookId, rating, comment)` | Create a review | Yes |
| `deleteReview(reviewId)` | Delete a review (IDOR vulnerability) | Yes |
| `registerWebhook(url, events, secret)` | Register a webhook URL | Yes |
| `testWebhook(webhookId)` | Test a webhook (SSRF vulnerability) | Yes |

### Recent Features Added
- **Shopping Cart System**: Full cart functionality with add/remove items
- **Order Management**: Create orders from cart, cancel orders with IDOR vulnerability
- **Review System**: Create and delete reviews with IDOR vulnerability
- **Webhook System**: Register webhooks with SSRF via testWebhook
- **Admin Queries**: Stats, orders, and payments accessible without auth
- **SQL Injection**: `_searchAdvanced` query with direct SQL concatenation

### Testing New Features
```bash
# Test cart with authentication
cat > /tmp/test_login.json << 'EOF'
{"query":"mutation { login(username: \"admin\", password: \"password123\") { token } }"}
EOF
TOKEN=$(curl -s -X POST http://localhost:4000/graphql \
  -H 'Content-Type: application/json' \
  --data-binary @/tmp/test_login.json | grep -oP '"token":"[^"]+' | cut -d'"' -f4)

cat > /tmp/test_cart.json << 'EOF'
{"query":"mutation { addToCart(bookId: 1, quantity: 2) { success message } }"}
EOF
curl -X POST http://localhost:4000/graphql \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  --data-binary @/tmp/test_cart.json

# Test order creation
cat > /tmp/test_order.json << 'EOF'
{"query":"mutation { createOrder { success orderId totalAmount } }"}
EOF
curl -X POST http://localhost:4000/graphql \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  --data-binary @/tmp/test_order.json

# Test admin queries (no auth required!)
cat > /tmp/test_admin.json << 'EOF'
{"query":"query { _adminStats { userCount bookCount totalRevenue } }"}
EOF
curl -X POST http://localhost:4000/graphql \
  -H 'Content-Type: application/json' \
  --data-binary @/tmp/test_admin.json

# Test SQL injection
cat > /tmp/test_sql.json << 'EOF'
{"query":"query { _searchAdvanced(query: \"1 OR 1=1\") { id title } }"}
EOF
curl -X POST http://localhost:4000/graphql \
  -H 'Content-Type: application/json' \
  --data-binary @/tmp/test_sql.json
```

### SSRF URL Whitelist
Allowed prefixes for `_fetchExternalResource`:
- `http://example.com`
- `http://httpbin.org`
- `http://api.github.com`, `https://api.github.com`
- `http://169.254.169.254` (cloud metadata)
- `http://localhost:`, `http://127.0.0.1:`

### Debug Output
- Debug logging enabled: `cerr << "[DEBUG] message"`
- Shows query parsing, SQL execution status, and responses
- Useful for tracking flow and identifying issues
- Example: `cerr << "[DEBUG] Processing request - isMutation: " << (isMutation ? "true" : "false") << endl;`

---

## CRITICAL: GraphQL Query Parsing Guidelines

### The Backslash-Escaped Quote Problem

When bash receives curl commands with `\"` inside single-quoted JSON, bash adds additional backslashes. For example:

```bash
# What you TYPE:
-d '{"query":"mutation { login(username: \"admin\", password: \"password123\") { success } }"}'

# What the SERVER receives:
{"query":"mutation { login(username: \\"admin\\", password: \\"password123\\") { success } }"}
```

The `\"` becomes `\\"` - the backslash is LITERAL in the string.

### Correct extractValue() Implementation

This is the CORRECT implementation that handles escaped quotes:

```cpp
string extractValue(const string& query, const string& key) {
    string searchKey = key + ":";
    size_t keyPos = query.find(searchKey);
    if (keyPos == string::npos) return "";
    
    size_t searchStart = keyPos + searchKey.length();
    
    // Skip whitespace
    while (searchStart < query.length() && 
           (query[searchStart] == ' ' || query[searchStart] == '\t')) {
        searchStart++;
    }
    
    if (searchStart >= query.length()) return "";
    
    // Skip opening quote (may be escaped with backslash like \")
    if (query[searchStart] == '"') {
        searchStart++;
    } else if (query[searchStart] == '\\' && 
               searchStart + 1 < query.length() && 
               query[searchStart + 1] == '"') {
        // Skip escaped quote: \"
        searchStart += 2;
    }
    
    string value;
    bool escaped = false;
    
    for (size_t i = searchStart; i < query.length(); i++) {
        char c = query[i];
        
        if (escaped) {
            // If we're escaped and see a quote, it's an escaped quote - skip it
            if (c != '"') {
                value += c;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            // End of string
            return value;
        } else if (c == ' ' || c == ',' || c == ')' || c == '{' || c == '}') {
            // End of value (unquoted)
            return value;
        } else {
            value += c;
        }
    }
    
    return value;
}
```

### Key Points:
1. Check for both `"` (unescaped) AND `\"` (escaped) as opening quotes
2. When `escaped=true`, a `"` means an escaped quote - skip it, don't add to value
3. Only return when you hit an UNESCAPED closing quote
4. Handle whitespace, commas, parentheses, and braces as value delimiters

### Testing Query Parsing

ALWAYS test with debug logging enabled:

```cpp
cerr << "[DEBUG] Raw body: " << body << endl;
cerr << "[DEBUG] Extracted query: " << queryStr << endl;
cerr << "[DEBUG] username='" << username << "', password='" << password << "'" << endl;
```

If you see `username='"admin"'` (with quotes included), the parsing is broken.

### Never Use Regex for This

Regex is fragile with escaped strings. Use the simple character-by-character parsing shown above.

### If You Break This Again...

Symptoms to watch for:
- `{"data":{"login":{"success":false,"message":"Missing required fields: username, password"}}}`
- Logs show `username='"admin"'` or `username=''`
- Server receives `\\"` in the raw body

Fix: Use the extractValue() implementation above.

---

## Fly.io Deployment

### Why Fly.io?
- Faster cold starts than Render
- Better performance for low-traffic apps
- Generous free tier
- Docker-based deployments

### Initial Setup

```bash
# Install flyctl
curl -L https://fly.io/install.sh | sh
export FLYCTL_INSTALL="$HOME/.fly"
export PATH="$FLYCTL_INSTALL/bin:$PATH"

# Authenticate
flyctl auth login

# Launch app (creates fly.toml and deploys)
./deploy-fly.sh
```

### Manual Deployment
```bash
# Deploy to Fly.io
fly deploy

# Check status
fly status

# View logs
fly logs

# Open app
fly open
```

### Environment Variables
Set these in Fly.io dashboard or via CLI:
- `DATABASE_URL`: Neon PostgreSQL connection string
- `JWT_SECRET`: JWT signing secret

### Scale Down (Free Tier)
```bash
# Scale to 0 machines when not in use (saves credits)
fly scale count 0

# Scale back up
fly scale count 1
```

### Troubleshooting
```bash
# Check deployed machines
fly machine list

# Restart a machine
fly machine restart <machine-id>

# Check connection to database
fly ssh console
psql "$DATABASE_URL" -c "SELECT 1"
```
