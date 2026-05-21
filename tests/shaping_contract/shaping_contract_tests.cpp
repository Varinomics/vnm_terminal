#include "vnm_terminal/internal/cell_stable_shaping.h"
#include "helpers/test_check.h"

#include <QRectF>
#include <QSizeF>
#include <QString>
#include <iostream>
#include <limits>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::terminal_cell_metrics_t metrics()
{
    return {10.0, 20.0, 14.0, 6.0};
}

term::Terminal_cell_shaping_input cell(
    int                                        column,
    QString                                    text,
    int                                        display_width = 1,
    term::Terminal_shaped_presentation_mode    presentation = term::Terminal_shaped_presentation_mode::DEFAULT_TEXT)
{
    term::Terminal_cell_shaping_input input;
    input.column                                  = column;
    input.text                                    = text;
    input.display_width                           = display_width;
    input.presentation_mode                       = presentation;
    input.style_id                                = 3U;
    input.hyperlink_id                            = 91U;
    input.payload_identity.fallback_font_identity = QStringLiteral("fallback:framework-mono");
    return input;
}

term::Terminal_cell_shaping_input continuation(
    int                                        column,
    term::Terminal_shaped_presentation_mode    presentation = term::Terminal_shaped_presentation_mode::DEFAULT_TEXT)
{
    term::Terminal_cell_shaping_input input;
    input.column                                  = column;
    input.display_width                           = 0;
    input.wide_continuation                       = true;
    input.style_id                                = 3U;
    input.hyperlink_id                            = 91U;
    input.presentation_mode                       = presentation;
    input.payload_identity.fallback_font_identity = QStringLiteral("fallback:framework-mono");
    return input;
}

term::Terminal_shaped_run_build_result build(
    int                    start_column,
    int                    cell_span,
    const std::vector<term::Terminal_cell_shaping_input>&
                           cells)
{
    return term::build_cell_stable_shaped_run(
        7,
        start_column,
        cell_span,
        metrics(),
        cells);
}

term::Terminal_shaped_run_build_result build_with_metrics(
    term::terminal_cell_metrics_t                          cell_metrics,
    const std::vector<term::Terminal_cell_shaping_input>&  cells)
{
    return term::build_cell_stable_shaped_run(
        7,
        0,
        1,
        cell_metrics,
        cells);
}

bool expect_valid_run(
    const term::Terminal_shaped_run_build_result&
                           result,
    const char*            message)
{
    if (!check(result.status == term::Terminal_shaped_run_status::OK, message)) {
        return false;
    }

    return
        check(
            term::validate_cell_stable_shaped_run(result.run) == term::Terminal_shaped_run_status::OK,
            "valid run also passes standalone validation");
}

bool expect_status(
    const term::Terminal_shaped_run_build_result&  result,
    term::Terminal_shaped_run_status               status,
    const char*                                    message)
{
    bool ok = true;
    ok &= check(result.status == status, message);
    ok &= check(result.run.clusters.empty() && result.run.cell_ownership.empty(),
        "invalid shaping result returns no partial run");
    return ok;
}

