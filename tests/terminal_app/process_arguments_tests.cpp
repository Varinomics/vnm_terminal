#include "process_arguments.h"

#include <QTest>

#include <string>

class Process_arguments_tests final : public QObject
{
    Q_OBJECT

private slots:
    void windows_command_line_data()
    {
        QTest::addColumn<QString>("command_line");
        QTest::addColumn<QStringList>("expected");

        QTest::newRow("unicode and empty argument")
            << QStringLiteral("\"C:\\terminal folder\\terminal.exe\" \"\u03bb-\u4e2d-\U0001f600\" \"\"")
            << QStringList({QStringLiteral("C:\\terminal folder\\terminal.exe"),
                            QStringLiteral("\u03bb-\u4e2d-\U0001f600"), QString()});
        QTest::newRow("quoted whitespace")
            << QStringLiteral("terminal.exe \"a b c\" d e")
            << QStringList({QStringLiteral("terminal.exe"), QStringLiteral("a b c"),
                            QStringLiteral("d"), QStringLiteral("e")});
        QTest::newRow("backslash before quote")
            << QStringLiteral("terminal.exe \"ab\\\"c\" \"\\\\\" d")
            << QStringList({QStringLiteral("terminal.exe"), QStringLiteral("ab\"c"),
                            QStringLiteral("\\"), QStringLiteral("d")});
        QTest::newRow("doubled quote")
            << QStringLiteral("terminal.exe \"a\"\"b\"")
            << QStringList({QStringLiteral("terminal.exe"), QStringLiteral("a\"b")});
        // The shell parser ends quote mode after the doubled quote here; the
        // CRT instead keeps the following spaces inside the same argument.
        QTest::newRow("shell parser quote boundary")
            << QStringLiteral("terminal.exe a\"b\"\" c d")
            << QStringList({QStringLiteral("terminal.exe"), QStringLiteral("ab\""),
                            QStringLiteral("c"), QStringLiteral("d")});
        QTest::newRow("quoted trailing backslash")
            << QStringLiteral("terminal.exe \"folder name\\\\\"")
            << QStringList({QStringLiteral("terminal.exe"), QStringLiteral("folder name\\")});
    }

    void windows_command_line()
    {
        QFETCH(QString, command_line);
        QFETCH(QStringList, expected);
        const std::wstring native_command_line = command_line.toStdWString();
        const auto result = vnm_terminal::terminal_app::windows_command_line_arguments(
            native_command_line.c_str());
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.arguments, expected);
    }
};

QTEST_GUILESS_MAIN(Process_arguments_tests)

#include "process_arguments_tests.moc"
