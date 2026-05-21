#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/unicode_width.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_shaped_presentation_mode
{
    DEFAULT_TEXT,
    TEXT,
    EMOJI,
    REPLACEMENT,
};

enum class Terminal_shaped_run_status
{
    OK,
    INVALID_SOURCE_RANGE,
    INVALID_CELL_METRICS,
    INVALID_CELL_INPUT,
    INVALID_CELL_OVERLAP,
    INVALID_CELL_OWNERSHIP,
    INVALID_TEXT_WIDTH,
    INVALID_RUN_METADATA,
    INVALID_PAYLOAD_IDENTITY,
    INVALID_TEXT_ENCODING,
    INVALID_GRAPHEME_CLUSTER,
    INVALID_RUN_INVARIANT,
};

struct Terminal_shaped_payload_identity
{
    QString                                        text_node_key;
    QString                                        fallback_font_identity;
    std::uint64_t                                  generation        = 0U;
};

struct Terminal_cell_shaping_input
{
    int                                            column            = 0;
    QString                                        text;
    int                                            display_width     = 1;
    bool                                           wide_continuation = false;
    Terminal_style_id                              style_id          = k_default_terminal_style_id;
    std::uint64_t                                  hyperlink_id      = 0U;
    Terminal_shaped_presentation_mode  presentation_mode =
        Terminal_shaped_presentation_mode::DEFAULT_TEXT;
    Terminal_shaped_payload_identity               payload_identity;
};

struct Terminal_shaped_cluster
{
    int                                            text_offset       = 0;
    int                                            text_length       = 0;
    int                                            owner_cell_offset = 0;
    int                                            cell_span         = 1;
    QPointF                                        position;
    QPointF                                        advance;
    QRectF                                         clip_rect;
    Terminal_shaped_presentation_mode  presentation_mode =
        Terminal_shaped_presentation_mode::DEFAULT_TEXT;
};

struct Terminal_shaped_cell_ownership
{
    int                                            cell_offset       = 0;
    int                                            owner_cell_offset = 0;
    int                                            cluster_index     = -1;
    bool                                           continuation      = false;
};

struct Terminal_shaped_cell_run
{
    int                                            source_row = 0;
    int                                            start_column = 0;
    int                                            cell_span = 0;
    int                                            display_width = 0;
    QSizeF                                         cell_size;
    QPointF                                        run_origin;
    QPointF                                        baseline_origin;
    QRectF                                         clip_rect;
    QString                                        text_payload;
    Terminal_shaped_payload_identity               payload_identity;
    Terminal_style_id                              style_id = k_default_terminal_style_id;
    std::uint64_t                                  hyperlink_id = 0U;
    Terminal_shaped_presentation_mode  presentation_mode =
        Terminal_shaped_presentation_mode::DEFAULT_TEXT;
    std::vector<Terminal_shaped_cluster>           clusters;
    std::vector<Terminal_shaped_cell_ownership>    cell_ownership;
};

struct Terminal_shaped_run_build_result
{
    Terminal_shaped_run_status                     status = Terminal_shaped_run_status::OK;
    Terminal_shaped_cell_run                       run;
};

inline bool is_valid_shaping_cell_size(QSizeF cell_size)
{
    return
        std::isfinite(cell_size.width())  &&
        std::isfinite(cell_size.height()) &&
        cell_size.width() > 0.0           &&
        cell_size.height() > 0.0;
}

inline QSizeF shaping_cell_size_from_metrics(terminal_cell_metrics_t metrics)
{
    return QSizeF(metrics.width, metrics.height);
}

inline Terminal_shaped_presentation_mode shaped_presentation_from_width(
    Terminal_unicode_presentation presentation)
{
    switch (presentation) {
        case Terminal_unicode_presentation::TEXT:
            return Terminal_shaped_presentation_mode::TEXT;
        case Terminal_unicode_presentation::EMOJI:
            return Terminal_shaped_presentation_mode::EMOJI;
        case Terminal_unicode_presentation::DEFAULT:
            return Terminal_shaped_presentation_mode::DEFAULT_TEXT;
    }

    return Terminal_shaped_presentation_mode::DEFAULT_TEXT;
}