bool test_ascii_sentinels_around_unicode_payloads()
{
    bool ok = true;
    const std::vector<term::Terminal_cell_shaping_input> cells = {
        cell(3, QStringLiteral("A")),
        cell(4, QStringLiteral("\u03a9")),
        cell(5, QStringLiteral("B")),
        cell(6, QStringLiteral("\u754c"), 2),
        continuation(7),
        cell(8, QStringLiteral("C")),
        cell(9, QStringLiteral("e\u0301")),
        cell(10, QString::fromUcs4(U"\u2764\ufe0e"), 1,
            term::Terminal_shaped_presentation_mode::TEXT),
        cell(11, QString::fromUcs4(U"\u2764\ufe0f"), 2,
            term::Terminal_shaped_presentation_mode::EMOJI),
        continuation(12, term::Terminal_shaped_presentation_mode::EMOJI),
        cell(13, QStringLiteral("D")),
    };

    const term::Terminal_shaped_run_build_result result = build(3, 11, cells);
    if (!expect_valid_run(result, "mixed unicode shaping run builds")) {
        return false;
    }

    const term::Terminal_shaped_cell_run& run = result.run;
    ok &= check(run.source_row == 7, "source row is preserved");
    ok &= check(run.start_column == 3, "start column is preserved");
    ok &= check(run.cell_span == 11 && run.display_width == 11,
        "run span and display width stay cell-count based");
    ok &= check(run.cell_size == QSizeF(10.0, 20.0),
        "run cell size is copied from metrics");
    ok &= check(run.run_origin == QPointF(0.0, 0.0),
        "run origin is relative to the render-plan placement");
    ok &= check(run.clip_rect == QRectF(0.0, 0.0, 110.0, 20.0),
        "run clip rect covers exactly the source cell span");
    ok &= check(run.baseline_origin == QPointF(0.0, 14.0),
        "baseline origin is relative to the run origin");
    ok &= check(run.text_payload == QStringLiteral("A\u03a9B\u754cCe\u0301") +
        QString::fromUcs4(U"\u2764\ufe0e") +
        QString::fromUcs4(U"\u2764\ufe0f") +
        QStringLiteral("D"),
        "text payload preserves source cluster order");
    ok &= check(run.payload_identity.text_node_key == run.text_payload,
        "text-node payload identity defaults to payload text");
    ok &= check(run.payload_identity.fallback_font_identity ==
        QStringLiteral("fallback:framework-mono"),
        "fallback font identity placeholder is value data");
    ok &= check(run.style_id == 3U && run.hyperlink_id == 91U,
        "style and hyperlink ids are copied to the run");
    ok &= check(run.clusters.size() == 9U,
        "combining and variation-selector payloads stay single clusters");

    const auto& ownership = run.cell_ownership;
    ok &= check(ownership[0].owner_cell_offset == 0 && !ownership[0].continuation,
        "leading ASCII sentinel owns one cell");
    ok &= check(ownership[1].owner_cell_offset == 1 && !ownership[1].continuation,
        "Omega owns one cell");
    ok &= check(ownership[2].owner_cell_offset == 2 && !ownership[2].continuation,
        "ASCII sentinel before CJK owns one cell");
    ok &= check(ownership[3].owner_cell_offset == 3 && !ownership[3].continuation,
        "CJK base owns its first cell");
    ok &= check(ownership[4].owner_cell_offset == 3 && ownership[4].continuation,
        "CJK continuation is owned by the CJK base cluster");
    ok &= check(ownership[5].owner_cell_offset == 5 && !ownership[5].continuation,
        "ASCII sentinel after CJK is not pulled into the wide cluster");
    ok &= check(ownership[6].owner_cell_offset == 6 && !ownership[6].continuation,
        "combining sequence owns one cell");
    ok &= check(ownership[7].owner_cell_offset == 7 && !ownership[7].continuation,
        "VS15 text presentation owns one cell");
    ok &= check(ownership[8].owner_cell_offset == 8 && !ownership[8].continuation,
        "VS16 emoji base owns its first cell");
    ok &= check(ownership[9].owner_cell_offset == 8 && ownership[9].continuation,
        "VS16 emoji continuation is owned by the emoji base cluster");
    ok &= check(ownership[10].owner_cell_offset == 10 && !ownership[10].continuation,
        "trailing ASCII sentinel owns one cell");

    ok &= check(run.clusters[3].position == QPointF(30.0, 0.0) &&
        run.clusters[3].advance == QPointF(20.0, 0.0) &&
        run.clusters[3].clip_rect == QRectF(30.0, 0.0, 20.0, 20.0),
        "wide CJK cluster geometry is cell-stable");
    ok &= check(run.clusters[6].presentation_mode ==
        term::Terminal_shaped_presentation_mode::TEXT,
        "VS15 cluster records text presentation");
    ok &= check(run.clusters[7].presentation_mode ==
        term::Terminal_shaped_presentation_mode::EMOJI,
        "VS16 cluster records emoji presentation");
    return ok;
}

