#include "dmw/context.hpp"

int main() {
    dmw::ContextOptions options;
    options.runtime_mode = dmw::RuntimeMode::DDS;
    return options.domain_id == 0 ? 0 : 1;
}
