FROM debian:12-slim

# Install necessary build tools
RUN apt-get update && apt-get install -y \
    git build-essential cmake ninja-build curl zip unzip tar pkg-config

# Install vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /opt/vcpkg && \
    /opt/vcpkg/bootstrap-vcpkg.sh

# Set environment variables for vcpkg
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV PATH="/opt/vcpkg:${PATH}"

# Install Crow C++ framework (header-only library)
RUN git clone https://github.com/CrowCpp/Crow.git /opt/crow

# Copy project files
WORKDIR /app
COPY . .

# Install dependencies via vcpkg (Added asio for Crow)
RUN /opt/vcpkg/vcpkg install cpr nlohmann-json cpp-httplib asio

# Build the project
RUN mkdir -p build && cd build && \
    cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCROW_INCLUDE_DIR=/opt/crow/include \
    -G Ninja && \
    cmake --build .

# Run the application
CMD ["./build/RealssaSportsApi"]