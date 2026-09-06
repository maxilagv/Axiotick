# Define source groups
set(CORE_SOURCES
    modules/core/src/dummy.c
    modules/core/src/time_utils.cpp
    modules/core/src/trace_context.cpp
    modules/core/src/pipeline_telemetry.cpp
)
set(NETWORK_SOURCES modules/network/src/socket_manager.c)
set(DATAFEED_SOURCES modules/datafeed/src/market_parser.c modules/datafeed/src/normalizer.c modules/datafeed/src/feed_player.cpp)
set(BUS_SOURCES modules/bus/src/message_bus.cpp modules/bus/src/message_protocol.cpp)
set(ENGINE_SOURCES modules/engine/src/order_book.cpp)
set(RISK_SOURCES modules/risk/src/risk_manager.cpp)
set(TRADING_SOURCES modules/trading/src/order_manager.cpp)
set(GATEWAY_SOURCES
    modules/gateway/src/fix_adapter.cpp
    modules/gateway/src/smart_order_router.cpp
    modules/gateway/src/market_simulator.cpp
    modules/gateway/src/execution_quality.cpp
)
set(BACKTEST_SOURCES
    modules/backtest/src/backtest_engine.cpp
    modules/backtest/src/backtest_metrics.cpp
    modules/backtest/src/backtest_advanced.cpp
)
set(REGIME_SOURCES modules/regime/src/regime_classifier.cpp)
set(STRATEGY_SOURCES modules/strategy/src/strategy_registry.cpp)
set(PERSIST_SOURCES modules/persist/src/data_writer.cpp modules/persist/src/event_journal.cpp)
set(CODEC_SOURCES modules/codec/src/market_tick_codec.cpp)
set(EV_SOURCES modules/ev/src/ev_gate.cpp)
set(SIGNAL_SOURCES modules/signal/src/signal_engine.cpp)
# New modules are header-only for now, but added to includes

if(ARGENTUM_USE_FLATBUFFERS)
    find_package(Flatbuffers REQUIRED)
    find_program(FLATC_EXECUTABLE flatc REQUIRED)

    set(FB_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/schema/argentum.fbs)
    set(FB_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/flatbuffers)
    file(MAKE_DIRECTORY ${FB_GENERATED_DIR})

    add_custom_command(
        OUTPUT ${FB_GENERATED_DIR}/argentum_generated.h
        COMMAND ${FLATC_EXECUTABLE} --cpp --scoped-enums -o ${FB_GENERATED_DIR} ${FB_SCHEMA}
        DEPENDS ${FB_SCHEMA}
        COMMENT "Generating FlatBuffers schema"
    )

    add_custom_target(argentum_flatbuffers_gen DEPENDS ${FB_GENERATED_DIR}/argentum_generated.h)

    add_library(argentum_flatbuffers_generated INTERFACE)
    add_dependencies(argentum_flatbuffers_generated argentum_flatbuffers_gen)
    target_include_directories(argentum_flatbuffers_generated INTERFACE ${FB_GENERATED_DIR})
endif()

# Libraries
add_library(argentum_core STATIC ${CORE_SOURCES})
target_include_directories(argentum_core PUBLIC include)
if(ARGENTUM_ENABLE_LATENCY_TRACE)
    # PUBLIC on the root library: every target links argentum_core, so the
    # flag propagates to all TraceSpans::stamp() call sites consistently.
    target_compile_definitions(argentum_core PUBLIC ARGENTUM_ENABLE_LATENCY_TRACE)
endif()

add_library(argentum_bus STATIC ${BUS_SOURCES})
target_include_directories(argentum_bus PUBLIC include)
target_link_libraries(argentum_bus PUBLIC argentum_core)

add_library(argentum_codec STATIC ${CODEC_SOURCES})
target_include_directories(argentum_codec PUBLIC include)
target_link_libraries(argentum_codec PUBLIC argentum_bus argentum_core)

if(ARGENTUM_USE_FLATBUFFERS)
    target_compile_definitions(argentum_codec PUBLIC ARGENTUM_USE_FLATBUFFERS)
    target_link_libraries(argentum_codec PUBLIC Flatbuffers::flatbuffers argentum_flatbuffers_generated)
endif()

add_library(argentum_network STATIC ${NETWORK_SOURCES})
target_include_directories(argentum_network PUBLIC include)
target_link_libraries(argentum_network PUBLIC argentum_core)

add_library(argentum_datafeed STATIC ${DATAFEED_SOURCES})
target_include_directories(argentum_datafeed PUBLIC include)
target_link_libraries(argentum_datafeed PUBLIC argentum_core argentum_codec)

add_library(argentum_engine STATIC ${ENGINE_SOURCES})
target_include_directories(argentum_engine PUBLIC include)
target_link_libraries(argentum_engine PUBLIC argentum_core)

