#pragma once

#include <array>
#include <string_view>
#include <vector>

namespace vnm_terminal::internal {

constexpr int k_terminal_canvas_fixture_protocol_version = 1;

enum class Terminal_canvas_fixture_record_kind
{
    OUTPUT,
    EXPECT_INPUT,
    RESIZE,
    REPEAT_OUTPUT,
    CHECKPOINT,
    EXIT,
};

struct terminal_canvas_fixture_record_t
{
    Terminal_canvas_fixture_record_kind kind         = Terminal_canvas_fixture_record_kind::CHECKPOINT;
    std::string_view                    label;
    std::string_view                    payload_hex;
    int                                 rows         = 0;
    int                                 columns      = 0;
    int                                 repeat_count = 0;
    int                                 exit_code    = 0;
};

struct terminal_canvas_fixture_behavior_smoke_case_t
{
    std::string_view   name;
    std::string_view   payload_hex;
    int                repeat_count                  = 1;
};

struct terminal_canvas_fixture_shell_like_smoke_contract_t
{
    std::string_view   prompt;
    std::string_view   echo_command;
    std::string_view   echo_text;
    std::string_view   size_command;
    std::string_view   size_prefix;
    std::string_view   stream_command;
    std::string_view   gated_stream_command;
    std::string_view   gated_stream_ready_output;
    std::string_view   gated_stream_resized_prefix;
    std::string_view   gated_stream_continue_command;
    std::string_view   wait_command;
    std::string_view   wait_output;
    std::string_view   exit_command;
    int                stream_count     = 64;
    int                stream_max_count = 256;
};

inline std::string_view terminal_canvas_fixture_kind_name(
    Terminal_canvas_fixture_record_kind kind)
{
    switch (kind) {
        case Terminal_canvas_fixture_record_kind::OUTPUT:        return "output";
        case Terminal_canvas_fixture_record_kind::EXPECT_INPUT:  return "expect-input";
        case Terminal_canvas_fixture_record_kind::RESIZE:        return "resize";
        case Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT: return "repeat-output";
        case Terminal_canvas_fixture_record_kind::CHECKPOINT:    return "checkpoint";
        case Terminal_canvas_fixture_record_kind::EXIT:          return "exit";
    }

    return {};
}

inline std::string_view terminal_canvas_fixture_scenario_name()
{
    return "terminal-canvas";
}

inline constexpr std::string_view k_terminal_canvas_fixture_enable_input_modes_label =
    "enable-input-modes";

inline constexpr terminal_canvas_fixture_shell_like_smoke_contract_t
terminal_canvas_fixture_shell_like_smoke_contract()
{
    return {
        "vnm$ ",
        "echo",
        "surface-ok",
        "size",
        "size ",
        "stream",
        "stream-gated",
        "stream-gated-ready",
        "stream-gated-resized ",
        "stream-gated-continue",
        "wait",
        "waiting",
        "exit",
        64,
        256,
    };
}

inline const std::array<std::string_view, 15>& terminal_canvas_fixture_required_labels()
{
    static constexpr std::array<std::string_view, 15> labels = {
        "startup",
        "enter-alternate-screen",
        "prompt-editing-keys",
        "prompt-editing-keys-ack",
        "resize",
        "resize-report",
        "bracketed-paste",
        "bracketed-paste-ack",
        "focus-reporting",
        "focus-reporting-ack",
        "mouse-sgr-1006",
        "mouse-sgr-1006-ack",
        "reply-handling",
        "high-volume-streaming",
        "clean-exit",
    };

    return labels;
}

inline const std::vector<terminal_canvas_fixture_record_t>&
terminal_canvas_fixture_contract_script()
{
    static const std::vector<terminal_canvas_fixture_record_t> records = {
        {
            Terminal_canvas_fixture_record_kind::CHECKPOINT,
            "startup",
            {},
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "enter-alternate-screen",
            "1b5b3f3130343968",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "prompt",
            "7465726d3e20",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "prompt-editing-keys",
            "1b5b447f7465726d0d",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "prompt-editing-keys-ack",
            "696e7075742070726f6d70742d65646974696e672d6b657973206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::RESIZE,
            "resize",
            {},
            33,
            120,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "resize-report",
            "726573697a65203333783132300d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            k_terminal_canvas_fixture_enable_input_modes_label,
            "1b5b3f32303034681b5b3f31303034681b5b3f31303030681b5b3f31303032681b5b3f3130303668",
            0,
            0,
            0,
            0,
        },
        // Bracketed paste is expected only after the fixture has enabled mode
        // 2004, because hosts should frame paste input from terminal state.
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "bracketed-paste",
            "1b5b3230307e6c696e65310a6c696e65321b5b3230317e",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "bracketed-paste-ack",
            "696e70757420627261636b657465642d7061737465206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "focus-reporting",
            "1b5b491b5b4f",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "focus-reporting-ack",
            "696e70757420666f6375732d7265706f7274696e67206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "mouse-sgr-1006",
            "1b5b3c303b31323b384d1b5b3c303b31323b386d",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "mouse-sgr-1006-ack",
            "696e707574206d6f7573652d7367722d31303036206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "reply-handling",
            "1b5b32343b3830481b5b631b5b366e1b5b3f323030342470",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "reply-handling",
            "1b5b3f313b32631b5b32343b3830521b5b3f323030343b312479",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT,
            "high-volume-streaming",
            "73747265616d2d726f770d0a",
            0,
            0,
            4096,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "leave-alternate-screen",
            "1b5b3f313034396c",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXIT,
            "clean-exit",
            {},
            0,
            0,
            0,
            0,
        },
    };

    return records;
}

inline const std::vector<terminal_canvas_fixture_behavior_smoke_case_t>&
terminal_canvas_fixture_behavior_smoke_cases()
{
    // Byte-output smokes shared by the fixture and tests. Resize/output ordering
    // needs an explicit model resize and is covered in the behavior smoke tests.
    static const std::vector<terminal_canvas_fixture_behavior_smoke_case_t> cases = {
        {
            "cursor-addressing",
            "411b5b323b3348421b5b313b31484344",
        },
        {
            "erase-line-screen",
            "6162636465661b5b313b33481b5b4b0d0a3132333435361b5b323b34481b5b314b",
        },
        {
            "alternate-screen-1049",
            "501b5b323b34481b5b3f3130343968414c541b5b333b38485a1b5b3f313034396c42",
        },
        {
            "unicode-width",
            "41e7958c4265cc8143e29da4efb88e44e29da4efb88f45f09f988046cea947",
        },
        {
            "output-burst",
            "30313233343536373839616263646566",
            512,
        },
        {
            "sgr-reset-interactions",
            "1b5b33316d411b5b316d421b5b346d431b5b32326d441b5b33396d451b5b306d46",
        },
        {
            "decstbm-scroll-region",
            "1b5b313b31483131311b5b323b31483232321b5b333b31483333331b5b343b31483434341b5b323b33721b5b333b31480a",
        },
        {
            "primary-scrollback-insert",
            "1b5b313b3148746f702d6f6e651b5b323b3148746f702d74776f1b5b333b3148"
            "766965771b5b343b314862656c6f771b5b353b314870726f6d70741b5b343b3572"
            "1b5b343b31481b4d1b5b721b5b313b34721b5b333b31480d0a484953541b5b72"
            "1b5b353b31481b5b313b34721b5b343b31480d0a4e4558541b5b721b5b353b3148",
        },
    };

    return cases;
}

}
