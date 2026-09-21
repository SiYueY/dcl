#include "impl/parameter_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <utility>

#include "dmw/error.hpp"

namespace dmw::impl {

namespace {

constexpr std::size_t kUnlimitedDepth = 0;
constexpr double kFloatingEpsilon = 1e-9;

bool is_token_start(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool is_token_char(char value) noexcept {
    return is_token_start(value) || (value >= '0' && value <= '9');
}

bool parses_as_integer(std::string_view text, std::int64_t& value) noexcept {
    if (text.empty()) return false;
    std::size_t index = 0;
    bool negative = false;
    if (text.front() == '-' || text.front() == '+') {
        negative = text.front() == '-';
        index = 1;
    }
    if (index == text.size()) return false;
    std::uint64_t magnitude = 0;
    constexpr std::uint64_t kPositiveLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    // |INT64_MIN| is one larger than INT64_MAX, so the magnitude cap allows it.
    constexpr std::uint64_t kMagnitudeCap = kPositiveLimit + 1;
    for (; index < text.size(); ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9') return false;
        const auto unit = static_cast<std::uint64_t>(digit - '0');
        if (magnitude > (kMagnitudeCap - unit) / 10) return false;
        magnitude = magnitude * 10 + unit;
    }
    if (!negative) {
        if (magnitude > kPositiveLimit) return false;
        value = static_cast<std::int64_t>(magnitude);
        return true;
    }
    if (magnitude == kMagnitudeCap) {
        value = std::numeric_limits<std::int64_t>::min();
        return true;
    }
    value = -static_cast<std::int64_t>(magnitude);
    return true;
}

bool parses_as_double(std::string_view text, double& value) noexcept {
    if (text.empty()) return false;
    bool has_marker = false;
    for (const char character : text) {
        if (character == '.' || character == 'e' || character == 'E') has_marker = true;
    }
    if (!has_marker) return false;
    std::string buffer(text);
    char* end = nullptr;
    const double parsed = std::strtod(buffer.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    value = parsed;
    return true;
}

/// A value matching at least one declared range is acceptable, matching the
/// common ROS 2 descriptor semantics.  Step is checked on the matched range.
bool integer_value_is_acceptable(std::int64_t value, const IntegerRange& range) noexcept {
    if (range.from_value > range.to_value) return false;
    if (value < range.from_value || value > range.to_value) return false;
    if (range.step != 0) {
        const auto offset =
            static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(range.from_value);
        if (offset % range.step != 0) return false;
    }
    return true;
}

bool double_value_is_acceptable(double value, const FloatingPointRange& range) noexcept {
    if (std::isnan(value)) return false;
    if (range.from_value > range.to_value) return false;
    if (value < range.from_value - kFloatingEpsilon ||
        value > range.to_value + kFloatingEpsilon) {
        return false;
    }
    if (range.step > 0.0) {
        const double offset = value - range.from_value;
        const double ratio = offset / range.step;
        const double nearest = std::round(ratio);
        if (std::fabs(ratio - nearest) > kFloatingEpsilon) return false;
    }
    return true;
}

std::size_t count_tokens(std::string_view name) noexcept {
    std::size_t count = 1;
    for (const char character : name) {
        if (character == '.') ++count;
    }
    return count;
}

std::string truncate_to_depth(std::string_view name, std::size_t tokens) {
    std::size_t seen = 0;
    for (std::size_t index = 0; index < name.size(); ++index) {
        if (name[index] == '.') {
            ++seen;
            if (seen == tokens) return std::string(name.substr(0, index));
        }
    }
    return std::string(name);
}

}  // namespace

Result<void> validate_parameter_name(std::string_view name) {
    if (name.empty()) {
        return Result<void>::failure(Error(ErrorCode::InvalidName, "Parameter name is empty"));
    }
    if (name.front() == '.' || name.back() == '.') {
        return Result<void>::failure(
            Error(ErrorCode::InvalidName, "Parameter name must not start or end with '.'"));
    }
    std::size_t token_start = 0;
    for (std::size_t index = 0; index <= name.size(); ++index) {
        if (index != name.size() && name[index] != '.') {
            if (index == token_start) {
                if (!is_token_start(name[index])) {
                    return Result<void>::failure(Error(
                        ErrorCode::InvalidName,
                        "Parameter name token must start with a letter or underscore"));
                }
            } else if (!is_token_char(name[index])) {
                return Result<void>::failure(
                    Error(ErrorCode::InvalidName, "Parameter name contains an invalid character"));
            }
            continue;
        }
        if (index == token_start) {
            return Result<void>::failure(
                Error(ErrorCode::InvalidName, "Parameter name contains an empty token"));
        }
        token_start = index + 1;
    }
    return Result<void>::success();
}

ParameterValue parse_parameter_literal(std::string_view text) {
    if (text == "true") return ParameterValue::make_bool(true);
    if (text == "false") return ParameterValue::make_bool(false);
    std::int64_t integer = 0;
    if (parses_as_integer(text, integer)) return ParameterValue::make_integer(integer);
    double floating = 0.0;
    if (parses_as_double(text, floating)) return ParameterValue::make_double(floating);
    return ParameterValue::make_string(std::string(text));
}

std::vector<Parameter> select_parameter_overrides(
    const std::vector<ParameterOverride>& overrides, std::string_view node_name,
    std::string_view fully_qualified_name) {
    std::vector<Parameter> selected;
    selected.reserve(overrides.size());
    std::string_view unqualified_fqn = fully_qualified_name;
    if (!unqualified_fqn.empty() && unqualified_fqn.front() == '/') {
        unqualified_fqn.remove_prefix(1);
    }
    for (const auto& override : overrides) {
        const auto separator = override.name.find(':');
        if (separator == std::string::npos) {
            selected.push_back({override.name, parse_parameter_literal(override.value)});
            continue;
        }
        auto scope = std::string_view(override.name).substr(0, separator);
        if (!scope.empty() && scope.front() == '/') scope.remove_prefix(1);
        if (scope.empty() || (scope != node_name && scope != unqualified_fqn)) continue;
        selected.push_back(
            {override.name.substr(separator + 1), parse_parameter_literal(override.value)});
    }
    return selected;
}

ParameterStoreState::ParameterStoreState(
    bool allow_undeclared_parameters, std::vector<Parameter> overrides)
: allow_undeclared_parameters_(allow_undeclared_parameters), overrides_(std::move(overrides)) {}

Result<void> ParameterStoreState::validate_descriptor(
    const ParameterDescriptor& descriptor, const ParameterValue& direct_value) {
    for (const auto& range : descriptor.integer_ranges) {
        if (range.from_value > range.to_value) {
            return Result<void>::failure(
                Error(ErrorCode::InvalidArgument, "Integer range has from_value > to_value"));
        }
    }
    for (const auto& range : descriptor.floating_point_ranges) {
        if (!(range.from_value <= range.to_value)) {
            return Result<void>::failure(
                Error(ErrorCode::InvalidArgument, "Floating point range has from_value > to_value"));
        }
        if (range.step < 0.0 || (!std::isfinite(range.step) && range.step != 0.0)) {
            return Result<void>::failure(Error(
                ErrorCode::InvalidArgument,
                "Floating point range step must be zero or a finite positive value"));
        }
    }
    if (!descriptor.dynamic_typing && !direct_value.is_set()) {
        return Result<void>::failure(Error(
            ErrorCode::InvalidArgument,
            "Fixed-type parameter requires a typed default value"));
    }
    return Result<void>::success();
}

Result<void> ParameterStoreState::validate_value_against_descriptor(
    const ParameterValue& value, const ParameterDescriptor& descriptor) {
    if (value.type() == ParameterType::Integer) {
        const auto integer = value.as_integer();
        if (!descriptor.integer_ranges.empty()) {
            const bool acceptable = std::any_of(
                descriptor.integer_ranges.begin(), descriptor.integer_ranges.end(),
                [integer](const IntegerRange& range) {
                    return integer_value_is_acceptable(integer, range);
                });
            if (!acceptable) {
                return Result<void>::failure(Error(
                    ErrorCode::InvalidArgument, "Integer parameter value is outside its range"));
            }
        }
    } else if (value.type() == ParameterType::Double) {
        const auto floating = value.as_double();
        if (!descriptor.floating_point_ranges.empty()) {
            const bool acceptable = std::any_of(
                descriptor.floating_point_ranges.begin(),
                descriptor.floating_point_ranges.end(),
                [floating](const FloatingPointRange& range) {
                    return double_value_is_acceptable(floating, range);
                });
            if (!acceptable) {
                return Result<void>::failure(Error(
                    ErrorCode::InvalidArgument,
                    "Floating point parameter value is outside its range"));
            }
        }
    }
    return Result<void>::success();
}

Result<void> ParameterStoreState::validate_assignment_locked(
    const std::string& name, const ParameterValue& value, bool implicit_declare) const {
    auto valid_name = validate_parameter_name(name);
    if (!valid_name) return valid_name;

    const auto found = parameters_.find(name);
    if (found == parameters_.end()) {
        if (!allow_undeclared_parameters_ && !implicit_declare) {
            return Result<void>::failure(
                Error(ErrorCode::NotFound, "Parameter is not declared"));
        }
        return Result<void>::success();
    }
    if (found->second.descriptor.read_only) {
        return Result<void>::failure(
            Error(ErrorCode::InvalidState, "Parameter is read-only"));
    }
    if (!found->second.descriptor.dynamic_typing && found->second.value.is_set() &&
        found->second.value.type() != value.type()) {
        return Result<void>::failure(
            Error(ErrorCode::InvalidArgument, "Parameter value type does not match its declaration"));
    }
    return validate_value_against_descriptor(value, found->second.descriptor);
}

Result<void> ParameterStoreState::validate_name_and_duplicates(
    const std::vector<Parameter>& parameters) const {
    std::set<std::string> seen;
    for (const auto& parameter : parameters) {
        auto valid_name = validate_parameter_name(parameter.name);
        if (!valid_name) return valid_name;
        if (!seen.insert(parameter.name).second) {
            return Result<void>::failure(
                Error(ErrorCode::InvalidArgument, "Duplicate parameter name in one request"));
        }
    }
    return Result<void>::success();
}

Result<Parameter> ParameterStoreState::declare_locked(
    const std::string& name, const ParameterValue& default_value,
    const ParameterDescriptor& descriptor, bool ignore_override) {
    ParameterValue effective = default_value;
    if (!ignore_override) {
        const auto override = std::find_if(
            overrides_.begin(), overrides_.end(),
            [&name](const Parameter& value) { return value.name == name; });
        if (override != overrides_.end()) effective = override->value;
    }
    if (!descriptor.dynamic_typing && effective.is_set() && default_value.is_set() &&
        effective.type() != default_value.type()) {
        return Result<Parameter>::failure(Error(
            ErrorCode::InvalidArgument, "Parameter override type does not match the declared type"));
    }
    auto validated = validate_value_against_descriptor(effective, descriptor);
    if (!validated) return Result<Parameter>::failure(std::move(validated.error()));

    parameters_.emplace(name, StoredParameter{effective, descriptor});
    return Result<Parameter>::success(Parameter{name, std::move(effective)});
}

Result<Parameter> ParameterStoreState::declare(
    std::string_view name, const ParameterValue& default_value,
    const ParameterDescriptor& descriptor, bool ignore_override) {
    auto valid_name = validate_parameter_name(name);
    if (!valid_name) return Result<Parameter>::failure(std::move(valid_name.error()));
    auto valid_descriptor = validate_descriptor(descriptor, default_value);
    if (!valid_descriptor) {
        return Result<Parameter>::failure(std::move(valid_descriptor.error()));
    }

    std::lock_guard lock(mutex_);
    const std::string key(name);
    if (parameters_.count(key) != 0) {
        return Result<Parameter>::failure(
            Error(ErrorCode::AlreadyExists, "Parameter is already declared"));
    }
    auto declared = declare_locked(key, default_value, descriptor, ignore_override);
    if (!declared) return declared;
    try {
        pending_changes_.new_parameters.push_back(declared.value());
    } catch (...) {
        // The public declare operation is transactional: if recording the
        // change-set allocation fails, undo the already-inserted store entry.
        parameters_.erase(key);
        throw;
    }
    return declared;
}

Result<void> ParameterStoreState::undeclare(std::string_view name) {
    auto valid_name = validate_parameter_name(name);
    if (!valid_name) return valid_name;
    std::lock_guard lock(mutex_);
    const std::string key(name);
    const auto found = parameters_.find(key);
    if (found == parameters_.end()) {
        return Result<void>::failure(Error(ErrorCode::NotFound, "Parameter is not declared"));
    }
    pending_changes_.deleted_parameters.push_back(Parameter{key, found->second.value});
    parameters_.erase(found);
    return Result<void>::success();
}

Result<bool> ParameterStoreState::has(std::string_view name) const {
    auto valid_name = validate_parameter_name(name);
    if (!valid_name) return Result<bool>::failure(std::move(valid_name.error()));
    std::lock_guard lock(mutex_);
    return Result<bool>::success(parameters_.count(std::string(name)) != 0);
}

Result<Parameter> ParameterStoreState::get(std::string_view name) const {
    auto valid_name = validate_parameter_name(name);
    if (!valid_name) return Result<Parameter>::failure(std::move(valid_name.error()));
    std::lock_guard lock(mutex_);
    const auto found = parameters_.find(std::string(name));
    if (found == parameters_.end()) {
        if (allow_undeclared_parameters_) {
            return Result<Parameter>::success(Parameter{std::string(name), ParameterValue{}});
        }
        return Result<Parameter>::failure(Error(ErrorCode::NotFound, "Parameter is not declared"));
    }
    return Result<Parameter>::success(Parameter{found->first, found->second.value});
}

Result<std::vector<Parameter>> ParameterStoreState::get_many(
    const std::vector<std::string>& names) const {
    std::vector<Parameter> result;
    result.reserve(names.size());
    for (const auto& name : names) {
        auto value = get(name);
        if (!value) return Result<std::vector<Parameter>>::failure(std::move(value.error()));
        result.push_back(std::move(value.value()));
    }
    return Result<std::vector<Parameter>>::success(std::move(result));
}

Result<ParameterDescriptor> ParameterStoreState::describe(std::string_view name) const {
    auto valid_name = validate_parameter_name(name);
    if (!valid_name) return Result<ParameterDescriptor>::failure(std::move(valid_name.error()));
    std::lock_guard lock(mutex_);
    const auto found = parameters_.find(std::string(name));
    if (found == parameters_.end()) {
        return Result<ParameterDescriptor>::failure(
            Error(ErrorCode::NotFound, "Parameter is not declared"));
    }
    return Result<ParameterDescriptor>::success(found->second.descriptor);
}

Result<ParameterListResult> ParameterStoreState::list(
    const std::vector<std::string>& prefixes, std::size_t depth) const {
    for (const auto& prefix : prefixes) {
        if (!prefix.empty()) {
            auto valid_name = validate_parameter_name(prefix);
            if (!valid_name) return Result<ParameterListResult>::failure(std::move(valid_name.error()));
        }
    }
    std::lock_guard lock(mutex_);
    ParameterListResult result;
    std::set<std::string> names;
    std::set<std::string> prefixes_out;
    for (const auto& entry : parameters_) {
        const std::string& name = entry.first;
        bool matches = prefixes.empty();
        for (const auto& prefix : prefixes) {
            if (prefix.empty()) {
                matches = true;
                break;
            }
            if (name == prefix) {
                matches = true;
                break;
            }
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
                name[prefix.size()] == '.') {
                matches = true;
                break;
            }
        }
        if (!matches) continue;
        if (depth == kUnlimitedDepth) {
            names.insert(name);
            continue;
        }
        const auto tokens = count_tokens(name);
        if (tokens <= depth) {
            names.insert(name);
        } else {
            prefixes_out.insert(truncate_to_depth(name, depth));
        }
    }
    result.names.assign(names.begin(), names.end());
    result.prefixes.assign(prefixes_out.begin(), prefixes_out.end());
    return Result<ParameterListResult>::success(std::move(result));
}

Result<void> ParameterStoreState::validate(const std::vector<Parameter>& parameters) const {
    auto shape = validate_name_and_duplicates(parameters);
    if (!shape) return shape;
    std::lock_guard lock(mutex_);
    for (const auto& parameter : parameters) {
        auto validated = validate_assignment_locked(parameter.name, parameter.value, false);
        if (!validated) return validated;
    }
    return Result<void>::success();
}

Result<ParameterChangeSet> ParameterStoreState::set_atomically(
    const std::vector<Parameter>& parameters) {
    auto shape = validate_name_and_duplicates(parameters);
    if (!shape) return Result<ParameterChangeSet>::failure(std::move(shape.error()));

    std::lock_guard lock(mutex_);
    for (const auto& parameter : parameters) {
        auto validated = validate_assignment_locked(parameter.name, parameter.value, false);
        if (!validated) {
            return Result<ParameterChangeSet>::failure(std::move(validated.error()));
        }
    }

    // Prepare every allocation before the commit point so a failed set leaves
    // all previous values untouched.
    auto candidate_parameters = parameters_;
    ParameterChangeSet delta;
    delta.new_parameters.reserve(parameters.size());
    delta.changed_parameters.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        const auto found = candidate_parameters.find(parameter.name);
        if (found == candidate_parameters.end()) {
            candidate_parameters.emplace(
                parameter.name, StoredParameter{parameter.value, ParameterDescriptor{}});
            delta.new_parameters.push_back(parameter);
            continue;
        }
        if (found->second.value == parameter.value) continue;
        found->second.value = parameter.value;
        delta.changed_parameters.push_back(parameter);
    }

    auto candidate_pending = pending_changes_;
    candidate_pending.new_parameters.insert(
        candidate_pending.new_parameters.end(), delta.new_parameters.begin(),
        delta.new_parameters.end());
    candidate_pending.changed_parameters.insert(
        candidate_pending.changed_parameters.end(), delta.changed_parameters.begin(),
        delta.changed_parameters.end());

    parameters_.swap(candidate_parameters);
    pending_changes_.new_parameters.swap(candidate_pending.new_parameters);
    pending_changes_.changed_parameters.swap(candidate_pending.changed_parameters);
    pending_changes_.deleted_parameters.swap(candidate_pending.deleted_parameters);
    return Result<ParameterChangeSet>::success(std::move(delta));
}

Result<ParameterChangeSet> ParameterStoreState::take_changes() {
    std::lock_guard lock(mutex_);
    ParameterChangeSet result;
    result.new_parameters.swap(pending_changes_.new_parameters);
    result.changed_parameters.swap(pending_changes_.changed_parameters);
    result.deleted_parameters.swap(pending_changes_.deleted_parameters);
    return Result<ParameterChangeSet>::success(std::move(result));
}

}  // namespace dmw::impl
