add_library(argentum_api STATIC
  src/market_gateway.cpp
  src/http_ws_server.cpp
)

target_include_directories(argentum_api PUBLIC include)
target_link_libraries(argentum_api PUBLIC
  argentum_bus
  argentum_codec
  argentum_trading
  argentum_persist
  argentum_network
  argentum_core
)
