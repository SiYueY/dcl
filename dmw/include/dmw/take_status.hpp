#ifndef DMW_TAKE_STATUS_HPP_
#define DMW_TAKE_STATUS_HPP_

namespace dmw {

/// Distinguishes a successful take from an empty reader history.
enum class TakeStatus { Taken, NoData };

}  // namespace dmw

#endif  // DMW_TAKE_STATUS_HPP_
