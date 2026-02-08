#!/bin/bash

echo "========================================"
echo "  Vulnerable GraphQL Bookstore API     "
echo "  Build Script                         "
echo "========================================"

# Check for g++
command -v g++ >/dev/null 2>&1 || { echo "g++ is required but not installed. Aborting." >&2; exit 1; }

echo "Building server with PostgreSQL, JWT, CURL, and pthread support..."

# Build the full version with database support
g++ -std=c++17 -pthread -o bookstore-server src/main.cpp -lpq -ljwt -lcurl -lssl -lcrypto

if [ $? -eq 0 ]; then
    echo "========================================"
    echo "  Build successful!                    "
    echo "========================================"
    echo ""
    echo "Executable: bookstore-server"
    echo ""
    echo "To run the server:"
    echo "  ./bookstore-server"
    echo ""
    echo "Note: Make sure PostgreSQL is running and database is initialized:"
    echo "  sudo -u postgres psql -f scripts/init_database.sql"
    echo ""
    echo "GraphQL Playground will be available at:"
    echo "  http://localhost:4000/"
    echo ""
    echo "GraphQL endpoint:"
    echo "  http://localhost:4000/graphql"
else
    echo "Build failed!"
    exit 1
fi
