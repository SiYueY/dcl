#ifndef DMW_IMPL__FASTDDS__RETURN_CODE_HPP_
#define DMW_IMPL__FASTDDS__RETURN_CODE_HPP_

#include <string>
#include <utility>

#include <fastrtps/types/TypesBase.h>

#include "dmw/error.hpp"

namespace dmw::impl::fastdds {

inline ErrorCode to_return_code(eprosima::fastrtps::types::ReturnCode_t code) noexcept {
    using ReturnCode = eprosima::fastrtps::types::ReturnCode_t;
    switch (code()) {
        case ReturnCode::RETCODE_BAD_PARAMETER:
            return ErrorCode::InvalidArgument;
        case ReturnCode::RETCODE_OUT_OF_RESOURCES:
            return ErrorCode::ResourceExhausted;
        case ReturnCode::RETCODE_UNSUPPORTED:
        case ReturnCode::RETCODE_NOT_ALLOWED_BY_SECURITY:
            return ErrorCode::Unsupported;
        case ReturnCode::RETCODE_INCONSISTENT_POLICY:
            return ErrorCode::IncompatibleQos;
        case ReturnCode::RETCODE_TIMEOUT:
            return ErrorCode::Timeout;
        case ReturnCode::RETCODE_PRECONDITION_NOT_MET:
        case ReturnCode::RETCODE_NOT_ENABLED:
        case ReturnCode::RETCODE_IMMUTABLE_POLICY:
        case ReturnCode::RETCODE_ALREADY_DELETED:
        case ReturnCode::RETCODE_ILLEGAL_OPERATION:
            return ErrorCode::InvalidState;
        default:
            return ErrorCode::DDSError;
    }
}

inline Error to_error(eprosima::fastrtps::types::ReturnCode_t code, std::string message) {
    return Error(to_return_code(code), std::move(message));
}

}  // namespace dmw::impl::fastdds

#endif
