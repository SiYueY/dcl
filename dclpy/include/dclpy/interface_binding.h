#ifndef DCLPY_INTERFACE_BINDING_H_
#define DCLPY_INTERFACE_BINDING_H_

#include <Python.h>
#include <cstdint>

#include "dmw/action_type.hpp"
#include "dmw/message_type.hpp"
#include "dmw/service_type.hpp"

inline constexpr std::uint32_t DCLPY_INTERFACE_BINDING_ABI = 1;

struct DclpyBindingHeaderV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    const char* compatibility_id;
};

enum class DclpyBindingErrorCodeV1 : std::uint32_t {
    None = 0,
    InvalidArgument = 1,
    TypeMismatch = 2,
    ResourceExhausted = 3,
    Internal = 4
};

struct DclpyBindingErrorV1 {
    DclpyBindingErrorCodeV1 code;
    char message[256];
};

struct DclpyMessageBindingV1 {
    DclpyBindingHeaderV1 header;
    const char* python_qualified_name;
    const dmw::MessageType* (*message_type)() noexcept;
    bool (*is_instance)(PyObject*) noexcept;
    void* (*sample_ptr)(PyObject*) noexcept;
    PyObject* (*create_instance)() noexcept;
    void* (*clone_sample)(const void*, DclpyBindingErrorV1*) noexcept;
    void (*destroy_sample)(void*) noexcept;
};

struct DclpyServiceBindingV1 {
    DclpyBindingHeaderV1 header;
    const char* python_qualified_name;
    const dmw::ServiceType* (*service_type)() noexcept;
    const DclpyMessageBindingV1* request;
    const DclpyMessageBindingV1* response;
};

struct DclpyActionBindingV1 {
    DclpyBindingHeaderV1 header;
    const char* python_qualified_name;
    const dmw::ActionType* (*action_type)() noexcept;
    const DclpyMessageBindingV1* goal;
    const DclpyMessageBindingV1* result;
    const DclpyMessageBindingV1* feedback;
    const DclpyServiceBindingV1* send_goal;
    const DclpyServiceBindingV1* cancel_goal;
    const DclpyServiceBindingV1* get_result;
    const DclpyMessageBindingV1* feedback_message;
    const DclpyMessageBindingV1* status_message;
};

#endif  // DCLPY_INTERFACE_BINDING_H_
