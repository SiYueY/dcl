#!/usr/bin/env python3
"""Generate the unique DCLPY binding provider for each ROSIDL package.

Run with the ROS installation's build-tools Python after sourcing its setup.
The emitted extensions use the independently selected CMake Python ABI.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
from typing import Any

from rosidl_parser.definition import (
    Action, Service, Message, NamespacedType, AbstractNestedType,
    IdlLocator, Include, Array,
)
from rosidl_parser.parser import parse_idl_file


def snake(name: str) -> str:
    name = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name).lower()


def key(namespaced: NamespacedType) -> str:
    return "::".join(namespaced.namespaced_name())


def variable(namespaced: NamespacedType) -> str:
    return "_".join(namespaced.namespaced_name())


def qualified(namespaced: NamespacedType) -> str:
    package, group = namespaced.namespaces
    return f"{package}_dclpy.{group}.{namespaced.name}"


@dataclass
class Package:
    name: str
    directory: Path
    messages: dict[str, Message] = field(default_factory=dict)
    services: dict[str, Service] = field(default_factory=dict)
    actions: list[Action] = field(default_factory=list)
    includes: set[str] = field(default_factory=set)
    dependencies: set[str] = field(default_factory=set)

    def message(self, value: Message) -> None:
        self.messages[key(value.structure.namespaced_type)] = value
        for member in value.structure.members:
            element = member.type
            while isinstance(element, AbstractNestedType):
                element = element.value_type
            if isinstance(element, NamespacedType) and element.namespaces[0] != self.name:
                self.dependencies.add(element.namespaces[0])

    def service(self, value: Service) -> None:
        self.services[key(value.namespaced_type)] = value
        self.message(value.request_message)
        self.message(value.response_message)


def read_package(directory: Path) -> Package:
    package = Package(directory.name, directory)
    files = sorted(directory.glob("*/*.idl"))
    if not files:
        raise ValueError(f"No installed ROSIDL files in {directory}")
    for path in files:
        relative = path.relative_to(directory)
        stem = snake(path.stem)
        group = relative.parts[0]
        if group not in {"msg", "srv", "action"}:
            continue
        package.includes.update({f"{package.name}/{group}/{stem}.hpp",
            f"{package.name}/{group}/detail/{stem}__rosidl_typesupport_fastrtps_cpp.hpp"})
        content = parse_idl_file(IdlLocator(directory, relative)).content
        for element in content.elements:
            if isinstance(element, Message):
                package.message(element)
            elif isinstance(element, Service):
                package.service(element)
            elif isinstance(element, Action):
                package.actions.append(element)
                for message in (element.goal, element.result, element.feedback, element.feedback_message):
                    package.message(message)
                package.service(element.send_goal_service)
                package.service(element.get_result_service)
                # Standard Cancel/Status are owned by action_msgs, never bound
                # a second time inside an action provider.
                package.dependencies.add("action_msgs")
            elif not isinstance(element, Include):
                raise ValueError(f"Unsupported top-level ROSIDL element {type(element).__name__} in {path}")
    return package


def field_rules(kind: Any) -> str:
    bound = getattr(kind, "maximum_size", None)
    if isinstance(kind, Array):
        bound = kind.size
    element_bound = getattr(getattr(kind, "value_type", None), "maximum_size", None)
    maximum = "std::numeric_limits<std::size_t>::max()"
    return "{" + (str(bound) if bound is not None else maximum) + ", " + (
        str(element_bound) if element_bound is not None else maximum) + "}"


def foreign_binding(namespaced: NamespacedType, capsule_kind: str) -> str:
    package, group = namespaced.namespaces
    capsule_name = f"dclpy.{capsule_kind}BindingV1"
    attribute = f"__dclpy_{capsule_kind.lower()}_binding__"
    cpp_type = f"Dclpy{capsule_kind}BindingV1"
    return (f'dependency_binding<{cpp_type}>('
            f'dep_{package}.attr("{group}").attr("{namespaced.name}").attr("{attribute}"), '
            f'"{capsule_name}")')


def emit_package(package: Package, destination: Path) -> None:
    output = destination / f"{package.name}_dclpy"
    output.mkdir(parents=True, exist_ok=True)
    cpp = ["// Generated from installed ROSIDL. Do not hand edit.",
           '#include "dclpy/rosidl_provider.hpp"',
           '#include "dclpy/field.hpp"']
    cpp += [f"#include <{include}>" for include in sorted(package.includes)]
    cpp += ["namespace py = pybind11;", "using namespace dclpy::provider;"]
    for message in package.messages.values():
        typename = message.structure.namespaced_type
        package_name, group = typename.namespaces
        tag = variable(typename) + "_tag"
        cpp += [f"struct {tag} {{",
                "static const rosidl_message_type_support_t* support() {",
                "return ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME("
                f"rosidl_typesupport_fastrtps_cpp, {package_name}, {group}, {typename.name})(); }}",
                f'static const char* wire_name() {{ return "{package_name}::{group}::dds_::{typename.name}_"; }}',
                "};"]
    cpp += [f"PYBIND11_MODULE(_{package.name}, module) {{", "py::list dependencies;",
            'module.attr("__dclpy_compatibility_id__") = dclpy::interface_compatibility_id();']
    for dependency in sorted(package.dependencies):
        cpp += [f'auto dep_{dependency} = py::module_::import("{dependency}_dclpy");',
                f'if (py::cast<std::string>(dep_{dependency}.attr("__dclpy_compatibility_id__")) != dclpy::interface_compatibility_id()) '
                'throw py::import_error("Incompatible DCLPY dependency module");',
                f"dependencies.append(dep_{dependency});"]
    cpp += ['module.attr("__dclpy_dependencies__") = dependencies;']
    groups: dict[str, list[str]] = {"msg": [], "srv": [], "action": []}
    for message in package.messages.values():
        typename = message.structure.namespaced_type
        typ = key(typename)
        var = variable(typename)
        cls = var + "_class"
        groups[typename.namespaces[1]].append(typename.name)
        cpp += [f'auto {cls} = py::class_<{typ}>(module, "{typename.name}");',
                f"{cls}.def(py::init([](py::kwargs kwargs) {{",
                f"auto sample = std::make_unique<{typ}>();"]
        allowed = " || ".join(f'key == "{member.name}"' for member in message.structure.members) or "false"
        cpp += ["for (const auto& item : kwargs) {", "auto key = py::cast<std::string>(item.first);",
                f'if (!({allowed})) throw py::type_error("Unknown message field: " + key);', "}"]
        for member in message.structure.members:
            cpp += [f'if (kwargs.contains("{member.name}")) assign(*sample, &{typ}::{member.name}, '
                    f'kwargs["{member.name}"], {field_rules(member.type)});']
        cpp += ["return sample; }));"]
        for member in message.structure.members:
            cpp += [f'field({cls}, "{member.name}", &{typ}::{member.name}, {field_rules(member.type)});']
        for constant in message.constants:
            cpp += [f'{cls}.attr("{constant.name}") = py::cast({typ}::{constant.name});']
        cpp += [f'auto* {var}_binding = MessageAdapter<{typ}, {var}_tag>::initialize("{qualified(typename)}");',
                f'{cls}.attr("__dclpy_message_binding__") = py::capsule({var}_binding, "dclpy.MessageBindingV1");']
    for service in package.services.values():
        typename = service.namespaced_type
        typ, var = key(typename), variable(typename)
        request = variable(service.request_message.structure.namespaced_type)
        response = variable(service.response_message.structure.namespaced_type)
        groups[typename.namespaces[1]].append(typename.name)
        cpp += [f'auto {var}_class = py::class_<{typ}>(module, "{typename.name}");',
                f'{var}_class.attr("Request") = {request}_class;',
                f'{var}_class.attr("Response") = {response}_class;',
                f'auto* {var}_binding = ServiceAdapter<{typ}>::initialize("{qualified(typename)}", '
                f'{request}_binding, {response}_binding);',
                f'{var}_class.attr("__dclpy_service_binding__") = py::capsule({var}_binding, "dclpy.ServiceBindingV1");']
    for action in package.actions:
        typename = action.namespaced_type
        typ, var = key(typename), variable(typename)
        goal, result, feedback, feedback_message = [variable(m.structure.namespaced_type)
            for m in (action.goal, action.result, action.feedback, action.feedback_message)]
        send, get = [variable(s.namespaced_type) for s in (action.send_goal_service, action.get_result_service)]
        groups["action"].append(typename.name)
        cancel_type = NamespacedType(["action_msgs", "srv"], "CancelGoal")
        status_type = NamespacedType(["action_msgs", "msg"], "GoalStatusArray")
        cpp += [f'auto {var}_class = py::class_<{typ}>(module, "{typename.name}");',
                f'auto {var}_impl = py::class_<{typ}::Impl>(module, "{typename.name}_Impl");',
                f'{var}_class.attr("Goal") = {goal}_class;', f'{var}_class.attr("Result") = {result}_class;',
                f'{var}_class.attr("Feedback") = {feedback}_class;',
                f'{var}_class.attr("Impl") = {var}_impl;',
                f'{var}_impl.attr("SendGoalService") = {send}_class;',
                f'{var}_impl.attr("GetResultService") = {get}_class;',
                f'{var}_impl.attr("CancelGoalService") = dep_action_msgs.attr("srv").attr("CancelGoal");',
                f'{var}_impl.attr("FeedbackMessage") = {feedback_message}_class;',
                f'{var}_impl.attr("GoalStatusMessage") = dep_action_msgs.attr("msg").attr("GoalStatusArray");',
                f'auto* {var}_binding = ActionAdapter<{typ}>::initialize("{qualified(typename)}", '
                f'{goal}_binding, {result}_binding, {feedback}_binding, {send}_binding, '
                f'{foreign_binding(cancel_type, "Service")}, {get}_binding, {feedback_message}_binding, '
                f'{foreign_binding(status_type, "Message")});',
                f'{var}_class.attr("__dclpy_action_binding__") = py::capsule({var}_binding, "dclpy.ActionBindingV1");']
    cpp += ["}"]
    (output / "binding.cpp").write_text("\n".join(cpp) + "\n")
    for group, names in groups.items():
        path = output / group
        path.mkdir(exist_ok=True)
        source = f"from .._{package.name} import " + ", ".join(names) + "\n" if names else ""
        (path / "__init__.py").write_text(source)
    (output / "__init__.py").write_text(f"from ._{package.name} import __dclpy_compatibility_id__\nfrom . import msg, srv, action\n")


def generate(packages: list[str], prefixes: list[Path], destination: Path) -> None:
    loaded: dict[str, Package] = {}
    visiting: set[str] = set()
    order: list[Package] = []

    def visit(name: str) -> None:
        if name in visiting:
            raise ValueError(f"Cyclic ROSIDL package dependency involving {name}")
        if name in loaded:
            return
        directory = next((prefix / "share" / name for prefix in prefixes
                          if (prefix / "share" / name).is_dir()), None)
        if directory is None:
            raise ValueError(f"No installed ROSIDL package {name} in the supplied prefixes")
        visiting.add(name)
        package = read_package(directory)
        for dependency in sorted(package.dependencies):
            visit(dependency)
        visiting.remove(name)
        loaded[name] = package
        order.append(package)

    for name in packages:
        visit(name)
    destination.mkdir(parents=True, exist_ok=True)
    cmake = ["cmake_minimum_required(VERSION 3.20)", "project(dclpy_interfaces LANGUAGES CXX)",
             "find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)",
             "find_package(pybind11 3.0 CONFIG REQUIRED)", "find_package(dmw 0.1 CONFIG REQUIRED)",
             "find_package(fastcdr CONFIG REQUIRED)",
             'set(DCLPY_INCLUDE_DIR "" CACHE PATH "DCLPY public binding headers")',
             'if(NOT EXISTS "${DCLPY_INCLUDE_DIR}/dclpy/interface_binding.h")',
             'message(FATAL_ERROR "DCLPY_INCLUDE_DIR must point to the shared binding headers")', "endif()",
             'set(DCLPY_ROS_PROFILE "humble" CACHE STRING "Frozen ROS interface profile")']
    for package in order:
        emit_package(package, destination)
        name = package.name
        target = name + "_dclpy"
        cmake += [f"find_package({name} CONFIG REQUIRED)",
                  f"pybind11_add_module({target} NO_EXTRAS {target}/binding.cpp)",
                  f"target_link_libraries({target} PRIVATE dmw::fastdds_binding "
                  f"{name}::{name}__rosidl_typesupport_fastrtps_cpp)",
                  f"target_include_directories({target} PRIVATE ${{DCLPY_INCLUDE_DIR}})",
                  f"target_compile_features({target} PRIVATE cxx_std_17)",
                  f'target_compile_definitions({target} PRIVATE DCLPY_ROS_PROFILE="${{DCLPY_ROS_PROFILE}}")',
                  f"target_compile_options({target} PRIVATE -Wall -Wextra -Wpedantic -Werror)",
                  f'set_target_properties({target} PROPERTIES OUTPUT_NAME _{name} '
                  'INSTALL_RPATH "$<TARGET_FILE_DIR:dmw::dmw>" INSTALL_RPATH_USE_LINK_PATH TRUE)',
                  f"install(TARGETS {target} LIBRARY DESTINATION {target})",
                  f'install(DIRECTORY {target}/ DESTINATION {target} FILES_MATCHING PATTERN "*.py")']
    (destination / "CMakeLists.txt").write_text("\n".join(cmake) + "\n")
    (destination / "manifest.json").write_text(json.dumps({
        "packages": [package.name for package in order],
        "interface_abi": 1,
        "sources": {package.name: str(package.directory) for package in order},
    }, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", action="append", required=True)
    parser.add_argument("--prefix", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.package, arguments.prefix, arguments.output)


if __name__ == "__main__":
    main()
