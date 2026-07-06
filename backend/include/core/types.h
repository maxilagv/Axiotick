#ifndef ARGENTUM_CORE_TYPES_H
#define ARGENTUM_CORE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Constants ---
#define SYMBOL_LEN 16
#define SOURCE_LEN 8

#if defined(_MSC_VER)
#define ARGENTUM_ALIGNAS_64 __declspec(align(64))
#elif defined(__GNUC__) || defined(__clang__)
#define ARGENTUM_ALIGNAS_64 __attribute__((aligned(64)))
#else
#define ARGENTUM_ALIGNAS_64
#endif

// --- Enumerations (Bit-packed usually, keeping simple for C compat) ---
typedef enum {
    SIDE_BUY = 1,
    SIDE_SELL = 2
} Side;

typedef enum {
    ORDER_TYPE_MARKET = 1,
    ORDER_TYPE_LIMIT = 2,
    ORDER_TYPE_STOP = 3
} OrderType;

typedef enum {
    TIF_GTC = 1, // Good-Till-Cancel
    TIF_IOC = 2, // Immediate-Or-Cancel
    TIF_FOK = 3  // Fill-Or-Kill
} TimeInForce;

// --- Data Structures ---
// Alignment: cache-line aligned for hot-path structs.
// Usage of fixed-size arrays avoids pointer chasing.

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324)
#endif
/**
 * @brief Represents a single tick from the market.
 * Optimized size: 8 (ts) + 8 (px) + 8 (qty) + 16 (sym) + 8 (src) + 1 (side) + 7 (pad) = 56 bytes data + align.
 * Fits within a single 64-byte cache line.
 */
typedef struct ARGENTUM_ALIGNAS_64 {
    uint64_t timestamp_ns;  // Nanoseconds since epoch
    double price;
    double quantity;
    char symbol[SYMBOL_LEN];
    char source[SOURCE_LEN]; // e.g., "BINANCE", "BYMA"
    uint8_t side;           // 1=Buy, 2=Sell
    uint8_t _padding[7];    // Align to 64 bytes (optional but good for cache)
} MarketTick;
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/**
 * @brief Represents an internal order within the engine.
 */
typedef struct {
    uint64_t order_id;
    uint64_t client_id;
    uint64_t timestamp_ns;
    double price;
    double quantity;
    int64_t price_ticks;      // Fixed-point price for matching/risk critical path.
    int64_t quantity_lots;    // Fixed-point quantity for matching/risk critical path.
    char symbol[SYMBOL_LEN];
    uint8_t side;
    uint8_t type;
    uint8_t tif;
    uint8_t _padding[3];
} Order;

/**
 * @brief Represents a matched trade between orders.
 */
typedef struct {
    uint64_t trade_id;
    uint64_t maker_order_id;
    uint64_t taker_order_id;
    uint64_t timestamp_ns;
    double price;
    double quantity;
    int64_t price_ticks;
    int64_t quantity_lots;
    uint8_t side;          // side of aggressor
    uint8_t _padding[3];
} Trade;

#ifdef __cplusplus
static_assert(sizeof(MarketTick) == 64, "MarketTick must be exactly 64 bytes.");
static_assert(alignof(MarketTick) == 64, "MarketTick must be 64-byte aligned.");
#else
_Static_assert(sizeof(MarketTick) == 64, "MarketTick must be exactly 64 bytes.");
_Static_assert(_Alignof(MarketTick) == 64, "MarketTick must be 64-byte aligned.");
#endif

#ifdef __cplusplus
}
#endif

#endif // ARGENTUM_CORE_TYPES_H