inline Terminal_unicode_presentation unicode_presentation_from_shaped(
    Terminal_shaped_presentation_mode presentation)
{
    switch (presentation) {
        case Terminal_shaped_presentation_mode::TEXT:
            return Terminal_unicode_presentation::TEXT;
        case Terminal_shaped_presentation_mode::EMOJI:
            return Terminal_unicode_presentation::EMOJI;
        case Terminal_shaped_presentation_mode::DEFAULT_TEXT:
        case Terminal_shaped_presentation_mode::REPLACEMENT:
            return Terminal_unicode_presentation::DEFAULT;
    }

    return Terminal_unicode_presentation::DEFAULT;
}

inline bool is_utf16_high_surrogate(ushort value)
{
    return value >= 0xd800U && value <= 0xdbffU;
}

inline bool is_utf16_low_surrogate(ushort value)
{
    return value >= 0xdc00U && value <= 0xdfffU;
}

inline bool shaped_payload_identity_equal(
    const Terminal_shaped_payload_identity&    lhs,
    const Terminal_shaped_payload_identity&    rhs)
{
    return
        lhs.text_node_key          == rhs.text_node_key          &&
        lhs.fallback_font_identity == rhs.fallback_font_identity &&
        lhs.generation             == rhs.generation;
}

inline Terminal_shaped_run_status validate_cell_text_scalars(const QString& text)
{
    for (qsizetype offset = 0; offset < text.size();) {
        const ushort first = text.at(offset).unicode();
        if (is_utf16_low_surrogate(first)) {
            return Terminal_shaped_run_status::INVALID_TEXT_ENCODING;
        }

        if (is_utf16_high_surrogate(first)) {
            if (offset + 1 >= text.size()) {
                return Terminal_shaped_run_status::INVALID_TEXT_ENCODING;
            }

            const ushort second = text.at(offset + 1).unicode();
            if (!is_utf16_low_surrogate(second)) {
                return Terminal_shaped_run_status::INVALID_TEXT_ENCODING;
            }

            offset += 2;
            continue;
        }

        if (first == 0x200dU) {
            return Terminal_shaped_run_status::INVALID_GRAPHEME_CLUSTER;
        }

        ++offset;
    }

    return Terminal_shaped_run_status::OK;
}

inline char32_t utf16_scalar_at(const QString& text, qsizetype offset, int& length)
{
    length = 1;
    const ushort first = text.at(offset).unicode();
    if (!is_utf16_high_surrogate(first)) {
        return is_utf16_low_surrogate(first) ? 0xfffdU : static_cast<char32_t>(first);
    }

    if (offset + 1 >= text.size()) {
        return 0xfffdU;
    }

    const ushort second = text.at(offset + 1).unicode();
    if (!is_utf16_low_surrogate(second)) {
        return 0xfffdU;
    }

    length = 2;
    return
         0x10000U                                         +
        ((static_cast<char32_t>(first) - 0xd800U) << 10U) +
        (static_cast<char32_t>(second) - 0xdc00U);
}

inline bool starts_new_terminal_text_cluster(char32_t codepoint, bool cluster_active)
{
    if (!cluster_active) {
        return true;
    }

    return
        !is_terminal_combining_codepoint(codepoint) &&
        !is_terminal_variation_selector(codepoint);
}

inline int terminal_text_cluster_count(const QString& text)
{
    int  cluster_count  = 0;
    bool cluster_active = false;

    for (qsizetype offset = 0; offset < text.size();) {
        int length = 0;
        const char32_t codepoint = utf16_scalar_at(text, offset, length);
        if (starts_new_terminal_text_cluster(codepoint, cluster_active)) {
            ++cluster_count;
            cluster_active = true;
        }

        offset += length;
    }

    return cluster_count;
}

