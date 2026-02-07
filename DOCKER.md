# Docker Deployment Guide

## Quick Start

### Using Docker Compose (Recommended)

```bash
# Build and start all services
docker-compose up --build

# Run in detached mode
docker-compose up -d --build

# View logs
docker-compose logs -f

# Stop all services
docker-compose down

# Stop and remove volumes
docker-compose down -v
```

### Using Docker Build Directly

```bash
# Build the Docker image
docker build -t vulnerable-graphql-bookstore:latest .

# Run the container
docker run -p 4000:4000 \
  -e DB_CONNECTION_STRING="dbname=bookstore_db user=bookstore_user password=bookstore_password host=host.docker.internal port=5432" \
  vulnerable-graphql-bookstore:latest
```

## Environment Variables

The Docker image supports the following environment variables:

| Variable | Description | Default |
|----------|-------------|----------|
| `PORT` | Server port | `4000` |
| `JWT_SECRET` | JWT signing secret | `CHANGE_ME_IN_PRODUCTION_real_jwt_secret_key_2024` |
| `DB_CONNECTION_STRING` | PostgreSQL connection string | `dbname=bookstore_db user=bookstore_user password=bookstore_password host=localhost port=5432` |

### Example with Custom Configuration

```bash
docker run -p 4000:4000 \
  -e PORT=8080 \
  -e JWT_SECRET="my-custom-secret" \
  -e DB_CONNECTION_STRING="dbname=bookstore_db user=bookstore_user password=bookstore_password host=postgres port=5432" \
  vulnerable-graphql-bookstore:latest
```

## Services

### PostgreSQL

- **Image**: `postgres:16`
- **Database**: `bookstore_db`
- **User**: `bookstore_user`
- **Password**: `bookstore_password`
- **Port**: `5432` (host), `5432` (container)
- **Health check**: Every 10 seconds

### API

- **Build**: From `Dockerfile` in current directory
- **Exposed ports**: `4000:4000`
- **Dependencies**: Waits for PostgreSQL to be healthy
- **Environment**: Configured via docker-compose

## Connecting to Database

### From Host Machine

```bash
psql -h localhost -p 5432 -U bookstore_user -d bookstore_db
# Password: bookstore_password
```

### From Another Container

```bash
docker exec -it bookstore-postgres psql -U bookstore_user -d bookstore_db
```

## Troubleshooting

### Database Connection Issues

```bash
# Check if PostgreSQL is running
docker-compose ps postgres

# View PostgreSQL logs
docker-compose logs postgres

# Restart services
docker-compose restart postgres
```

### API Issues

```bash
# Check if API is running
docker-compose ps api

# View API logs
docker-compose logs api

# Restart API
docker-compose restart api

# Rebuild from scratch
docker-compose down
docker-compose up --build --force-recreate
```

### Port Conflicts

If port 4000 is already in use:

```bash
# Change port in docker-compose.yml
ports:
  - "8080:4000"  # Use port 8080 instead
```

### Database Initialization Issues

```bash
# Reinitialize database
docker-compose down -v  # Remove volumes
docker-compose up --build  # Start fresh
```

## GitHub Actions CI/CD

### Workflow Triggers

The GitHub Actions workflow (`.github/workflows/docker-build.yml`) automatically:

1. **On push** to `main` or `master` branches
   - Builds Docker image
   - Tags as `latest` and `<branch-name>`
   - Pushes to Docker Hub

2. **On version tags** (e.g., `v1.0.0`)
   - Builds Docker image
   - Tags as `v1.0.0`, `1.0.0`, `1.0`, `1`, `latest`
   - Pushes to Docker Hub

3. **On pull requests**
   - Builds Docker image
   - Runs tests
   - Does NOT push to Docker Hub

### Setting Up Secrets

1. Go to your repository on GitHub
2. Navigate to: Settings → Secrets and variables → Actions
3. Click "New repository secret"
4. Add the following secrets:

| Secret | Description |
|--------|-------------|
| `DOCKER_USERNAME` | Your Docker Hub username |
| `DOCKER_PASSWORD` | Docker Hub password or access token |

### Manually Triggering Build

```bash
# Tag a commit to trigger release build
git tag v1.0.0
git push origin v1.0.0
```

### Pulling from Docker Hub

```bash
# Pull latest image
docker pull vulnerable-graphql-bookstore:latest

# Pull specific version
docker pull vulnerable-graphql-bookstore:v1.0.0

# Run pulled image
docker run -p 4000:4000 vulnerable-graphql-bookstore:latest
```

## Production Considerations

**This is a deliberately vulnerable API. DO NOT USE IN PRODUCTION.**

For a production deployment, you would need to:

1. **Security hardening**
   - Fix all OWASP API Security Top 10 vulnerabilities
   - Use environment-specific secrets
   - Enable HTTPS/TLS

2. **Database security**
   - Use strong passwords
   - Enable SSL/TLS for database connections
   - Regular backups

3. **API security**
   - Rate limiting
   - Query depth limits
   - Request size limits
   - Input validation and sanitization
   - Disable introspection in production

4. **Infrastructure**
   - Use multiple replicas for high availability
   - Load balancer
   - Health checks
   - Logging and monitoring
   - Secrets management (e.g., AWS Secrets Manager, HashiCorp Vault)

5. **Docker best practices**
   - Use non-root user in containers
   - Scan images for vulnerabilities
   - Use multi-stage builds for smaller images
   - Implement resource limits
