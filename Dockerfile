FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    g++ \
    make \
    cmake \
    libpq-dev \
    libssl-dev \
    libjwt-dev \
    libcurl4-openssl-dev \
    postgresql-client \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY src/main.cpp src/main.cpp

RUN g++ -std=c++17 -pthread -o bookstore-server src/main.cpp -lpq -ljwt -lcurl -lssl -lcrypto

EXPOSE 4000

CMD ["./bookstore-server"]
