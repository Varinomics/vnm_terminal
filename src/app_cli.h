#pragma once

#include "app_options.h"

#include <QString>
#include <QStringList>

namespace vnm_terminal::terminal_app {

struct Parse_result
{
    App_options        options;
    QString            error;
    bool               help_requested                = false;
};

void print_usage();

Parse_result parse_arguments(const QStringList& arguments);

bool validate_capture_paths(App_options* options, QString* out_error);

bool prepare_capture_file(
    const QString& option_name,
    const QString& path,
    QString*       out_error);

} // namespace vnm_terminal::terminal_app
