#ifndef ARGENTUM_CORE_ERRORS_H
#define ARGENTUM_CORE_ERRORS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ArgentumStatus {
    ARGENTUM_OK = 0,
    ARGENTUM_ERR_PARSE = 1,
    ARGENTUM_ERR_IO = 2,
    ARGENTUM_ERR_RANGE = 3,
    ARGENTUM_ERR_TIMEOUT = 4,
    ARGENTUM_ERR_PROTO = 5,
    ARGENTUM_ERR_NOMEM = 6,
    ARGENTUM_ERR_INVALID = 7
} ArgentumStatus;

#ifdef __cplusplus
}
#endif

#endif // ARGENTUM_CORE_ERRORS_H
