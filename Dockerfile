FROM ubuntu:22.04

# Install build dependencies and PostgreSQL client
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    cmake \
    libpq-dev \
    libssl-dev \
    libjwt-dev \
    libcurl4-openssl-dev \
    postgresql-client \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source files
COPY src/main.cpp src/main.cpp

# Copy init script
COPY scripts/init_database.sql scripts/init_database.sql
COPY scripts/init-db.sh scripts/init-db.sh
RUN chmod +x scripts/init-db.sh

# Compile the server
RUN g++ -std=c++17 -pthread -o bookstore-server src/main.cpp -lpq -ljwt -lcurl -lssl -lcrypto

# Expose port
EXPOSE 4000

# Run initialization then start server
CMD /app/scripts/init-db.sh && ./bookstore-server
