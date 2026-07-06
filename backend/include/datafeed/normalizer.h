#ifndef ARGENTUM_DATAFEED_NORMALIZER_H
#define ARGENTUM_DATAFEED_NORMALIZER_H

#include <stddef.h>
#include "core/errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Normalize symbols to canonical "AAA/BBB" format.
 * @return ARGENTUM_OK on success, error code on failure.
 */
ArgentumStatus normalize_symbol(const char* in, char* out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif // ARGENTUM_DATAFEED_NORMALIZER_H