inline bool can_clip_natural_wide_cluster_to_one_cell(
    const Terminal_cell_shaping_input& cell,
    const Terminal_utf8_width_result&  width)
{
    return
        cell.display_width                     == 1 &&
        width.cells                            == 2 &&
        terminal_text_cluster_count(cell.text) == 1;
}

inline Terminal_shaped_run_status append_cell_clusters(
    Terminal_shaped_cell_run&          run,
    const Terminal_cell_shaping_input& cell,
    int                                text_base_offset,
    int                                owner_cell_offset)
{
    bool      cluster_active = false;
    qsizetype cluster_start  = 0;

    for (qsizetype offset = 0; offset < cell.text.size();) {
        int length = 0;
        const char32_t codepoint = utf16_scalar_at(cell.text, offset, length);
        if (starts_new_terminal_text_cluster(codepoint, cluster_active)) {
            if (cluster_active) {
                const int cluster_length = static_cast<int>(offset - cluster_start);
                run.clusters.push_back({
                    text_base_offset + static_cast<int>(cluster_start),
                    cluster_length,
                    owner_cell_offset,
                    cell.display_width,
                    QPointF(run.cell_size.width() * owner_cell_offset, 0.0),
                    QPointF(run.cell_size.width() * cell.display_width, 0.0),
                    QRectF(
                        run.cell_size.width() * owner_cell_offset,
                        0.0,
                        run.cell_size.width() * cell.display_width,
                        run.cell_size.height()),
                    cell.presentation_mode,
                });
            }

            cluster_start = offset;
            cluster_active = true;
        }

        offset += length;
    }

    if (!cluster_active) {
        return Terminal_shaped_run_status::INVALID_CELL_INPUT;
    }

    run.clusters.push_back({
        text_base_offset + static_cast<int>(cluster_start),
        static_cast<int>(cell.text.size() - cluster_start),
        owner_cell_offset,
        cell.display_width,
        QPointF(run.cell_size.width() * owner_cell_offset, 0.0),
        QPointF(run.cell_size.width() * cell.display_width, 0.0),
        QRectF(
            run.cell_size.width() * owner_cell_offset,
            0.0,
            run.cell_size.width() * cell.display_width,
            run.cell_size.height()),
        cell.presentation_mode,
    });

    return Terminal_shaped_run_status::OK;
}

inline Terminal_shaped_run_status validate_cell_text_width(
    const Terminal_cell_shaping_input& cell)
{
    const Terminal_utf8_width_result width = measure_utf8_width(cell.text.toUtf8());
    if (width.status != Terminal_unicode_width_status::OK) {
        return Terminal_shaped_run_status::INVALID_TEXT_WIDTH;
    }

    const Terminal_unicode_presentation presentation =
        unicode_presentation_from_shaped(cell.presentation_mode);
    const bool presentation_matches = std::all_of(
        width.codepoints.begin(),
        width.codepoints.end(),
        [presentation](const Terminal_codepoint_width& codepoint) {
            return
                presentation           == Terminal_unicode_presentation::DEFAULT ||
                codepoint.presentation == Terminal_unicode_presentation::DEFAULT ||
                codepoint.presentation == presentation;
        });

    if (!presentation_matches) {
        return Terminal_shaped_run_status::INVALID_TEXT_WIDTH;
    }

    if (width.cells != cell.display_width &&
        !can_clip_natural_wide_cluster_to_one_cell(cell, width))
    {
        return Terminal_shaped_run_status::INVALID_TEXT_WIDTH;
    }

    return Terminal_shaped_run_status::OK;
}

