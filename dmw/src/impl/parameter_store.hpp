#ifndef DMW_IMPL__PARAMETER_STORE_HPP_
#define DMW_IMPL__PARAMETER_STORE_HPP_

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "dmw/arguments.hpp"
#include "dmw/parameter.hpp"
#include "dmw/parameter_change_set.hpp"
#include "dmw/parameter_descriptor.hpp"
#include "dmw/result.hpp"

namespace dmw::impl {

/// Canonical parameter name validation shared by every entry point.
Result<void> validate_parameter_name(std::string_view name);

/// Parse one CLI/YAML override literal into a canonical typed value.
///
/// Boolean and numeric literals become typed values; everything else stays a
/// string.  Array literals are not part of the V1 command-line contract.
ParameterValue parse_parameter_literal(std::string_view text);

/// Select the overrides that apply to one Node identity.
///
/// `scope:name` entries apply only when the scope matches the Node name or its
/// fully qualified name; unscoped entries apply to every Node.
std::vector<Parameter> select_parameter_overrides(
    const std::vector<ParameterOverride>& overrides, std::string_view node_name,
    std::string_view fully_qualified_name);

/// Node-local parameter state: the sole authority for declared parameters,
/// descriptors, overrides, validation and atomic commits.
class ParameterStoreState {
public:
    ParameterStoreState(bool allow_undeclared_parameters, std::vector<Parameter> overrides);

    const std::vector<Parameter>& overrides() const noexcept { return overrides_; }

    Result<Parameter> declare(
        std::string_view name, const ParameterValue& default_value,
        const ParameterDescriptor& descriptor, bool ignore_override);

    Result<void> undeclare(std::string_view name);

    Result<bool> has(std::string_view name) const;

    Result<Parameter> get(std::string_view name) const;

    Result<std::vector<Parameter>> get_many(const std::vector<std::string>& names) const;

    Result<ParameterDescriptor> describe(std::string_view name) const;

    Result<ParameterListResult> list(
        const std::vector<std::string>& prefixes, std::size_t depth) const;

    Result<void> validate(const std::vector<Parameter>& parameters) const;

    Result<ParameterChangeSet> set_atomically(const std::vector<Parameter>& parameters);

    /// Return and clear the accumulated delta, including undeclared
    /// parameters, so a language layer can publish a complete typed
    /// parameter event.
    Result<ParameterChangeSet> take_changes();

private:
    struct StoredParameter {
        ParameterValue value;
        ParameterDescriptor descriptor;
    };

    Result<void> validate_name_and_duplicates(const std::vector<Parameter>& parameters) const;

    Result<void> validate_assignment_locked(
        const std::string& name, const ParameterValue& value, bool implicit_declare) const;

    static Result<void> validate_descriptor(
        const ParameterDescriptor& descriptor, const ParameterValue& direct_value);

    static Result<void> validate_value_against_descriptor(
        const ParameterValue& value, const ParameterDescriptor& descriptor);

    Result<Parameter> declare_locked(
        const std::string& name, const ParameterValue& default_value,
        const ParameterDescriptor& descriptor, bool ignore_override);

    mutable std::mutex mutex_;
    const bool allow_undeclared_parameters_;
    const std::vector<Parameter> overrides_;
    std::map<std::string, StoredParameter> parameters_;
    ParameterChangeSet pending_changes_;
};

}  // namespace dmw::impl

#endif  // DMW_IMPL__PARAMETER_STORE_HPP_
