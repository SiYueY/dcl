#ifndef DCLPY_ROSIDL_PROVIDER_ROSIDL_PROVIDER_HPP_
#define DCLPY_ROSIDL_PROVIDER_ROSIDL_PROVIDER_HPP_

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <fastcdr/config.h>
#include <rosidl_typesupport_fastrtps_cpp/message_type_support.h>

#include "dclpy/compatibility.hpp"
#include "dclpy/diagnostic.hpp"
#include "dclpy/interface_binding.h"
#include "dmw/fastdds/message_type.hpp"

namespace dclpy::provider {
namespace py = pybind11;

template<class Binding>
const Binding* dependency_binding(py::handle capsule, const char* capsule_name) {
    auto* pointer = PyCapsule_GetPointer(capsule.ptr(), capsule_name);
    if (!pointer) throw py::error_already_set();
    const auto* header = static_cast<const DclpyBindingHeaderV1*>(pointer);
    if (header->abi_version != DCLPY_INTERFACE_BINDING_ABI ||
        header->struct_size < sizeof(Binding) || !header->compatibility_id ||
        std::strcmp(header->compatibility_id, interface_compatibility_id()) != 0)
        throw py::import_error("Incompatible DCLPY dependency descriptor");
    return static_cast<const Binding*>(pointer);
}

inline void diagnostic(DclpyBindingErrorV1* error, DclpyBindingErrorCodeV1 code,
                       const char* message) noexcept {
    if (!error) return;
    error->code = code;
    copy_diagnostic(error->message, message);
}

inline void require_descriptor(bool condition, const char* diagnostic) {
    if (!condition) throw py::import_error(diagnostic);
}

template <typename Sample, typename Tag>
class RosTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    RosTopicDataType() {
        const auto* handle = Tag::support();
        require_descriptor(handle && handle->data, "Missing ROSIDL Fast DDS type support");
        callbacks_ = static_cast<const message_type_support_callbacks_t*>(handle->data);
        require_descriptor(callbacks_->cdr_serialize && callbacks_->cdr_deserialize &&
                               callbacks_->get_serialized_size,
                           "Incomplete ROSIDL serialization callbacks");
        setName(Tag::wire_name());
        m_isGetKeyDefined = false;
        // Initial allocation hint, not an upper bound. Unbounded messages use
        // the descriptor's exact size provider and DMW's reallocating history.
        Sample initial;
        const auto size = callbacks_->get_serialized_size(&initial);
        if (size > std::numeric_limits<std::uint32_t>::max() - 4U)
            throw py::import_error("ROSIDL default serialized size overflows uint32");
        m_typeSize = size + 4U;
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        try {
            eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(payload->data), payload->max_size);
#if FASTCDR_VERSION_MAJOR >= 2
            eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                     eprosima::fastcdr::DDS_CDR);
#else
            eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                     eprosima::fastcdr::Cdr::DDS_CDR);
#endif
            cdr.serialize_encapsulation();
            if (!callbacks_->cdr_serialize(data, cdr)) return false;
#if FASTCDR_VERSION_MAJOR >= 2
            const auto length = cdr.get_serialized_data_length();
#else
            const auto length = cdr.getSerializedDataLength();
#endif
            if (length > std::numeric_limits<std::uint32_t>::max()) return false;
            payload->length = static_cast<std::uint32_t>(length);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        try {
            eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(payload->data), payload->length);
#if FASTCDR_VERSION_MAJOR >= 2
            eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                     eprosima::fastcdr::DDS_CDR);
#else
            eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                     eprosima::fastcdr::Cdr::DDS_CDR);
#endif
            cdr.read_encapsulation();
            return callbacks_->cdr_deserialize(cdr, data);
        } catch (...) {
            return false;
        }
    }
    std::function<std::uint32_t()> getSerializedSizeProvider(void* data) override {
        const auto size = callbacks_->get_serialized_size(data);
        if (size > std::numeric_limits<std::uint32_t>::max() - 4U)
            throw std::length_error("ROSIDL serialized size overflows uint32");
        return [size] { return size + 4U; };
    }
    void* createData() override { return new Sample(); }
    void deleteData(void* sample) override { delete static_cast<Sample*>(sample); }
    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }

private:
    const message_type_support_callbacks_t* callbacks_;
};

template <typename Sample, typename Tag>
struct MessageAdapter {
    inline static std::optional<dmw::MessageType> descriptor;
    inline static std::optional<DclpyMessageBindingV1> binding;

