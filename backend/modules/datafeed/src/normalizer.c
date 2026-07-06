#include "datafeed/normalizer.h"
#include <ctype.h>
#include <string.h>

static size_t strip_and_upper(const char* in, char* out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        char c = in[i];
        if (c == '/' || c == '-' || c == '_' || c == ' ' || c == '.') continue;
        if (j + 1 >= out_len) break;
        out[j++] = (char)toupper((unsigned char)c);
    }
    out[j] = '\0';
    return j;
}

ArgentumStatus normalize_symbol(const char* in, char* out, size_t out_len) {
    if (!in || !out || out_len < 5) return ARGENTUM_ERR_INVALID;

    char tmp[32];
    size_t len = strip_and_upper(in, tmp, sizeof(tmp));
    if (len < 6) return ARGENTUM_ERR_RANGE;

    // Common explicit mappings for ARS/USD market
    if (strcmp(tmp, "ARSUSD") == 0) {
        strncpy(out, "ARS/USD", out_len);
        out[out_len - 1] = '\0';
        return ARGENTUM_OK;
    }
    if (strcmp(tmp, "USDARS") == 0) {
        strncpy(out, "USD/ARS", out_len);
        out[out_len - 1] = '\0';
        return ARGENTUM_OK;
    }
    if (strcmp(tmp, "USDTARS") == 0) {
        strncpy(out, "USDT/ARS", out_len);
        out[out_len - 1] = '\0';
        return ARGENTUM_OK;
    }

    // Heuristic: if ends with USDT (BTCUSDT -> BTC/USDT)
    if (len >= 7 && strcmp(&tmp[len - 4], "USDT") == 0) {
        size_t base_len = len - 4;
        if (base_len + 1 + 4 + 1 > out_len) return ARGENTUM_ERR_RANGE;
        memcpy(out, tmp, base_len);
        out[base_len] = '/';
        memcpy(out + base_len + 1, "USDT", 4);
        out[base_len + 1 + 4] = '\0';
        return ARGENTUM_OK;
    }

    // Default: split into 3/3 if length == 6
    if (len == 6) {
        if (out_len < 8) return ARGENTUM_ERR_RANGE;
        memcpy(out, tmp, 3);
        out[3] = '/';
        memcpy(out + 4, tmp + 3, 3);
        out[7] = '\0';
        return ARGENTUM_OK;
    }

    return ARGENTUM_ERR_INVALID;
}
