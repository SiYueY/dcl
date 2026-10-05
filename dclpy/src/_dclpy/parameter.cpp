#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "context.hpp"
#include "dmw/parameter_descriptor.hpp"

namespace py = pybind11;
namespace dclpy::detail {

py::object parameter_value_to_python(const dmw::ParameterValue& value) {
    switch (value.type()) {
        case dmw::ParameterType::NotSet: return py::none();
        case dmw::ParameterType::Bool: return py::bool_(value.as_bool());
        case dmw::ParameterType::Integer: return py::int_(value.as_integer());
        case dmw::ParameterType::Double: return py::float_(value.as_double());
        case dmw::ParameterType::String: return py::str(value.as_string());
        case dmw::ParameterType::ByteArray: {
            const auto& bytes = value.as_byte_array();
            return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        case dmw::ParameterType::BoolArray: return py::cast(value.as_bool_array());
        case dmw::ParameterType::IntegerArray: return py::cast(value.as_integer_array());
        case dmw::ParameterType::DoubleArray: return py::cast(value.as_double_array());
        case dmw::ParameterType::StringArray: return py::cast(value.as_string_array());
    }
    throw std::logic_error("Unrecognized parameter type");
}

void bind_parameters(py::module_& module) {
    py::enum_<dmw::ParameterType>(module, "ParameterType")
        .value("NOT_SET", dmw::ParameterType::NotSet).value("BOOL", dmw::ParameterType::Bool)
        .value("INTEGER", dmw::ParameterType::Integer).value("DOUBLE", dmw::ParameterType::Double)
        .value("STRING", dmw::ParameterType::String).value("BYTE_ARRAY", dmw::ParameterType::ByteArray)
        .value("BOOL_ARRAY", dmw::ParameterType::BoolArray)
        .value("INTEGER_ARRAY", dmw::ParameterType::IntegerArray)
        .value("DOUBLE_ARRAY", dmw::ParameterType::DoubleArray)
        .value("STRING_ARRAY", dmw::ParameterType::StringArray);
    py::class_<dmw::ParameterValue>(module, "_ParameterValue")
        .def(py::init<>()).def_property_readonly("type", &dmw::ParameterValue::type)
        .def_property_readonly("value", &parameter_value_to_python)
        .def_static("bool", &dmw::ParameterValue::make_bool)
        .def_static("integer", &dmw::ParameterValue::make_integer)
        .def_static("double", &dmw::ParameterValue::make_double)
        .def_static("string", &dmw::ParameterValue::make_string)
        .def_static("byte_array", &dmw::ParameterValue::make_byte_array)
        .def_static("bool_array", &dmw::ParameterValue::make_bool_array)
        .def_static("integer_array", &dmw::ParameterValue::make_integer_array)
        .def_static("double_array", &dmw::ParameterValue::make_double_array)
        .def_static("string_array", &dmw::ParameterValue::make_string_array);
    py::class_<dmw::Parameter>(module, "_Parameter")
        .def(py::init<>()).def_readwrite("name", &dmw::Parameter::name)
        .def_readwrite("value", &dmw::Parameter::value);
    py::class_<dmw::IntegerRange>(module, "IntegerRange")
        .def(py::init<>()).def_readwrite("from_value", &dmw::IntegerRange::from_value)
        .def_readwrite("to_value", &dmw::IntegerRange::to_value)
        .def_readwrite("step", &dmw::IntegerRange::step);
    py::class_<dmw::FloatingPointRange>(module, "FloatingPointRange")
        .def(py::init<>()).def_readwrite("from_value", &dmw::FloatingPointRange::from_value)
        .def_readwrite("to_value", &dmw::FloatingPointRange::to_value)
        .def_readwrite("step", &dmw::FloatingPointRange::step);
    py::class_<dmw::ParameterDescriptor>(module, "ParameterDescriptor")
        .def(py::init<>()).def_readwrite("description", &dmw::ParameterDescriptor::description)
        .def_readwrite("additional_constraints", &dmw::ParameterDescriptor::additional_constraints)
        .def_readwrite("read_only", &dmw::ParameterDescriptor::read_only)
        .def_readwrite("dynamic_typing", &dmw::ParameterDescriptor::dynamic_typing)
        .def_readwrite("integer_ranges", &dmw::ParameterDescriptor::integer_ranges)
        .def_readwrite("floating_point_ranges", &dmw::ParameterDescriptor::floating_point_ranges);
    py::class_<dmw::ParameterChangeSet>(module, "_ParameterChangeSet")
        .def_readonly("new_parameters", &dmw::ParameterChangeSet::new_parameters)
        .def_readonly("changed_parameters", &dmw::ParameterChangeSet::changed_parameters)
        .def_readonly("deleted_parameters", &dmw::ParameterChangeSet::deleted_parameters);
    py::class_<dmw::ParameterListResult>(module, "ParameterListResult")
        .def_readonly("names", &dmw::ParameterListResult::names)
        .def_readonly("prefixes", &dmw::ParameterListResult::prefixes);

    auto node = py::reinterpret_borrow<py::object>(module.attr("_Node"));
    auto method = [&](const char* name, auto function) {
        node.attr(name) = py::cpp_function(function, py::name(name), py::is_method(node),
                                         py::call_guard<py::gil_scoped_release>());
    };
    method("declare_parameter", [](NativeNode& self, const std::string& name,
                                   const dmw::ParameterValue& value,
                                   const dmw::ParameterDescriptor& descriptor, bool ignore_override) {
        return unwrap(self.backing()->operation()->declare_parameter(name, value, descriptor,
                                                                      ignore_override));
    });
    method("undeclare_parameter", [](NativeNode& self, const std::string& name) {
        unwrap(self.backing()->operation()->undeclare_parameter(name));
    });
    method("get_parameter", [](NativeNode& self, const std::string& name) {
        return unwrap(self.backing()->operation()->get_parameter(name));
    });
    method("get_parameters", [](NativeNode& self, const std::vector<std::string>& names) {
        return unwrap(self.backing()->operation()->get_parameters(names));
    });
    method("has_parameter", [](NativeNode& self, const std::string& name) {
        return unwrap(self.backing()->operation()->has_parameter(name));
    });
    method("describe_parameter", [](NativeNode& self, const std::string& name) {
        return unwrap(self.backing()->operation()->describe_parameter(name));
    });
    method("list_parameters", [](NativeNode& self, const std::vector<std::string>& prefixes,
                                 std::size_t depth) {
        return unwrap(self.backing()->operation()->list_parameters(prefixes, depth));
    });
    method("validate_parameters", [](NativeNode& self, const std::vector<dmw::Parameter>& values) {
        unwrap(self.backing()->operation()->validate_parameters(values));
    });
    method("set_parameters_atomically", [](NativeNode& self, const std::vector<dmw::Parameter>& values) {
        return unwrap(self.backing()->operation()->set_parameters_atomically(values));
    });
    method("take_parameter_changes", [](NativeNode& self) {
        return unwrap(self.backing()->operation()->take_parameter_changes());
    });
}

}  // namespace dclpy::detail
