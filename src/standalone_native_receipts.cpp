#include "standalone_native_receipts.h"

#if defined(__APPLE__)
#include "app_common.h"

#include <vnm_process_custody/owner.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QUuid>
#include <sys/wait.h>

#include <memory>
#include <mutex>
#include <string>

namespace vnm_terminal::terminal_app {
namespace {

namespace custody = vnm::process_custody;
using Ack = Terminal_native_receipt_ack;
using Kind = Terminal_native_receipt_kind;

class Standalone_native_receipts
{
public:
    Standalone_native_receipts()
    {
        const QString location = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!location.isEmpty()) {
            const QDir directory(QDir(location).filePath(QStringLiteral("terminal-processes")));
            if (directory.mkpath(QStringLiteral("."))) {
                m_path = directory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                    QStringLiteral(".json"));
            }
        }
    }

    ~Standalone_native_receipts()
    {
        custody::close_native_handle(m_root_observer);
        if (m_record.isEmpty()) {
            return;
        }

        // The backend retains this hook through deferred native cleanup. Its
        // last release follows helper collection, possibly after Qt app teardown.
        const auto status = m_helper.observe();
        m_record.insert(QStringLiteral("helper_status"), QJsonObject{
            {QStringLiteral("state"),        static_cast<int>(status.state)},
            {QStringLiteral("status_valid"), status.status_valid},
            {QStringLiteral("wait_status"),  status.wait_status},
        });
        if (!persist()) {
            print_error(QStringLiteral("failed to retain terminal cleanup status"));
            return;
        }

        // Persist the exact raw status before giving the admitted helper's
        // typed exit code meaning. Crashes before this write leave it unknown.
        const bool normal_exit = status.state == custody::Native_process_observation::ENDED &&
            status.status_valid && WIFEXITED(status.wait_status);
        if (normal_exit &&
            (WEXITSTATUS(status.wait_status) == static_cast<int>(custody::Darwin_pty_owner_exit::NO_WORKLOAD) ||
             WEXITSTATUS(status.wait_status) == static_cast<int>(custody::Darwin_pty_owner_exit::CONFIRMED)))
        {
            QFile::remove(m_path);
        }
        else {
            print_error(QStringLiteral("PTY cleanup remains unconfirmed"));
        }
    }

    Ack retain(const Terminal_native_receipt& receipt)
    {
        const std::lock_guard lock(m_mutex);
        std::string error;
        if (receipt.kind == Kind::HELPER_PRE_START) {
            if (m_path.isEmpty() || !m_helper.open(receipt.helper_pid, receipt.helper_birth_token, &error)) {
                return Ack::REJECTED;
            }
            m_identity = receipt;
            m_record = {
                {QStringLiteral("contract"), QString::fromLatin1(custody::k_darwin_pty_owner_contract)},
                {QStringLiteral("helper_pid"), static_cast<qint64>(receipt.helper_pid)},
                {QStringLiteral("helper_birth_token"), QString::number(receipt.helper_birth_token)},
            };
            return persist() ? Ack::RETAINED : Ack::REJECTED;
        }
        if (m_record.isEmpty() || receipt.helper_pid != m_identity.helper_pid ||
            receipt.helper_birth_token != m_identity.helper_birth_token)
        {
            return Ack::REJECTED;
        }

        switch (receipt.kind) {
            case Kind::ROOT_BORN: {
                m_identity.root_pid         = receipt.root_pid;
                m_identity.root_birth_token = receipt.root_birth_token;
                m_root_observer = custody::open_process_observer(
                    receipt.root_pid, receipt.root_birth_token, &error);
                m_record.insert(QStringLiteral("root_birth"), QJsonObject{
                    {QStringLiteral("pid"),         static_cast<qint64>(receipt.root_pid)},
                    {QStringLiteral("birth_token"), QString::number(receipt.root_birth_token)},
                });
                // A rapid exit may prevent registration; only the matching
                // ROOT_ENDED receipt can discharge that original obligation.
                return persist() && m_root_observer != custody::k_invalid_handle ? Ack::RETAINED : Ack::REJECTED;
            }
            case Kind::ROOT_ENDED: {
                if (receipt.root_pid == 0 || receipt.root_pid != m_identity.root_pid ||
                    receipt.root_birth_token != m_identity.root_birth_token)
                {
                    return Ack::REJECTED;
                }
                m_record.insert(QStringLiteral("root_exit"), QJsonObject{
                    {QStringLiteral("pid"),         static_cast<qint64>(receipt.root_pid)},
                    {QStringLiteral("birth_token"), QString::number(receipt.root_birth_token)},
                    {QStringLiteral("exit_code"),   receipt.root_exit_code},
                    {QStringLiteral("signal"),      receipt.root_signal},
                });
                break;
            }
            case Kind::NO_BIRTH:
                if (m_identity.root_pid != 0) {
                    return Ack::REJECTED;
                }
                m_record.insert(QStringLiteral("no_birth"), true);
                break;
            default:
                return Ack::REJECTED;
        }
        return persist() ? Ack::RETAINED : Ack::REJECTED;
    }

private:
    bool persist() const
    {
        QSaveFile file(m_path);
        const QByteArray bytes = QJsonDocument(m_record).toJson(QJsonDocument::Compact);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    }

    QString m_path;
    QJsonObject m_record;
    Terminal_native_receipt m_identity;
    custody::Native_process_status_observer m_helper;
    custody::Native_handle m_root_observer = custody::k_invalid_handle;
    std::mutex m_mutex;
};

} // namespace

Terminal_native_receipt_hook make_standalone_native_receipt_hook()
{
    auto receipts = std::make_shared<Standalone_native_receipts>();
    return [receipts](const Terminal_native_receipt& receipt) { return receipts->retain(receipt); };
}

} // namespace vnm_terminal::terminal_app
#else
namespace vnm_terminal::terminal_app {

Terminal_native_receipt_hook make_standalone_native_receipt_hook() { return {}; }

} // namespace vnm_terminal::terminal_app
#endif