bool test_invalid_input_does_not_create_cross_cell_ownership()
{
    bool ok = true;

    term::Terminal_shaped_run_build_result result = build(
        0,
        1,
        {cell(0, QStringLiteral("AB"), 1)});
    ok &= check(result.status == term::Terminal_shaped_run_status::INVALID_TEXT_WIDTH,
        "two narrow glyphs cannot be compressed into one terminal cell");
    ok &= check(result.run.clusters.empty() && result.run.cell_ownership.empty(),
        "invalid narrow overflow returns no partial ownership map");

    result  = build(
        0,
        2,
        {
            cell(0, QStringLiteral("\u754c"), 2),
            cell(1, QStringLiteral("X"), 1),
        });
    ok     &= check(result.status == term::Terminal_shaped_run_status::INVALID_CELL_OVERLAP,
        "overlapping wide and narrow inputs are rejected");
    ok     &= check(result.run.clusters.empty() && result.run.cell_ownership.empty(),
        "overlap rejection returns no partial shaped run");

    result  = build(
        0,
        1,
        {cell(0, QStringLiteral("\u754c"), 2)});
    ok     &= check(result.status == term::Terminal_shaped_run_status::INVALID_CELL_INPUT,
        "wide glyph cannot extend beyond the requested source span");
    ok     &= check(result.run.clusters.empty() && result.run.cell_ownership.empty(),
        "out-of-span wide input returns no cross-cell ownership");

    ok &= expect_status(
        build(3, 1, {cell(2, QStringLiteral("A"))}),
        term::Terminal_shaped_run_status::INVALID_CELL_INPUT,
        "cell before run start is rejected");

    ok &= expect_status(
        build(3, 1, {cell(4, QStringLiteral("A"))}),
        term::Terminal_shaped_run_status::INVALID_CELL_INPUT,
        "cell after run end is rejected");

    return ok;
}

bool test_unordered_input_and_duplicate_columns()
{
    bool ok = true;

    const term::Terminal_shaped_run_build_result result = build(
        3,
        3,
        {
            cell(5, QStringLiteral("C")),
            cell(3, QStringLiteral("A")),
            cell(4, QStringLiteral("B")),
        });
    if (!expect_valid_run(result, "unordered input builds in source-column order")) {
        return false;
    }

    ok &= check(result.run.text_payload == QStringLiteral("ABC"),
        "unordered input exports text payload in ascending column order");
    ok &= check(result.run.cell_ownership[0].owner_cell_offset == 0 &&
        result.run.cell_ownership[1].owner_cell_offset == 1 &&
        result.run.cell_ownership[2].owner_cell_offset == 2,
        "unordered input produces visual ownership order by column");

    ok &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("A")),
                cell(0, QStringLiteral("B")),
            }),
        term::Terminal_shaped_run_status::INVALID_CELL_OVERLAP,
        "duplicate source columns are rejected");

    return ok;
}

bool test_clamped_wide_single_clusters()
{
    bool ok = true;

    term::Terminal_shaped_run_build_result result =
        build(0, 1, {cell(0, QStringLiteral("\u754c"), 1)});
    if (!expect_valid_run(result, "clamped CJK cluster builds in one cell")) {
        return false;
    }
    ok &= check(result.run.clusters.size() == 1U &&
        result.run.clusters.front().cell_span == 1 &&
        result.run.clusters.front().advance == QPointF(10.0, 0.0),
        "clamped CJK cluster is clipped to the owning cell");

    result = build(
        0,
        1,
        {cell(0, QString::fromUcs4(U"\u2764\ufe0f"), 1,
            term::Terminal_shaped_presentation_mode::EMOJI)});
    if (!expect_valid_run(result, "clamped VS16 emoji cluster builds in one cell")) {
        return false;
    }
    ok &= check(result.run.clusters.front().presentation_mode ==
        term::Terminal_shaped_presentation_mode::EMOJI,
        "clamped VS16 emoji preserves cluster presentation");
    ok &= check(result.run.presentation_mode ==
        term::Terminal_shaped_presentation_mode::EMOJI,
        "uniform emoji presentation is preserved as the run summary");

    result = build(
        0,
        1,
        {cell(0, QString::fromUcs4(U"\U0001f600"), 1)});
    if (!expect_valid_run(result, "clamped non-BMP emoji cluster builds in one cell")) {
        return false;
    }
    ok &= check(result.run.clusters.front().text_length == 2,
        "clamped non-BMP emoji cluster keeps the UTF-16 surrogate pair payload");

    ok &= expect_status(
        build(0, 1, {cell(0, QStringLiteral("AB"), 1)}),
        term::Terminal_shaped_run_status::INVALID_TEXT_WIDTH,
        "two narrow text clusters still cannot be compressed into one cell");

    return ok;
}

