cmake_minimum_required(VERSION 3.10)
project("RealssaSportsApi")

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

add_executable(RealssaSportsApi "RealssaSportsApi/RealssaSportsApi.cpp")

target_link_libraries(RealssaSportsApi PRIVATE 
    cpr::cpr 
    nlohmann_json::nlohmann_json
    httplib::httplib
)

set_property(TARGET RealssaSportsApi PROPERTY CXX_STANDARD 17)