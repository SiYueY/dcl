#ifndef DMW_IMPL__FASTDDS__RETURN_CODE_HPP_
#define DMW_IMPL__FASTDDS__RETURN_CODE_HPP_

#include <string>
#include <utility>

#include <fastrtps/types/TypesBase.h>

#include "dmw/error.hpp"

namespace dmw::impl::fastdds {

inline ErrorCode map_return_code(eprosima::fastrtps::types::ReturnCode_t code) noexcept {
    using ReturnCode = eprosima::fastrtps::types::ReturnCode_t;
    if (code == ReturnCode::RETCODE_BAD_PARAMETER) return ErrorCode::InvalidArgument;
    if (code == ReturnCode::RETCODE_OUT_OF_RESOURCES) return ErrorCode::ResourceExhausted;
    if (code == ReturnCode::RETCODE_UNSUPPORTED || code == ReturnCode::RETCODE_NOT_ALLOWED_BY_SECURITY)
        return ErrorCode::Unsupported;
    if (code == ReturnCode::RETCODE_INCONSISTENT_POLICY) return ErrorCode::IncompatibleQos;
    if (code == ReturnCode::RETCODE_TIMEOUT) return ErrorCode::Timeout;
    if (code == ReturnCode::RETCODE_PRECONDITION_NOT_MET || code == ReturnCode::RETCODE_NOT_ENABLED ||
        code == ReturnCode::RETCODE_IMMUTABLE_POLICY || code == ReturnCode::RETCODE_ALREADY_DELETED ||
        code == ReturnCode::RETCODE_ILLEGAL_OPERATION)
        return ErrorCode::InvalidState;
    return ErrorCode::DdsError;
}

inline Error return_code_error(
    eprosima::fastrtps::types::ReturnCode_t code, std::string message) {
    return Error(map_return_code(code), std::move(message));
}

}  // namespace dmw::impl::fastdds

#endif