bool test_metrics_and_empty_input_rejection()
{
    bool ok = true;

    ok &= expect_status(
        build_with_metrics({0.0, 20.0, 14.0, 6.0}, {cell(0, QStringLiteral("A"))}),
        term::Terminal_shaped_run_status::INVALID_CELL_METRICS,
        "zero-width metrics are rejected before shaping");

    ok &= expect_status(
        build_with_metrics(
            { std::numeric_limits<double>::quiet_NaN(), 20.0, 14.0, 6.0 },
            {cell(0, QStringLiteral("A"))}),
        term::Terminal_shaped_run_status::INVALID_CELL_METRICS,
        "NaN metrics are rejected before shaping");

    ok &= expect_status(
        build(0, 1, {}),
        term::Terminal_shaped_run_status::INVALID_CELL_INPUT,
        "empty input is rejected");

    ok &= expect_status(
        build(0, 1, {continuation(0)}),
        term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "all-continuation input is rejected");

    return ok;
}

bool test_wide_continuation_contract()
{
    bool ok = true;

    ok &= expect_status(
        build(0, 2, {cell(0, QStringLiteral("\u754c"), 2)}),
        term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "wide input requires an explicit continuation cell");

    ok &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                continuation(1),
                continuation(1),
            }),
        term::Terminal_shaped_run_status::INVALID_CELL_OVERLAP,
        "duplicate continuation source columns are rejected");

    ok &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("A")),
                continuation(1),
            }),
        term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "continuation on an uncovered column is rejected");

    term::Terminal_cell_shaping_input invalid_continuation = continuation(1);
    invalid_continuation.text = QStringLiteral("x");
    ok &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                invalid_continuation,
            }),
        term::Terminal_shaped_run_status::INVALID_CELL_INPUT,
        "continuation with text is rejected");

    invalid_continuation                = continuation(1);
    invalid_continuation.display_width  = 1;
    ok                                 &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                invalid_continuation,
            }),
        term::Terminal_shaped_run_status::INVALID_CELL_INPUT,
        "continuation with non-zero display width is rejected");

    invalid_continuation           = continuation(1);
    invalid_continuation.style_id  = 4U;
    ok                            &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                invalid_continuation,
            }),
        term::Terminal_shaped_run_status::INVALID_RUN_METADATA,
        "continuation style metadata must match the owner");

    invalid_continuation               = continuation(1);
    invalid_continuation.hyperlink_id  = 92U;
    ok                                &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                invalid_continuation,
            }),
        term::Terminal_shaped_run_status::INVALID_RUN_METADATA,
        "continuation hyperlink metadata must match the owner");

    ok &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QString::fromUcs4(U"\u2764\ufe0f"), 2,
                    term::Terminal_shaped_presentation_mode::EMOJI),
                continuation(1),
            }),
        term::Terminal_shaped_run_status::INVALID_RUN_METADATA,
        "continuation presentation metadata must match the owner");

    invalid_continuation = continuation(1);
    invalid_continuation.payload_identity.fallback_font_identity =
        QStringLiteral("fallback:other");
    ok &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                invalid_continuation,
            }),
        term::Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY,
        "continuation payload identity must match the owner");

    invalid_continuation                                 = continuation(1);
    invalid_continuation.payload_identity.text_node_key  = QStringLiteral("continuation-key");
    ok                                                  &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                invalid_continuation,
            }),
        term::Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY,
        "continuation text-node key identity must match the owner");

    invalid_continuation                              = continuation(1);
    invalid_continuation.payload_identity.generation  = 23U;
    ok                                               &= expect_status(
        build(
            0,
            2,
            {
                cell(0, QStringLiteral("\u754c"), 2),
                invalid_continuation,
            }),
        term::Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY,
        "continuation payload generation must match the owner");

    return ok;
}

