#pragma once

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// Strings are borrowed for the duration of portable_launcher_run. The runtime path
// starts with a separator and is relative to the packaged launcher's directory.
typedef struct portable_launcher_config_t
{
    const wchar_t* title;
    const wchar_t* runtime_relative_path;
    const wchar_t* app_user_model_id_or_null;
    const wchar_t* locate_error;
    const wchar_t* runtime_path_too_long;
    const wchar_t* runtime_missing;
    const wchar_t* command_line_too_long;
} portable_launcher_config_t;

// Launches synchronously with the package directory as cwd and forwards argv[1..).
// Returns the child exit status, or 1 on launcher failure after showing a diagnostic.
// COM initialization is left to the application; an optional AppUserModelID applies
// only to this launcher process. No Qt or application runtime is loaded here.
int portable_launcher_run(const portable_launcher_config_t* in_config);

#ifdef __cplusplus
}
#endif
