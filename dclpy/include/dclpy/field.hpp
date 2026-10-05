#ifndef DCLPY_ROSIDL_PROVIDER_FIELD_HPP_
#define DCLPY_ROSIDL_PROVIDER_FIELD_HPP_

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace dclpy::provider {
namespace py = pybind11;

struct FieldRules {
    std::size_t maximum_size{std::numeric_limits<std::size_t>::max()};
    std::size_t element_maximum_size{std::numeric_limits<std::size_t>::max()};
};

template <typename T, typename = void>
struct IsSequence : std::false_type {};
template <typename T>
struct IsSequence<T, std::void_t<typename T::value_type, decltype(std::declval<T>().begin()),
                               decltype(std::declval<T>().end())>> : std::true_type {};
template <typename T>
struct IsArray : std::false_type {};
template <typename T, std::size_t N>
struct IsArray<std::array<T, N>> : std::true_type {};
template <typename T>
struct IsString : std::false_type {};
template <typename C, typename Traits, typename Allocator>
struct IsString<std::basic_string<C, Traits, Allocator>> : std::true_type {};

[[noreturn]] inline void overflow() {
    PyErr_SetString(PyExc_OverflowError, "Message integer is outside the field's range");
    throw py::error_already_set();
}

template <typename T>
T decode(py::handle value, FieldRules rules = {}) {
    if constexpr (std::is_same_v<T, bool>) {
        if (!PyBool_Check(value.ptr())) throw py::type_error("Boolean field requires bool");
        return value.ptr() == Py_True;
    } else if constexpr (std::is_integral_v<T>) {
        if (!PyLong_Check(value.ptr())) throw py::type_error("Integer field requires int");
        if constexpr (std::is_signed_v<T>) {
            auto integer = PyLong_AsLongLong(value.ptr());
            if (PyErr_Occurred()) throw py::error_already_set();
            if (integer < std::numeric_limits<T>::min() || integer > std::numeric_limits<T>::max()) overflow();
            return static_cast<T>(integer);
        } else {
            auto integer = PyLong_AsUnsignedLongLong(value.ptr());
            if (PyErr_Occurred()) throw py::error_already_set();
            if (integer > std::numeric_limits<T>::max()) overflow();
            return static_cast<T>(integer);
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!PyFloat_Check(value.ptr()) && !PyLong_Check(value.ptr()))
            throw py::type_error("Floating field requires a real number");
        return py::cast<T>(value);
    } else if constexpr (IsString<T>::value) {
        if (!PyUnicode_Check(value.ptr())) throw py::type_error("String field requires str");
        auto staged = py::cast<T>(value);
        if (staged.size() > rules.maximum_size) throw py::value_error("String exceeds its bound");
        return staged;
    } else if constexpr (IsSequence<T>::value) {
        if (!PySequence_Check(value.ptr()) || PyUnicode_Check(value.ptr()))
            throw py::type_error("Array or sequence field requires a sequence");
        auto sequence = py::reinterpret_borrow<py::sequence>(value);
        const auto size = sequence.size();
        if (size > rules.maximum_size) throw py::value_error("Sequence exceeds its bound");
        T staged{};
        if constexpr (IsArray<T>::value) {
            if (size != staged.size()) throw py::value_error("Array has the wrong length");
            for (std::size_t index = 0; index < size; ++index)
                staged[index] = decode<typename T::value_type>(sequence[index], {rules.element_maximum_size});
        } else {
            staged.reserve(size);
            for (const auto& item : sequence)
                staged.push_back(decode<typename T::value_type>(item, {rules.element_maximum_size}));
        }
        return staged;
    } else {
        if (!py::isinstance<T>(value)) throw py::type_error("Nested field has the wrong generated type");
        return py::cast<T>(value);
    }
}

template <typename Message, typename Field>
void assign(Message& message, Field Message::* member, py::handle value, FieldRules rules = {}) {
    static_assert(std::is_nothrow_swappable_v<Field>, "Generated field requires a noexcept commit");
    auto staged = decode<Field>(value, rules);
    using std::swap;
    swap(message.*member, staged);
}

template <typename Message, typename Field>
void field(py::class_<Message>& type, const char* name, Field Message::* member, FieldRules rules = {}) {
    if constexpr (IsSequence<Field>::value && !IsString<Field>::value) {
        if constexpr (std::is_same_v<typename Field::value_type, std::uint8_t>) {
            // Keep the list API compatible, with an O(n) byte-copy accessor for
            // image payloads that avoids one Python integer per byte.
            type.def_property_readonly((std::string(name) + "_bytes").c_str(),
                [member](const Message& message) {
                    const auto& data = message.*member;
                    return py::bytes(reinterpret_cast<const char*>(data.data()), data.size());
                });
        }
    }
    type.def_property(name,
        [member](py::object parent) -> py::object {
            auto& message = parent.cast<Message&>();
            if constexpr (std::is_arithmetic_v<Field> || IsString<Field>::value) {
                // Explicit copy policy also detaches message elements in a
                // sequence; callers cannot mutate the parent's storage here.
                return py::cast(message.*member, py::return_value_policy::copy);
            } else if constexpr (IsSequence<Field>::value) {
                py::list result;
                for (const auto& element : message.*member)
                    result.append(py::cast(static_cast<typename Field::value_type>(element),
                                           py::return_value_policy::copy));
                return result;
            } else {
                return py::cast(&(message.*member), py::return_value_policy::reference_internal, parent);
            }
        },
        [member, rules](Message& message, py::handle value) { assign(message, member, value, rules); });
}

}  // namespace dclpy::provider

#endif  // DCLPY_ROSIDL_PROVIDER_FIELD_HPP_
