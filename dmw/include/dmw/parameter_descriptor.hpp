#ifndef DMW_PARAMETER_DESCRIPTOR_HPP_
#define DMW_PARAMETER_DESCRIPTOR_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace dmw {

struct IntegerRange {
    std::int64_t from_value{0};
    std::int64_t to_value{0};
    /// 0 means "no step constraint".
    std::uint64_t step{0};
};

struct FloatingPointRange {
    double from_value{0.0};
    double to_value{0.0};
    /// 0.0 means "no step constraint".
    double step{0.0};
};

/// Declared constraints for one parameter.
///
/// `dynamic_typing == false` fixes the parameter to the type of the value used
/// at declare time.  Ranges are only consulted for a value of the matching
/// numeric type.
struct ParameterDescriptor {
    std::string description;
    std::string additional_constraints;
    bool read_only{false};
    bool dynamic_typing{false};
    std::vector<IntegerRange> integer_ranges;
    std::vector<FloatingPointRange> floating_point_ranges;
};

}  // namespace dmw

#endif  // DMW_PARAMETER_DESCRIPTOR_HPP_
