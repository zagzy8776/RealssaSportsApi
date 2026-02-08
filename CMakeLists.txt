cmake_minimum_required(VERSION 3.10)
project("RealssaSportsApi")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find vcpkg packages
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

# Add Crow include directory
if(DEFINED CROW_INCLUDE_DIR)
    include_directories(${CROW_INCLUDE_DIR})
else()
    include_directories(/opt/crow/include)
endif()

add_executable(RealssaSportsApi "RealssaSportsApi/RealssaSportsApi.cpp")

target_link_libraries(RealssaSportsApi PRIVATE 
    cpr::cpr 
    nlohmann_json::nlohmann_json
    httplib::httplib
)

set_property(TARGET RealssaSportsApi PROPERTY CXX_STANDARD 17)