// SPDX-License-Identifier: Apache-2.0

#include <cassert>

#include "impl/name.hpp"

int main() {
    assert(dmw::impl::normalize_namespace("").value() == "/");
    assert(dmw::impl::normalize_namespace("robot").value() == "/robot");
    assert(dmw::impl::normalize_namespace("/robot").value() == "/robot");
    assert(!dmw::impl::normalize_namespace("/robot/"));
    assert(!dmw::impl::normalize_namespace("/robot//camera"));
    assert(!dmw::impl::normalize_namespace("~/robot"));
    assert(dmw::impl::resolve_name("/", "camera").value() == "/camera");
    assert(dmw::impl::resolve_name("/robot", "camera").value() == "/robot/camera");
    assert(dmw::impl::resolve_name("/robot", "/camera").value() == "/camera");
    assert(!dmw::impl::resolve_name("/robot", "camera/"));
    return 0;
}