bool test_run_payload_identity_and_presentation_contract()
{
    bool ok = true;

    term::Terminal_cell_shaping_input right = cell(1, QStringLiteral("B"));
    right.payload_identity.fallback_font_identity = QStringLiteral("fallback:other");
    ok &= expect_status(
        build(0, 2, {cell(0, QStringLiteral("A")), right}),
        term::Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY,
        "run rejects mismatched fallback font identity");

    right                                 = cell(1, QStringLiteral("B"));
    right.payload_identity.text_node_key  = QStringLiteral("explicit-key");
    ok                                   &= expect_status(
        build(0, 2, {cell(0, QStringLiteral("A")), right}),
        term::Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY,
        "run rejects mismatched text node key identity");

    right                              = cell(1, QStringLiteral("B"));
    right.payload_identity.generation  = 17U;
    ok                                &= expect_status(
        build(0, 2, {cell(0, QStringLiteral("A")), right}),
        term::Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY,
        "run rejects mismatched payload generation");

    right           = cell(1, QStringLiteral("B"));
    right.style_id  = 4U;
    ok             &= expect_status(
        build(0, 2, {cell(0, QStringLiteral("A")), right}),
        term::Terminal_shaped_run_status::INVALID_RUN_METADATA,
        "run rejects mismatched style metadata");

    right               = cell(1, QStringLiteral("B"));
    right.hyperlink_id  = 92U;
    ok                 &= expect_status(
        build(0, 2, {cell(0, QStringLiteral("A")), right}),
        term::Terminal_shaped_run_status::INVALID_RUN_METADATA,
        "run rejects mismatched hyperlink metadata");

    ok &= expect_status(
        build(
            0,
            2,
            {cell(0, QString::fromUcs4(U"\u2764\ufe0f"), 2,
                term::Terminal_shaped_presentation_mode::TEXT)}),
        term::Terminal_shaped_run_status::INVALID_TEXT_WIDTH,
        "text-presentation cell rejects emoji-presentation payload");

    ok &= expect_status(
        build(
            0,
            1,
            {cell(0, QString::fromUcs4(U"\u2764\ufe0e"), 1,
                term::Terminal_shaped_presentation_mode::EMOJI)}),
        term::Terminal_shaped_run_status::INVALID_TEXT_WIDTH,
        "emoji-presentation cell rejects text-presentation payload");

    const term::Terminal_shaped_run_build_result mixed = build(
        0,
        3,
        {
            cell(0, QString::fromUcs4(U"\u2764\ufe0e"), 1,
                term::Terminal_shaped_presentation_mode::TEXT),
            cell(1, QString::fromUcs4(U"\u2764\ufe0f"), 2,
                term::Terminal_shaped_presentation_mode::EMOJI),
            continuation(2, term::Terminal_shaped_presentation_mode::EMOJI),
        });
    if (!expect_valid_run(mixed, "mixed cluster presentation remains valid")) {
        return false;
    }

    ok &= check(mixed.run.presentation_mode ==
        term::Terminal_shaped_presentation_mode::DEFAULT_TEXT,
        "mixed presentation uses a non-authoritative default run summary");
    ok &= check(mixed.run.clusters[0].presentation_mode ==
        term::Terminal_shaped_presentation_mode::TEXT,
        "first cluster keeps text presentation");
    ok &= check(mixed.run.clusters[1].presentation_mode ==
        term::Terminal_shaped_presentation_mode::EMOJI,
        "second cluster keeps emoji presentation");

    return ok;
}

QString text_with_code_unit(ushort code_unit)
{
    QString text;
    text.append(QChar(code_unit));
    return text;
}

