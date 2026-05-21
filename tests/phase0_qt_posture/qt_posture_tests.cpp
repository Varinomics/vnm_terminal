#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Case_result
{
    int            exit_code = 0;
    std::string    output;
};

bool fail(const std::string& message)
{
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

std::string quote_for_command(const fs::path& path)
{
    return "\"" + path.string() + "\"";
}

void write_text_file(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string synthetic_project(
    const fs::path&    posture_file,
    const std::string& route,
    const std::string& core_type,
    const std::string& gui_type,
    const std::string& quick_type,
    const std::string& direct_targets,
    const std::string& extra_lines)
{
    std::ostringstream out;
    out << "cmake_minimum_required(VERSION 3.21)\n";
    out << "project(qt_posture_case NONE)\n";
    out << "add_library(Qt6::Core " << core_type << " IMPORTED)\n";
    out << "add_library(Qt6::Gui " << gui_type << " IMPORTED)\n";
    out << "add_library(Qt6::Quick " << quick_type << " IMPORTED)\n";
    out << "add_library(Qt6::Network SHARED IMPORTED)\n";
    out << "set(VNM_TERMINAL_QT_LICENSE_ROUTE \"" << route << "\" CACHE STRING \"\")\n";
    out << "include(\"" << posture_file.generic_string() << "\")\n";
    out << "vnm_terminal_validate_qt_posture(DIRECT_TARGETS " << direct_targets << ")\n";
    out << extra_lines;
    return out.str();
}

Case_result run_case(
    const std::string& cmake_exe,
    const std::string& generator,
    const fs::path&    project_dir,
    const std::string& label)
{
    const fs::path output_path = project_dir / (label + ".out");
    const fs::path build_dir   = project_dir / "build";

#if defined(_WIN32)
    const std::string command = "cmd.exe /S /C \"\"" + cmake_exe + "\" -S " +
        quote_for_command(project_dir) + " -B " + quote_for_command(build_dir) +
        " -G \"" + generator + "\" > " + quote_for_command(output_path) + " 2>&1\"";
#else
    const std::string command = quote_for_command(cmake_exe) + " -S " +
        quote_for_command(project_dir) + " -B " + quote_for_command(build_dir) +
        " -G \"" + generator + "\" > " + quote_for_command(output_path) + " 2>&1";
#endif

    Case_result result;
    result.exit_code = std::system(command.c_str());

    std::ifstream in(output_path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    result.output = out.str();
    return result;
}

bool expect_success(const Case_result& result, const std::string& label)
{
    if (result.exit_code == 0) {
        return true;
    }

    return fail(label + " failed unexpectedly:\n" + result.output);
}

bool expect_failure(
    const Case_result& result,
    const std::string& label,
    const std::string& expected)
{
    if (result.exit_code != 0 && result.output.find(expected) != std::string::npos) {
        return true;
    }

    return fail(label + " did not fail with expected text: " + expected + "\n" + result.output);
}

}

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::cerr << "usage: qt_posture_tests <cmake> <generator> <repo-root> <work-dir>\n";
        return 2;
    }

    const std::string cmake_exe    = argv[1];
    const std::string generator    = argv[2];
    const fs::path    repo_root    = argv[3];
    const fs::path    work_dir     = argv[4];
    const fs::path    posture_file = repo_root / "cmake" / "vnm_terminal_qt_posture.cmake";

    fs::create_directories(work_dir);

    bool ok = true;

    const auto run_script =
        [&](const std::string& label,
            const std::string& route,
            const std::string& core_type,
            const std::string& gui_type,
            const std::string& quick_type,
            const std::string& direct_targets,
            const std::string& extra_lines) {
            const fs::path project_dir = work_dir / label;
            write_text_file(project_dir / "CMakeLists.txt",
                synthetic_project(
                    posture_file,
                    route,
                    core_type,
                    gui_type,
                    quick_type,
                    direct_targets,
                    extra_lines));
            return run_case(cmake_exe, generator, project_dir, label);
        };

    ok &= expect_success(
        run_script("allowed_shared", "lgpl_dynamic", "SHARED", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick", ""),
        "allowed_shared");

    ok &= expect_failure(
        run_script("forbidden_qt_module", "lgpl_dynamic", "SHARED", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network", ""),
        "forbidden_qt_module",
        "Qt target Qt6::Network is outside the Qt module allowlist");

    ok &= expect_failure(
        run_script("commercial_forbidden_qt_module",
            "commercial",
            "SHARED",
            "SHARED",
            "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network",
            ""),
        "commercial_forbidden_qt_module",
        "Qt target Qt6::Network is outside the Qt module allowlist");

    ok &= expect_failure(
        run_script("forbidden_link_interface", "lgpl_dynamic", "SHARED", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick",
            "add_library(example INTERFACE)\n"
            "target_link_libraries(example INTERFACE Qt6::Network)\n"
            "vnm_terminal_validate_qt_link_interface(example)\n"),
        "forbidden_link_interface",
        "Qt target Qt6::Network is outside the Qt module allowlist");

    ok &= expect_failure(
        run_script("forbidden_link_libraries", "lgpl_dynamic", "SHARED", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick",
            "file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/empty.cpp \"void f() {}\\n\")\n"
            "add_library(example STATIC empty.cpp)\n"
            "target_link_libraries(example PRIVATE Qt6::Network)\n"
            "vnm_terminal_validate_qt_link_interface(example)\n"),
        "forbidden_link_libraries",
        "Qt target Qt6::Network is outside the Qt module allowlist");

    ok &= expect_failure(
        run_script("forbidden_genex_qt_module", "lgpl_dynamic", "SHARED", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick",
            "add_library(example INTERFACE)\n"
            "target_link_libraries(example INTERFACE $<LINK_ONLY:Qt6::Network>)\n"
            "vnm_terminal_validate_qt_link_interface(example)\n"),
        "forbidden_genex_qt_module",
        "Qt generator expression $<LINK_ONLY:Qt6::Network> is outside");

    ok &= expect_failure(
        run_script("static_lgpl", "lgpl_dynamic", "STATIC", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick", ""),
        "static_lgpl",
        "requires Qt6::Core to be a shared library");

    ok &= expect_failure(
        run_script("static_lgpl_gui", "lgpl_dynamic", "SHARED", "STATIC", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick", ""),
        "static_lgpl_gui",
        "requires Qt6::Gui to be a shared library");

    ok &= expect_failure(
        run_script("static_lgpl_quick", "lgpl_dynamic", "SHARED", "SHARED", "STATIC",
            "Qt6::Core Qt6::Gui Qt6::Quick", ""),
        "static_lgpl_quick",
        "requires Qt6::Quick to be a shared library");

    ok &= expect_success(
        run_script("static_commercial", "commercial", "STATIC", "STATIC", "STATIC",
            "Qt6::Core Qt6::Gui Qt6::Quick", ""),
        "static_commercial");

    ok &= expect_failure(
        run_script("bad_route", "unknown", "SHARED", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick", ""),
        "bad_route",
        "VNM_TERMINAL_QT_LICENSE_ROUTE must be lgpl_dynamic or commercial");

    ok &= expect_failure(
        run_script("bad_arguments", "lgpl_dynamic", "SHARED", "SHARED", "SHARED",
            "Qt6::Core Qt6::Gui Qt6::Quick",
            "vnm_terminal_validate_qt_posture(DIRECT_TARGTS Qt6::Network)\n"),
        "bad_arguments",
        "Unexpected Qt posture arguments");

    return ok ? 0 : 1;
}