    static bool is_instance(PyObject* object) noexcept {
        try { return py::isinstance<Sample>(py::handle(object)); }
        catch (...) { PyErr_Clear(); return false; }
    }
    static void* sample_ptr(PyObject* object) noexcept {
        try {
            if (!is_instance(object)) return nullptr;
            return &py::cast<Sample&>(py::handle(object));
        } catch (...) { PyErr_Clear(); return nullptr; }
    }
    static PyObject* create_instance() noexcept {
        try { return py::cast(Sample(), py::return_value_policy::move).release().ptr(); }
        catch (py::error_already_set& error) { error.restore(); }
        catch (const std::bad_alloc&) { PyErr_NoMemory(); }
        catch (const std::exception& error) { PyErr_SetString(PyExc_RuntimeError, error.what()); }
        catch (...) { PyErr_SetString(PyExc_RuntimeError, "Message construction failed"); }
        return nullptr;
    }
    static void* clone_sample(const void* source, DclpyBindingErrorV1* error) noexcept {
        if (!source) {
            diagnostic(error, DclpyBindingErrorCodeV1::InvalidArgument, "Cannot clone null sample");
            return nullptr;
        }
        try { return new Sample(*static_cast<const Sample*>(source)); }
        catch (const std::bad_alloc&) {
            diagnostic(error, DclpyBindingErrorCodeV1::ResourceExhausted, "Sample allocation failed");
        } catch (const std::exception& exception) {
            diagnostic(error, DclpyBindingErrorCodeV1::Internal, exception.what());
        } catch (...) {
            diagnostic(error, DclpyBindingErrorCodeV1::Internal, "Sample copy failed");
        }
        return nullptr;
    }
    static void destroy_sample(void* sample) noexcept { delete static_cast<Sample*>(sample); }
    static const dmw::MessageType* message_type() noexcept { return descriptor ? &*descriptor : nullptr; }

    static const DclpyMessageBindingV1* initialize(const char* qualified_name) {
        if (!descriptor) {
            auto type = dmw::fastdds::create_message_type<RosTopicDataType<Sample, Tag>, Sample>();
            if (!type) throw py::import_error(std::string(type.error().message()));
            descriptor.emplace(std::move(type).value());
            binding.emplace(DclpyMessageBindingV1{
                {DCLPY_INTERFACE_BINDING_ABI, sizeof(DclpyMessageBindingV1), interface_compatibility_id()},
                qualified_name, &message_type, &is_instance, &sample_ptr, &create_instance,
                &clone_sample, &destroy_sample});
        }
        return &*binding;
    }
};

template <typename Service>
struct ServiceAdapter {
    inline static std::optional<dmw::ServiceType> descriptor;
    inline static std::optional<DclpyServiceBindingV1> binding;
    static const dmw::ServiceType* service_type() noexcept { return descriptor ? &*descriptor : nullptr; }
    static const DclpyServiceBindingV1* initialize(const char* name,
        const DclpyMessageBindingV1* request, const DclpyMessageBindingV1* response) {
        if (binding) {
            if (binding->request != request || binding->response != response ||
                std::strcmp(binding->python_qualified_name, name) != 0)
                throw py::import_error("Service provider metadata changed after registration");
            return &*binding;
        }
        descriptor.emplace(*request->message_type(), *response->message_type());
        binding.emplace(DclpyServiceBindingV1{
            {DCLPY_INTERFACE_BINDING_ABI, sizeof(DclpyServiceBindingV1), interface_compatibility_id()},
            name, &service_type, request, response});
        return &*binding;
    }
};

template <typename Action>
struct ActionAdapter {
    inline static std::optional<dmw::ActionType> descriptor;
    inline static std::optional<DclpyActionBindingV1> binding;
    static const dmw::ActionType* action_type() noexcept { return descriptor ? &*descriptor : nullptr; }
    static const DclpyActionBindingV1* initialize(const char* name,
        const DclpyMessageBindingV1* goal, const DclpyMessageBindingV1* result,
        const DclpyMessageBindingV1* feedback, const DclpyServiceBindingV1* send_goal,
        const DclpyServiceBindingV1* cancel_goal, const DclpyServiceBindingV1* get_result,
        const DclpyMessageBindingV1* feedback_message, const DclpyMessageBindingV1* status_message) {
        if (binding) {
            if (binding->goal != goal || binding->result != result || binding->feedback != feedback ||
                binding->send_goal != send_goal || binding->cancel_goal != cancel_goal ||
                binding->get_result != get_result || binding->feedback_message != feedback_message ||
                binding->status_message != status_message || std::strcmp(binding->python_qualified_name, name) != 0)
                throw py::import_error("Action provider metadata changed after registration");
            return &*binding;
        }
        descriptor.emplace(*send_goal->service_type(), *cancel_goal->service_type(),
                           *get_result->service_type(), *feedback_message->message_type(),
                           *status_message->message_type());
        binding.emplace(DclpyActionBindingV1{
            {DCLPY_INTERFACE_BINDING_ABI, sizeof(DclpyActionBindingV1), interface_compatibility_id()},
            name, &action_type, goal, result, feedback, send_goal, cancel_goal, get_result,
            feedback_message, status_message});
        return &*binding;
    }
};

}  // namespace dclpy::provider

#endif  // DCLPY_ROSIDL_PROVIDER_ROSIDL_PROVIDER_HPP_
