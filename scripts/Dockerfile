FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    dos2unix \
    make \
    tar \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY . /build/