bool test_malformed_utf16_and_zwj_rejection()
{
    bool ok = true;

    ok &= expect_status(
        build(0, 1, {cell(0, text_with_code_unit(0xd800U))}),
        term::Terminal_shaped_run_status::INVALID_TEXT_ENCODING,
        "unpaired high surrogate is rejected before payload export");

    ok &= expect_status(
        build(0, 1, {cell(0, text_with_code_unit(0xdc00U))}),
        term::Terminal_shaped_run_status::INVALID_TEXT_ENCODING,
        "unpaired low surrogate is rejected before payload export");

    QString high_followed_by_non_low = text_with_code_unit(0xd800U);
    high_followed_by_non_low.append(QChar('A'));
    ok &= expect_status(
        build(0, 1, {cell(0, high_followed_by_non_low)}),
        term::Terminal_shaped_run_status::INVALID_TEXT_ENCODING,
        "high surrogate followed by a non-low code unit is rejected");

    ok &= expect_status(
        build(
            0,
            2,
            {cell(0, QString::fromUcs4(U"\U0001f469\u200d\U0001f4bb"), 2)}),
        term::Terminal_shaped_run_status::INVALID_GRAPHEME_CLUSTER,
        "ZWJ emoji sequence is rejected by the cell-stable contract");

    return ok;
}

bool test_wide_and_narrow_ownership_snapshots()
{
    bool ok = true;

    term::Terminal_shaped_run_build_result result = build(
        2,
        2,
        {
            cell(2, QStringLiteral("\u754c"), 2),
            continuation(3),
        });
    if (!expect_valid_run(result, "wide ownership map builds")) {
        return false;
    }
    ok &= check(result.run.cell_ownership[0].owner_cell_offset == 0 &&
        !result.run.cell_ownership[0].continuation,
        "wide snapshot has base ownership at first cell");
    ok &= check(result.run.cell_ownership[1].owner_cell_offset == 0 &&
        result.run.cell_ownership[1].continuation,
        "wide snapshot has continuation ownership at second cell");

    result = build(
        2,
        2,
        {
            cell(2, QStringLiteral("x")),
            cell(3, QStringLiteral("y")),
        });
    if (!expect_valid_run(result, "narrow replacement ownership map builds")) {
        return false;
    }
    ok &= check(result.run.cell_ownership[0].owner_cell_offset == 0 &&
        !result.run.cell_ownership[0].continuation,
        "narrow snapshot has first narrow owner");
    ok &= check(result.run.cell_ownership[1].owner_cell_offset == 1 &&
        !result.run.cell_ownership[1].continuation,
        "narrow snapshot has second narrow owner");
    ok &= check(result.run.clusters[0].clip_rect == QRectF(0.0, 0.0, 10.0, 20.0) &&
        result.run.clusters[1].clip_rect == QRectF(10.0, 0.0, 10.0, 20.0),
        "narrow replacement clips each cell independently");

    result = build(
        2,
        2,
        {
            cell(2, QStringLiteral("\u754c"), 2),
            continuation(3),
        });
    if (!expect_valid_run(result, "narrow-to-wide replacement ownership map builds")) {
        return false;
    }
    ok &= check(result.run.cell_ownership[0].owner_cell_offset == 0 &&
        !result.run.cell_ownership[0].continuation,
        "second wide snapshot owns base cell");
    ok &= check(result.run.cell_ownership[1].owner_cell_offset == 0 &&
        result.run.cell_ownership[1].continuation,
        "second wide snapshot owns continuation cell");
    ok &= check(result.run.clusters.size() == 1U &&
        result.run.clusters.front().cell_span == 2,
        "second wide snapshot has one two-cell cluster");

    return ok;
}

