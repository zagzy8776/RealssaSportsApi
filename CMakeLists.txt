cmake_minimum_required(VERSION 3.10)
project("RealssaSportsApi")

# Set the toolchain file
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)

# Find packages
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Create executable
add_executable(RealssaSportsApi "RealssaSportsApi/RealssaSportsApi.cpp")

# Link libraries
target_link_libraries(RealssaSportsApi PRIVATE cpr::cpr nlohmann_json::nlohmann_json)

# Set C++ standard
set_property(TARGET RealssaSportsApi PROPERTY CXX_STANDARD 17)