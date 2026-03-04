#pragma once

#include "types.h"

enum class Error : uint32_t {
    None = 0,
    OutOfMemory,
    InvalidArgument,
    NotFound,
    NotSupported,
};

template <typename T>
class Result {
public:
    static Result ok(T val) { return Result(val, Error::None); }
    static Result err(Error e) { return Result(T{}, e); }

    bool  is_ok()  const { return m_error == Error::None; }
    bool  is_err() const { return m_error != Error::None; }
    T     value()  const { return m_value; }
    Error error()  const { return m_error; }

private:
    Result(T val, Error err) : m_value(val), m_error(err) {}
    T     m_value;
    Error m_error;
};
