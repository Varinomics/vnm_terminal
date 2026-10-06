#pragma once

#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>

#include <optional>

namespace vnm_terminal::terminal_app {

struct Codex_invocation
{
    // Includes the absolute executable and any fixed arguments such as a Node entry point.
    qsizetype argument_prefix_size;
};

class Codex_command_environment
{
public:
    // Keep this owner alive for the terminal session: its PATH commands live in
    // a private directory that is removed when the owner is destroyed.
    // An explicit invocation identifies a verified Codex launch without executable-name inference.
    // Windows callers supply a native executable; its prefix remains before Codex CLI options.
    bool prepare(
        QStringList& command, QProcessEnvironment& environment, QString& error,
        std::optional<Codex_invocation> explicit_invocation = std::nullopt);

private:
    QTemporaryDir m_directory;
};

} // namespace vnm_terminal::terminal_app
