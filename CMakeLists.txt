cmake_minimum_required(VERSION 3.10)
project("RealssaSportsApi")

# Find packages (vcpkg toolchain is set by Dockerfile on Railway)
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Create executable - note the path!
add_executable(RealssaSportsApi "RealssaSportsApi/RealssaSportsApi.cpp")

# Link libraries
target_link_libraries(RealssaSportsApi PRIVATE 
    cpr::cpr 
    nlohmann_json::nlohmann_json
)
set_property(TARGET RealssaSportsApi PROPERTY CXX_STANDARD 17)