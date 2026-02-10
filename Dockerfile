FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    g++ \
    libpq-dev \
    libssl3 \
    libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY src/main.cpp src/main.cpp

RUN g++ -std=c++17 -pthread -o bookstore-server src/main.cpp -lpq -ljwt -lcurl -lssl -lcrypto

EXPOSE 4000

CMD ["./bookstore-server"]
