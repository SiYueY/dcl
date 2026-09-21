#ifndef DMW_PARAMETER_HPP_
#define DMW_PARAMETER_HPP_

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dmw {

/// Parameter value kinds shared by every DCL language binding.
enum class ParameterType {
    NotSet,
    Bool,
    Integer,
    Double,
    String,
    ByteArray,
    BoolArray,
    IntegerArray,
    DoubleArray,
    StringArray
};

/// Type-tagged parameter value with checked accessors.
///
/// Reading a value through the wrong accessor is a programming precondition
/// violation and terminates, matching the project's other value types.  It is
/// never reported as an ordinary runtime Error.
class ParameterValue {
public:
    using ByteArray = std::vector<std::uint8_t>;
    using BoolArray = std::vector<bool>;
    using IntegerArray = std::vector<std::int64_t>;
    using DoubleArray = std::vector<double>;
    using StringArray = std::vector<std::string>;

    ParameterValue() noexcept = default;

    static ParameterValue make_bool(bool value) { return ParameterValue(value); }
    static ParameterValue make_integer(std::int64_t value) { return ParameterValue(value); }
    static ParameterValue make_double(double value) { return ParameterValue(value); }
    static ParameterValue make_string(std::string value) {
        return ParameterValue(std::move(value));
    }
    static ParameterValue make_byte_array(ByteArray value) {
        return ParameterValue(std::move(value));
    }
    static ParameterValue make_bool_array(BoolArray value) {
        return ParameterValue(std::move(value));
    }
    static ParameterValue make_integer_array(IntegerArray value) {
        return ParameterValue(std::move(value));
    }
    static ParameterValue make_double_array(DoubleArray value) {
        return ParameterValue(std::move(value));
    }
    static ParameterValue make_string_array(StringArray value) {
        return ParameterValue(std::move(value));
    }

    ParameterType type() const noexcept {
        return static_cast<ParameterType>(storage_.index());
    }

    /// False only for a NotSet value; every array type counts as set.
    bool is_set() const noexcept { return type() != ParameterType::NotSet; }

    bool as_bool() const {
        require_type(ParameterType::Bool);
        return std::get<bool>(storage_);
    }

    std::int64_t as_integer() const {
        require_type(ParameterType::Integer);
        return std::get<std::int64_t>(storage_);
    }

    double as_double() const {
        require_type(ParameterType::Double);
        return std::get<double>(storage_);
    }

    std::string_view as_string() const {
        require_type(ParameterType::String);
        return std::get<std::string>(storage_);
    }

    const ByteArray& as_byte_array() const {
        require_type(ParameterType::ByteArray);
        return std::get<ByteArray>(storage_);
    }

    const BoolArray& as_bool_array() const {
        require_type(ParameterType::BoolArray);
        return std::get<BoolArray>(storage_);
    }

    const IntegerArray& as_integer_array() const {
        require_type(ParameterType::IntegerArray);
        return std::get<IntegerArray>(storage_);
    }

    const DoubleArray& as_double_array() const {
        require_type(ParameterType::DoubleArray);
        return std::get<DoubleArray>(storage_);
    }

    const StringArray& as_string_array() const {
        require_type(ParameterType::StringArray);
        return std::get<StringArray>(storage_);
    }

    friend bool operator==(const ParameterValue& lhs, const ParameterValue& rhs) noexcept {
        return lhs.storage_ == rhs.storage_;
    }

    friend bool operator!=(const ParameterValue& lhs, const ParameterValue& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    // The variant alternative order must stay aligned with ParameterType.
    using Storage = std::variant<
        std::monostate, bool, std::int64_t, double, std::string, ByteArray, BoolArray,
        IntegerArray, DoubleArray, StringArray>;

    explicit ParameterValue(Storage storage) : storage_(std::move(storage)) {}

    void require_type(ParameterType expected) const {
        if (type() != expected) std::terminate();
    }

    Storage storage_{};
};

struct Parameter {
    std::string name;
    ParameterValue value;
};

inline bool operator==(const Parameter& lhs, const Parameter& rhs) noexcept {
    return lhs.name == rhs.name && lhs.value == rhs.value;
}

inline bool operator!=(const Parameter& lhs, const Parameter& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace dmw

#endif  // DMW_PARAMETER_HPP_