bool test_clip_rect_and_span_invariants()
{
    bool ok = true;
    term::Terminal_shaped_run_build_result result = build(
        4,
        3,
        {
            cell(4, QStringLiteral("L")),
            cell(5, QStringLiteral("\u754c"), 2),
            continuation(6),
        });
    if (!expect_valid_run(result, "clip invariant fixture builds")) {
        return false;
    }

    term::Terminal_shaped_cell_run broken = result.run;
    broken.source_row = -1;
    ok &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_SOURCE_RANGE,
        "negative source row is invalid");

    broken               = result.run;
    broken.start_column  = -1;
    ok                  &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_SOURCE_RANGE,
        "negative start column is invalid");

    broken                = result.run;
    broken.display_width  = 2;
    ok                   &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_SOURCE_RANGE,
        "display width must equal the source cell span");

    broken            = result.run;
    broken.cell_size  = QSizeF(0.0, 20.0);
    ok               &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_METRICS,
        "invalid cell size is rejected by standalone validation");

    broken            = result.run;
    broken.clip_rect  = QRectF(0.0, 0.0, 20.0, 20.0);
    ok               &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "run clip rect must match the exact cell span");

    broken = result.run;
    broken.clusters.front().clip_rect = QRectF(0.0, 0.0, 30.0, 20.0);
    ok &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "cluster clip rect cannot exceed owned terminal cells");

    broken = result.run;
    broken.clusters.front().position = QPointF(1.0, 0.0);
    ok &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "cluster position must match the owned cell rectangle");

    broken = result.run;
    broken.clusters.front().advance = QPointF(11.0, 0.0);
    ok &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "cluster advance must match the owned cell span");

    broken = result.run;
    broken.clusters.front().text_length = 0;
    ok &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "cluster text length must be positive");

    broken = result.run;
    broken.clusters.front().owner_cell_offset = -1;
    ok &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "cluster owner offset must be in range");

    broken = result.run;
    broken.clusters.front().cell_span = 0;
    ok &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "cluster cell span must be positive");

    broken                                      = result.run;
    broken.cell_ownership[1].owner_cell_offset  = 0;
    ok                                         &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "ownership map must agree with cluster ownership");

    broken                                = result.run;
    broken.cell_ownership[1].cell_offset  = 0;
    ok                                   &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "ownership entry cell offset must match its vector slot");

    broken                                      = result.run;
    broken.cell_ownership[1].owner_cell_offset  = 2;
    ok                                         &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "ownership owner cannot be to the right of the owned cell");

    broken                                  = result.run;
    broken.cell_ownership[1].cluster_index  = static_cast<int>(result.run.clusters.size());
    ok                                     &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "ownership cluster index must be in range");

    const term::Terminal_shaped_run_build_result text_range_result = build(
        0,
        4,
        {
            cell(0, QStringLiteral("A")),
            cell(1, QStringLiteral("B")),
            cell(2, QStringLiteral("C")),
            cell(3, QStringLiteral("D")),
        });
    if (!expect_valid_run(text_range_result, "text-range invariant fixture builds")) {
        return false;
    }

    broken                          = text_range_result.run;
    broken.clusters[0].text_length  = 2;
    broken.clusters[1].text_offset  = 1;
    ok                             &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "overlapping cluster text ranges are rejected");

    broken                          = text_range_result.run;
    broken.text_payload            += QStringLiteral("E");
    broken.clusters[1].text_offset  = 2;
    broken.clusters[2].text_offset  = 3;
    broken.clusters[3].text_offset  = 4;
    ok                             &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "in-bounds cluster text gaps are rejected");

    broken               = result.run;
    broken.text_payload += QStringLiteral("!");
    ok                  &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_RUN_INVARIANT,
        "cluster text ranges must cover the whole payload");

    broken                                  = result.run;
    broken.cell_ownership[2].cluster_index  = 0;
    ok                                     &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "ownership entry cluster must cover the referenced cell");

    broken                                 = result.run;
    broken.cell_ownership[2].continuation  = false;
    ok                                    &= check(
        term::validate_cell_stable_shaped_run(broken) ==
            term::Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP,
        "ownership continuation flag must match owner and cell offsets");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_ascii_sentinels_around_unicode_payloads();
    ok &= test_invalid_input_does_not_create_cross_cell_ownership();
    ok &= test_unordered_input_and_duplicate_columns();
    ok &= test_clamped_wide_single_clusters();
    ok &= test_metrics_and_empty_input_rejection();
    ok &= test_wide_continuation_contract();
    ok &= test_run_payload_identity_and_presentation_contract();
    ok &= test_malformed_utf16_and_zwj_rejection();
    ok &= test_wide_and_narrow_ownership_snapshots();
    ok &= test_clip_rect_and_span_invariants();
    return ok ? 0 : 1;
}
