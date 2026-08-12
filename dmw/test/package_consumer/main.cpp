// SPDX-License-Identifier: Apache-2.0

#include "dmw/context.hpp"

int main() {
    dmw::ContextOptions options;
    options.compatibility_profile = dmw::CompatibilityProfile::NativeDds;
    return options.domain_id == 0 ? 0 : 1;
}
