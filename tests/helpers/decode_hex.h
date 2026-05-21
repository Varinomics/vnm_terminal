#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <cstddef>
#include <string_view>

namespace vnm_terminal::test_helpers {

inline QByteArray decode_hex(std::string_view hex)
{
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(hex.size() / 2U));

    const auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }

        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }

        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }

        return -1;
    };

    if ((hex.size() % 2U) != 0U) {
        return {};
    }

    for (std::size_t i = 0U; i < hex.size(); i += 2U) {
        const int high = hex_value(hex[i]);
        const int low  = hex_value(hex[i + 1U]);
        if (high < 0 || low < 0) {
            return {};
        }

        bytes.append(static_cast<char>((high << 4) | low));
    }

    return bytes;
}

}
