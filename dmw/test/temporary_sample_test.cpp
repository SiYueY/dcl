#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <typeindex>

#include "dmw/error.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "impl/temporary_sample.hpp"

namespace {

const unsigned char* last_payload_data = nullptr;
std::size_t serialize_calls = 0;

dmw::Result<void> failing_commit(const void*, void*) noexcept {
    return dmw::Result<void>::failure(dmw::Error(dmw::ErrorCode::DDSError, "expected failure"));
}

class IntTopicDataType : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() { setName("dmw.test.IntTopicDataType"); }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        if (serialize_calls != 0) assert(payload->data == last_payload_data);
        last_payload_data = payload->data;
        ++serialize_calls;
        payload->length = sizeof(int);
        std::memcpy(payload->data, data, sizeof(int));
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        if (payload->length != sizeof(int)) {
            return false;
        }
        std::memcpy(data, payload->data, sizeof(int));
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return static_cast<std::uint32_t>(sizeof(int)); };
    }

    void* createData() override { return new int(0); }
    void deleteData(void* data) override { delete static_cast<int*>(data); }
    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

}  // namespace

int main() {
    auto type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(type);
    auto sample = dmw::impl::TemporarySample::create(type.value());
    assert(sample);
    *static_cast<int*>(sample.value().data()) = 42;

    int output = 0;
    assert(sample.value().commit_to(&output));
    assert(output == 42);
    *static_cast<int*>(sample.value().data()) = 7;
    assert(sample.value().commit_to(&output));
    assert(output == 7);
    // The transactional adapter commits the typed scratch directly; it must
    // not serialize or deserialize merely to protect the user output.
    assert(serialize_calls == 0);

    auto fallback_type = dmw::fastdds::create_message_type<IntTopicDataType>();
    assert(fallback_type);
    auto fallback_sample = dmw::impl::TemporarySample::create(fallback_type.value());
    assert(fallback_sample);
    *static_cast<int*>(fallback_sample.value().data()) = 13;
    assert(fallback_sample.value().commit_to(&output));
    assert(output == 13);
    assert(serialize_calls == 1);

    eprosima::fastdds::dds::TypeSupport support(new IntTopicDataType());
    auto failing_type = dmw::fastdds::MessageTypeAdapter::create(
        std::move(support), typeid(IntTopicDataType), &failing_commit);
    assert(failing_type);
    auto failing_sample = dmw::impl::TemporarySample::create(failing_type.value());
    assert(failing_sample);
    *static_cast<int*>(failing_sample.value().data()) = 99;
    output = 17;
    const auto failed = failing_sample.value().commit_to(&output);
    assert(!failed);
    assert(output == 17);
    return 0;
}
