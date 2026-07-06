#ifndef ARGENTUM_DATAFEED_MARKET_PARSER_H
#define ARGENTUM_DATAFEED_MARKET_PARSER_H

#include <stddef.h>
#include "core/types.h"
#include "core/errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FEED_FORMAT_JSON = 1,
    FEED_FORMAT_FIX = 2,
    FEED_FORMAT_SBE = 3
} FeedFormat;

/**
 * @brief Parse a raw feed message into a MarketTick.
 * @return ARGENTUM_OK on success, error code on failure.
 */
ArgentumStatus parse_market_message(FeedFormat format, const char* data, size_t len, MarketTick* out);

#ifdef __cplusplus
}
#endif

#endif // ARGENTUM_DATAFEED_MARKET_PARSER_H
