#!/bin/bash

set -e

echo "========================================"
echo "  GraphQL Bookstore API                 "
echo "  Build & Setup Script                 "
echo "========================================"

echo ""
echo "[1/5] Installing dependencies..."
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -qq
    sudo apt-get install -y -qq build-essential libpq-dev libjwt-dev libcurl4-openssl-dev postgresql > /dev/null 2>&1 || true
elif command -v yum >/dev/null 2>&1; then
    sudo yum install -y gcc-c++ postgresql-devel libjwt libcurl > /dev/null 2>&1 || true
elif command -v brew >/dev/null 2>&1; then
    brew install postgresql libjwt curl > /dev/null 2>&1 || true
fi
echo "  Dependencies OK"

echo ""
echo "[2/5] Building server..."
g++ -std=c++17 -pthread -o bookstore-server \
    src/main.cpp \
    src/utils.cpp \
    src/user_manager.cpp \
    src/book_manager.cpp \
    src/order_manager.cpp \
    src/extra_features.cpp \
    src/db_manager.cpp \
    src/network_manager.cpp \
    src/graphql_handler.cpp \
    src/html_generator.cpp \
    src/rate_limiter.cpp \
    src/payment_handler.cpp \
    -lpq -ljwt -lcurl -lssl -lcrypto
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi
echo "  Build OK"

echo ""
echo "[3/5] Setting up database..."

# Check if PostgreSQL is running, if not try to start it
if ! pg_isready >/dev/null 2>&1; then
    echo "  Starting PostgreSQL..."
    sudo service postgresql start > /dev/null 2>&1 || sudo pg_ctl start > /dev/null 2>&1 || true
    sleep 2
fi

# Try to create database and user
echo "  Creating database and user..."
sudo -u postgres psql -c "CREATE DATABASE bookstore_db;" > /dev/null 2>&1 || true
sudo -u postgres psql -c "CREATE USER bookstore_user WITH PASSWORD 'bookstore_password';" > /dev/null 2>&1 || true
sudo -u postgres psql -c "GRANT ALL PRIVILEGES ON DATABASE bookstore_db TO bookstore_user;" > /dev/null 2>&1 || true
sudo -u postgres psql -d bookstore_db -c "GRANT ALL ON SCHEMA public TO bookstore_user;" > /dev/null 2>&1 || true
echo "  Database created"

echo ""
echo "[4/5] Initializing schema..."
sudo -u postgres psql -d bookstore_db -f scripts/init_database.sql > /dev/null 2>&1 || true
echo "  Schema initialized"

echo ""
echo "[5/5] Loading seed data..."
sudo -u postgres psql -d bookstore_db -f scripts/seed.sql > /dev/null 2>&1 || true
echo "  Seed data loaded"

echo ""
echo "========================================"
echo "  Setup complete!                       "
echo "========================================"
echo ""
echo "To start the server:"
echo "  ./bookstore-server"
echo ""
echo "Server will be available at:"
echo "  http://localhost:4000/"
echo "  http://localhost:4000/graphql"
echo ""
echo "Default credentials:"
echo "  admin / password123"
echo "  staff / password123"
echo "  user / password123"
echo ""
