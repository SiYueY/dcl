#include <cassert>
#include <cstring>
#include <functional>

#include "dmw/fastdds/message_type.hpp"
#include "impl/temporary_sample.hpp"

namespace {

class IntTopicDataType : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() { setName("dmw.test.IntTopicDataType"); }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
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
    auto type = dmw::fastdds::make_message_type<IntTopicDataType>();
    assert(type);
    auto sample = dmw::impl::TemporarySample::create(type.value());
    assert(sample);
    *static_cast<int*>(sample.value().data()) = 42;

    int output = 0;
    assert(sample.value().commit_to(&output));
    assert(output == 42);
    return 0;
}
