#pragma once

#include "core/errors.h"

#include <optional>
#include <utility>

namespace argentum::core {

enum class ErrorCode {
    Ok = 0,
    Parse,
    Io,
    Range,
    Timeout,
    Proto,
    NoMem,
    Invalid
};

inline ErrorCode to_error(ArgentumStatus status) {
    switch (status) {
        case ARGENTUM_OK: return ErrorCode::Ok;
        case ARGENTUM_ERR_PARSE: return ErrorCode::Parse;
        case ARGENTUM_ERR_IO: return ErrorCode::Io;
        case ARGENTUM_ERR_RANGE: return ErrorCode::Range;
        case ARGENTUM_ERR_TIMEOUT: return ErrorCode::Timeout;
        case ARGENTUM_ERR_PROTO: return ErrorCode::Proto;
        case ARGENTUM_ERR_NOMEM: return ErrorCode::NoMem;
        case ARGENTUM_ERR_INVALID: return ErrorCode::Invalid;
        default: return ErrorCode::Invalid;
    }
}

inline ArgentumStatus to_status(ErrorCode error) {
    switch (error) {
        case ErrorCode::Ok: return ARGENTUM_OK;
        case ErrorCode::Parse: return ARGENTUM_ERR_PARSE;
        case ErrorCode::Io: return ARGENTUM_ERR_IO;
        case ErrorCode::Range: return ARGENTUM_ERR_RANGE;
        case ErrorCode::Timeout: return ARGENTUM_ERR_TIMEOUT;
        case ErrorCode::Proto: return ARGENTUM_ERR_PROTO;
        case ErrorCode::NoMem: return ARGENTUM_ERR_NOMEM;
        case ErrorCode::Invalid: return ARGENTUM_ERR_INVALID;
        default: return ARGENTUM_ERR_INVALID;
    }
}

template <typename T>
class Expected {
public:
    Expected(const T& value) : value_(value), error_(ErrorCode::Ok) {}
    Expected(T&& value) : value_(std::move(value)), error_(ErrorCode::Ok) {}
    Expected(ErrorCode error) : value_(std::nullopt), error_(error) {}

    [[nodiscard]] bool has_value() const { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const { return has_value(); }

    T& value() { return value_.value(); }
    const T& value() const { return value_.value(); }
    [[nodiscard]] ErrorCode error() const { return error_; }

private:
    std::optional<T> value_;
    ErrorCode error_;
};

} // namespace argentum::core