inline Terminal_shaped_run_status validate_cell_stable_shaped_run(
    const Terminal_shaped_cell_run& run)
{
    if (run.source_row <  0  || run.start_column  <  0 ||
        run.cell_span  <= 0  || run.display_width != run.cell_span)
    {
        return Terminal_shaped_run_status::INVALID_SOURCE_RANGE;
    }

    if (!is_valid_shaping_cell_size(run.cell_size)) {
        return Terminal_shaped_run_status::INVALID_CELL_METRICS;
    }

    if (run.clip_rect != QRectF(
            0.0, 0.0, run.cell_size.width() * run.cell_span, run.cell_size.height()))
    {
        return Terminal_shaped_run_status::INVALID_RUN_INVARIANT;
    }

    if (run.cell_ownership.size() != static_cast<std::size_t>(run.cell_span)) {
        return Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP;
    }

    qsizetype expected_text_offset = 0;
    for (std::size_t i = 0; i < run.clusters.size(); ++i) {
        const Terminal_shaped_cluster& cluster = run.clusters[i];
        if (cluster.text_offset                       != expected_text_offset ||
            cluster.text_length                       <= 0                    ||
            cluster.text_offset + cluster.text_length >  run.text_payload.size())
        {
            return Terminal_shaped_run_status::INVALID_RUN_INVARIANT;
        }

        if (cluster.owner_cell_offset                     <  0 ||
            cluster.cell_span                             <= 0 ||
            cluster.owner_cell_offset + cluster.cell_span >  run.cell_span)
        {
            return Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP;
        }

        expected_text_offset += cluster.text_length;

        const QRectF expected_clip(
            run.cell_size.width() * cluster.owner_cell_offset,
            0.0,
            run.cell_size.width() * cluster.cell_span,
            run.cell_size.height());
        if (cluster.position  != expected_clip.topLeft()             ||
            cluster.advance   != QPointF(expected_clip.width(), 0.0) ||
            cluster.clip_rect != expected_clip)
        {
            return Terminal_shaped_run_status::INVALID_RUN_INVARIANT;
        }

        for (int cell_offset = cluster.owner_cell_offset;
            cell_offset < cluster.owner_cell_offset + cluster.cell_span;
            ++cell_offset)
        {
            const Terminal_shaped_cell_ownership& ownership =
                run.cell_ownership[static_cast<std::size_t>(cell_offset)];
            if (ownership.owner_cell_offset != cluster.owner_cell_offset ||
                ownership.cluster_index     != static_cast<int>(i)       ||
                ownership.continuation      != (ownership.owner_cell_offset != ownership.cell_offset))
            {
                return Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP;
            }
        }
    }

    if (expected_text_offset != run.text_payload.size()) {
        return Terminal_shaped_run_status::INVALID_RUN_INVARIANT;
    }

    for (int cell_offset = 0; cell_offset < run.cell_span; ++cell_offset) {
        const Terminal_shaped_cell_ownership& ownership =
            run.cell_ownership[static_cast<std::size_t>(cell_offset)];
        if (ownership.cell_offset       != cell_offset || ownership.owner_cell_offset < 0 ||
            ownership.owner_cell_offset >  cell_offset || ownership.cluster_index     < 0 ||
            ownership.cluster_index     >= static_cast<int>(run.clusters.size()))
        {
            return Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP;
        }

        const Terminal_shaped_cluster& cluster =
            run.clusters[static_cast<std::size_t>(ownership.cluster_index)];
        if (ownership.owner_cell_offset != cluster.owner_cell_offset                     ||
            ownership.cell_offset       <  cluster.owner_cell_offset                     ||
            ownership.cell_offset       >= cluster.owner_cell_offset + cluster.cell_span ||
            ownership.continuation      != (ownership.owner_cell_offset != ownership.cell_offset))
        {
            return Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP;
        }
    }

    return Terminal_shaped_run_status::OK;
}

