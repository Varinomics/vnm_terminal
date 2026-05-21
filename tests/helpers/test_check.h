#pragma once

#include <iostream>
#include <string_view>

namespace vnm_terminal::test_helpers {

inline bool check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    return true;
}

}
