cmake_minimum_required(VERSION 3.16)
project(xrpld-dex-indexer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(RocksDB REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)

add_executable(dex-indexer
    src/main.cpp
    src/FeedConsumer.cpp
    src/TimeSeriesStore.cpp
    src/OHLCVAggregator.cpp
)

target_include_directories(dex-indexer PRIVATE src)

target_link_libraries(dex-indexer PRIVATE
    RocksDB::rocksdb
    Boost::program_options
    pthread
)