inline Terminal_shaped_run_build_result build_cell_stable_shaped_run(
    int                        source_row,
    int                        start_column,
    int                        cell_span,
    terminal_cell_metrics_t    cell_metrics,
    const std::vector<Terminal_cell_shaping_input>&
                               cells)
{
    Terminal_shaped_run_build_result result;
    if (source_row < 0 || start_column < 0 || cell_span <= 0) {
        result.status = Terminal_shaped_run_status::INVALID_SOURCE_RANGE;
        return result;
    }

    if (!is_valid_cell_metrics(cell_metrics)) {
        result.status = Terminal_shaped_run_status::INVALID_CELL_METRICS;
        return result;
    }

    Terminal_shaped_cell_run& run = result.run;
    run.source_row      = source_row;
    run.start_column    = start_column;
    run.cell_span       = cell_span;
    run.display_width   = cell_span;
    run.cell_size       = shaping_cell_size_from_metrics(cell_metrics);
    run.run_origin      = QPointF(0.0, 0.0);
    run.baseline_origin = QPointF(0.0, cell_metrics.ascent);
    run.clip_rect       = QRectF(0.0, 0.0, run.cell_size.width() * cell_span, run.cell_size.height());
    run.clusters.reserve(static_cast<std::size_t>(cell_span));
    run.cell_ownership.reserve(static_cast<std::size_t>(cell_span));
    run.cell_ownership.resize(static_cast<std::size_t>(cell_span));

    std::vector<int> owner_cluster_indices(static_cast<std::size_t>(cell_span), -1);
    std::vector<int> owner_cell_offsets(static_cast<std::size_t>(cell_span), -1);
    std::vector<const Terminal_cell_shaping_input*> cell_inputs_by_offset(
        static_cast<std::size_t>(cell_span),
        nullptr);

    std::vector<Terminal_cell_shaping_input> sorted_cells = cells;
    std::sort(
        sorted_cells.begin(),
        sorted_cells.end(),
        [](const Terminal_cell_shaping_input& lhs, const Terminal_cell_shaping_input& rhs) {
            return lhs.column < rhs.column;
        });

    qsizetype total_text_length = 0;
    for (const Terminal_cell_shaping_input& cell : sorted_cells) {
        const int cell_offset = cell.column - start_column;
        if (cell_offset < 0 || cell_offset >= cell_span) {
            return {Terminal_shaped_run_status::INVALID_CELL_INPUT, {}};
        }

        if (cell_inputs_by_offset[static_cast<std::size_t>(cell_offset)] != nullptr) {
            return {Terminal_shaped_run_status::INVALID_CELL_OVERLAP, {}};
        }

        cell_inputs_by_offset[static_cast<std::size_t>(cell_offset)] = &cell;

        if (cell.wide_continuation) {
            if (!cell.text.isEmpty() || cell.display_width != 0) {
                return {Terminal_shaped_run_status::INVALID_CELL_INPUT, {}};
            }
        }
        else {
            total_text_length += cell.text.size();
        }
    }

    run.text_payload.reserve(total_text_length);

    bool run_metadata_set   = false;
    bool presentation_mixed = false;
    for (const Terminal_cell_shaping_input& cell : sorted_cells) {
        const int cell_offset = cell.column - start_column;
        if (cell.wide_continuation) {
            continue;
        }

        if (cell.text.isEmpty()     ||
            cell.display_width <= 0 ||
            cell.display_width >  cell_span - cell_offset)
        {
            return {Terminal_shaped_run_status::INVALID_CELL_INPUT, {}};
        }

        const Terminal_shaped_run_status text_status = validate_cell_text_scalars(cell.text);
        if (text_status != Terminal_shaped_run_status::OK) {
            return {text_status, {}};
        }

        if (run_metadata_set) {
            if (cell.style_id     != run.style_id ||
                cell.hyperlink_id != run.hyperlink_id)
            {
                return {Terminal_shaped_run_status::INVALID_RUN_METADATA, {}};
            }

            if (!shaped_payload_identity_equal(cell.payload_identity, run.payload_identity)) {
                return {Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY, {}};
            }

            if (cell.presentation_mode != run.presentation_mode) {
                presentation_mixed = true;
            }
        }
        else {
            run.style_id          = cell.style_id;
            run.hyperlink_id      = cell.hyperlink_id;
            run.presentation_mode = cell.presentation_mode;
            run.payload_identity  = cell.payload_identity;
            run_metadata_set      = true;
        }

        const Terminal_shaped_run_status width_status = validate_cell_text_width(cell);
        if (width_status != Terminal_shaped_run_status::OK) {
            return {width_status, {}};
        }

        for (int covered = 0; covered < cell.display_width; ++covered) {
            const int covered_offset = cell_offset + covered;
            if (owner_cell_offsets[static_cast<std::size_t>(covered_offset)] >= 0) {
                return {Terminal_shaped_run_status::INVALID_CELL_OVERLAP, {}};
            }

            if (covered > 0) {
                const Terminal_cell_shaping_input* continuation =
                    cell_inputs_by_offset[static_cast<std::size_t>(covered_offset)];
                if (continuation == nullptr) {
                    return {Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP, {}};
                }

                if (!continuation->wide_continuation) {
                    return {Terminal_shaped_run_status::INVALID_CELL_OVERLAP, {}};
                }

                if (continuation->style_id          != cell.style_id     ||
                    continuation->hyperlink_id      != cell.hyperlink_id ||
                    continuation->presentation_mode != cell.presentation_mode)
                {
                    return {Terminal_shaped_run_status::INVALID_RUN_METADATA, {}};
                }

                if (!shaped_payload_identity_equal(
                        continuation->payload_identity, cell.payload_identity))
                {
                    return {Terminal_shaped_run_status::INVALID_PAYLOAD_IDENTITY, {}};
                }
            }

            owner_cell_offsets[static_cast<std::size_t>(covered_offset)] = cell_offset;
        }

        const int text_base_offset = run.text_payload.size();
        run.text_payload += cell.text;
        const int first_cluster_index = static_cast<int>(run.clusters.size());
        const Terminal_shaped_run_status cluster_status = append_cell_clusters(
            run,
            cell,
            text_base_offset,
            cell_offset);
        if (cluster_status != Terminal_shaped_run_status::OK) {
            return {cluster_status, {}};
        }

        if (run.clusters.size() != static_cast<std::size_t>(first_cluster_index + 1)) {
            return {Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP, {}};
        }

        for (int covered = 0; covered < cell.display_width; ++covered) {
            owner_cluster_indices[static_cast<std::size_t>(cell_offset + covered)] =
                first_cluster_index;
        }
    }

    for (const Terminal_cell_shaping_input& cell : sorted_cells) {
        if (!cell.wide_continuation) {
            continue;
        }

        const int cell_offset       = cell.column - start_column;
        const int owner_cell_offset = owner_cell_offsets[static_cast<std::size_t>(cell_offset)];
        if (owner_cell_offset < 0 || owner_cell_offset == cell_offset) {
            return {Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP, {}};
        }
    }

    if (run.clusters.empty()) {
        return {Terminal_shaped_run_status::INVALID_CELL_INPUT, {}};
    }

    for (int cell_offset = 0; cell_offset < cell_span; ++cell_offset) {
        const int owner_cell_offset = owner_cell_offsets[static_cast<std::size_t>(cell_offset)];
        const int cluster_index     = owner_cluster_indices[static_cast<std::size_t>(cell_offset)];
        if (owner_cell_offset < 0 || cluster_index < 0) {
            return {Terminal_shaped_run_status::INVALID_CELL_OWNERSHIP, {}};
        }

        run.cell_ownership[static_cast<std::size_t>(cell_offset)] = {
            cell_offset,
            owner_cell_offset,
            cluster_index,
            owner_cell_offset != cell_offset,
        };
    }

    run.payload_identity.text_node_key = run.payload_identity.text_node_key.isEmpty()
        ? run.text_payload
        : run.payload_identity.text_node_key;
    if (presentation_mixed) {
        run.presentation_mode = Terminal_shaped_presentation_mode::DEFAULT_TEXT;
    }

    result.status = validate_cell_stable_shaped_run(run);
    if (result.status != Terminal_shaped_run_status::OK) {
        result.run = {};
    }

    return result;
}

}
