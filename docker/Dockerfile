FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
# Everything the project needs, in one reproducible image:
#   arm-none-eabi-*      firmware build (make build)
#   gcc, gcovr, lcov     host unit tests and coverage (make test, make coverage)
#   clang, lld           Wokwi custom chip (WebAssembly)   (make chips)
#   sigrok-cli           decoding logic-analyzer captures   (make decode)
#   python3              tools/*.py (dependency / pinout checks, VCD tools)
RUN apt-get update && apt-get install -y --no-install-recommends \
    make gcc libc6-dev gcc-arm-none-eabi binutils-arm-none-eabi \
    libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
    clang lld gcovr lcov sigrok-cli \
    python3 git ca-certificates srecord \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
