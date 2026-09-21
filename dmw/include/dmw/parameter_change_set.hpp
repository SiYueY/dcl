#ifndef DMW_PARAMETER_CHANGE_SET_HPP_
#define DMW_PARAMETER_CHANGE_SET_HPP_

#include <string>
#include <vector>

#include "dmw/parameter.hpp"

namespace dmw {

/// Delta produced by one successful parameter store commit.
///
/// DMW is the authority for this delta; language layers assemble the typed
/// parameter event message from it.
struct ParameterChangeSet {
    std::vector<Parameter> new_parameters;
    std::vector<Parameter> changed_parameters;
    std::vector<Parameter> deleted_parameters;

    bool empty() const noexcept {
        return new_parameters.empty() && changed_parameters.empty() &&
               deleted_parameters.empty();
    }
};

/// Result of one prefix/depth limited parameter listing.
struct ParameterListResult {
    std::vector<std::string> names;
    std::vector<std::string> prefixes;
};

}  // namespace dmw

#endif  // DMW_PARAMETER_CHANGE_SET_HPP_
