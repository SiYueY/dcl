#ifndef DMW_EVENT_INFO_HPP_
#define DMW_EVENT_INFO_HPP_

#include <cstddef>
#include <cstdint>
#include <variant>

namespace dmw {

enum class EventType {
    LivelinessChanged,
    RequestedDeadlineMissed,
    RequestedIncompatibleQos,
    MessageLost,
    LivelinessLost,
    OfferedDeadlineMissed,
    OfferedIncompatibleQos
};

struct DeadlineMissedInfo {
    std::int32_t total_count{0};
    std::int32_t total_count_change{0};
};

struct LivelinessLostInfo {
    std::int32_t total_count{0};
    std::int32_t total_count_change{0};
};

struct LivelinessChangedInfo {
    std::int32_t alive_count{0};
    std::int32_t not_alive_count{0};
    std::int32_t alive_count_change{0};
    std::int32_t not_alive_count_change{0};
};

enum class QosPolicyKind {
    Unknown,
    History,
    Reliability,
    Durability,
    Deadline,
    Lifespan,
    Liveliness
};

struct IncompatibleQosInfo {
    std::int32_t total_count{0};
    std::int32_t total_count_change{0};
    QosPolicyKind last_policy{QosPolicyKind::Unknown};
};

struct MessageLostInfo {
    std::size_t total_count{0};
    std::size_t total_count_change{0};
};

using EventInfo = std::variant<
    DeadlineMissedInfo, LivelinessLostInfo, LivelinessChangedInfo, IncompatibleQosInfo,
    MessageLostInfo>;

}  // namespace dmw

#endif  // DMW_EVENT_INFO_HPP_
