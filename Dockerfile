# Use a slim version of Debian as the base
FROM debian:12-slim

# Install necessary build tools - ADDED ninja-build HERE
RUN apt-get update && apt-get install -y git build-essential cmake ninja-build curl zip unzip tar pkg-config

# Install vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /opt/vcpkg && \
    /opt/vcpkg/bootstrap-vcpkg.sh

# Set environment variables for vcpkg
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV PATH="/opt/vcpkg:${PATH}"

# Copy your project files
WORKDIR /app
COPY . .

# Install dependencies via vcpkg
RUN /opt/vcpkg/vcpkg install cpr nlohmann-json

# Build the project
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake && \
    cmake --build .

# Run the application
CMD ["./build/RealssaSportsApi"]