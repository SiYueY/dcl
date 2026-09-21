#ifndef DMW_ARGUMENTS_HPP_
#define DMW_ARGUMENTS_HPP_

#include <string>
#include <string_view>
#include <vector>

#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

struct RemapRule {
    std::string from;
    std::string to;
};

struct ParameterOverride {
    std::string name;
    std::string value;
};

class DMW_PUBLIC Arguments {
public:
    const std::vector<RemapRule>& remaps() const noexcept { return remaps_; }
    const std::vector<ParameterOverride>& parameter_overrides() const noexcept {
        return parameter_overrides_;
    }
    const std::vector<std::string>& unparsed_arguments() const noexcept { return unparsed_arguments_; }
    std::string_view node_name_remap() const noexcept { return node_name_remap_; }
    std::string_view namespace_remap() const noexcept { return namespace_remap_; }

private:
    friend DMW_PUBLIC Result<Arguments> parse_arguments(const std::vector<std::string>& arguments);

    std::vector<RemapRule> remaps_;
    std::vector<ParameterOverride> parameter_overrides_;
    std::vector<std::string> unparsed_arguments_;
    std::string node_name_remap_;
    std::string namespace_remap_;
};

DMW_PUBLIC Result<Arguments> parse_arguments(const std::vector<std::string>& arguments);

}  // namespace dmw

#endif  // DMW_ARGUMENTS_HPP_