add_library(argentum_risk STATIC ${RISK_SOURCES})
target_include_directories(argentum_risk PUBLIC include)
target_link_libraries(argentum_risk PUBLIC argentum_core)

add_library(argentum_trading STATIC ${TRADING_SOURCES})
target_include_directories(argentum_trading PUBLIC include)
target_link_libraries(argentum_trading PUBLIC argentum_risk argentum_engine argentum_core)

add_library(argentum_gateway STATIC ${GATEWAY_SOURCES})
target_include_directories(argentum_gateway PUBLIC include)
target_link_libraries(argentum_gateway PUBLIC argentum_core)

add_library(argentum_regime STATIC ${REGIME_SOURCES})
target_include_directories(argentum_regime PUBLIC include)
target_link_libraries(argentum_regime PUBLIC argentum_core)

add_library(argentum_backtest STATIC ${BACKTEST_SOURCES})
target_include_directories(argentum_backtest PUBLIC include)
target_link_libraries(argentum_backtest PUBLIC argentum_signal argentum_regime argentum_risk argentum_core)

add_library(argentum_persist STATIC ${PERSIST_SOURCES})
target_include_directories(argentum_persist PUBLIC include)
target_link_libraries(argentum_persist PUBLIC argentum_core)
target_link_libraries(argentum_trading PUBLIC argentum_persist)

add_library(argentum_ev STATIC ${EV_SOURCES})
target_include_directories(argentum_ev PUBLIC include)
target_link_libraries(argentum_ev PUBLIC argentum_core)

add_library(argentum_strategy STATIC ${STRATEGY_SOURCES})
target_include_directories(argentum_strategy PUBLIC include)
target_link_libraries(argentum_strategy PUBLIC argentum_ev argentum_core)

add_library(argentum_signal STATIC ${SIGNAL_SOURCES})
target_include_directories(argentum_signal PUBLIC include)
target_link_libraries(argentum_signal PUBLIC argentum_strategy argentum_ev argentum_trading argentum_core)

add_subdirectory(modules/api)

# Executable
add_executable(argentum_node apps/node/main.cpp)
target_link_libraries(argentum_node PRIVATE 
    argentum_trading 
    argentum_risk 
    argentum_engine 
    argentum_network 
    argentum_datafeed
    argentum_bus
    argentum_codec
    argentum_persist
    argentum_backtest
    argentum_api
    argentum_gateway
    argentum_core
)
if(WIN32)
    target_link_libraries(argentum_node PRIVATE ws2_32)
endif()

# Benchmarks: all run through benchmark::Harness (warmup, unified clock,
# JSON output with machine-captured environment metadata; ADR 0015).
# ARGENTUM_BUILD_CONFIG feeds the metadata for multi-config generators.
set(ARGENTUM_BENCHMARK_TARGETS
    argentum_pipeline_benchmark
    argentum_matching_benchmark
    argentum_regime_benchmark
    argentum_risk_benchmark
)

add_executable(argentum_pipeline_benchmark benchmarks/pipeline_benchmark.cpp)
target_link_libraries(argentum_pipeline_benchmark PRIVATE
    argentum_bus
    argentum_codec
    argentum_persist
    argentum_core
)

add_executable(argentum_matching_benchmark benchmarks/matching_benchmark.cpp)
target_link_libraries(argentum_matching_benchmark PRIVATE
    argentum_engine
    argentum_core
)

add_executable(argentum_regime_benchmark benchmarks/regime_benchmark.cpp)
target_link_libraries(argentum_regime_benchmark PRIVATE
    argentum_regime
    argentum_core
)

add_executable(argentum_risk_benchmark benchmarks/risk_benchmark.cpp)
target_link_libraries(argentum_risk_benchmark PRIVATE
    argentum_risk
    argentum_core
)

foreach(bench_target IN LISTS ARGENTUM_BENCHMARK_TARGETS)
    target_compile_definitions(${bench_target} PRIVATE ARGENTUM_BUILD_CONFIG="$<CONFIG>")
endforeach()

add_executable(argentum_clock_check apps/clock_check/clock_check_main.cpp)
target_link_libraries(argentum_clock_check PRIVATE argentum_core)

add_executable(argentum_replay apps/replay/replay_main.cpp)
target_link_libraries(argentum_replay PRIVATE
    argentum_persist
    argentum_core
)

add_executable(argentum_signal_demo apps/signal_demo/signal_demo_main.cpp)
target_link_libraries(argentum_signal_demo PRIVATE
    argentum_signal
    argentum_ev
    argentum_trading
    argentum_risk
    argentum_engine
    argentum_persist
    argentum_core
)

add_executable(argentum_backtest_demo apps/backtest_demo/backtest_demo_main.cpp)
target_link_libraries(argentum_backtest_demo PRIVATE
    argentum_backtest
    argentum_signal
    argentum_regime
    argentum_core
)

add_subdirectory(tests)
