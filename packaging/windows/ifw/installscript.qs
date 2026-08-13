function Component()
{
}

Component.prototype.normalizedStartMenuPath = function(path)
{
    var normalized = path.replace(/\\/g, "/");
    var isUncPath = normalized.indexOf("//") == 0;
    normalized = normalized.replace(/\/+/g, "/");
    if (isUncPath)
        normalized = "/" + normalized;

    while (normalized.length > 1 &&
        normalized.charAt(normalized.length - 1) == "/")
    {
        normalized = normalized.substring(0, normalized.length - 1);
    }
    return normalized;
}

Component.prototype.powershellLiteral = function(value)
{
    return "'" + value.replace(/'/g, "''") + "'";
}

Component.prototype.relativeStartMenuGroup = function()
{
    var selectedDirectory = Component.prototype.normalizedStartMenuPath(
        installer.value("StartMenuDir"));
    var userPrograms = Component.prototype.normalizedStartMenuPath(
        installer.value("UserStartMenuProgramsPath"));
    var allUsersPrograms = Component.prototype.normalizedStartMenuPath(
        installer.value("AllUsersStartMenuProgramsPath"));
    var rootDirectory = installer.toNativeSeparators(
        installer.value("RootDir"));
    var powershellPath = rootDirectory
        + "Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";

    if (!installer.fileExists(powershellPath))
        throw new Error("Windows PowerShell is required to validate the Start Menu folder.");

    var literal = Component.prototype.powershellLiteral;
    var command = "[Console]::OutputEncoding="
        + "[Text.UTF8Encoding]::new($false)"
        + ";$selected=" + literal(selectedDirectory)
        + ";$roots=@(" + literal(userPrograms) + ","
        + literal(allUsersPrograms) + ")"
        + ";$comparison=[StringComparison]::OrdinalIgnoreCase"
        + ";$relative=$null"
        + ";foreach($root in $roots){"
        + "if([string]::IsNullOrEmpty($root)){continue}"
        + ";if($selected.Length -eq $root.Length -and "
        + "$selected.Equals($root,$comparison)){$relative='';break}"
        + ";if($selected.Length -le $root.Length -or "
        + "$selected[$root.Length] -ne '/' -or "
        + "-not $selected.StartsWith($root,$comparison)){continue}"
        + ";$candidate=$selected.Substring($root.Length+1)"
        + ";foreach($segment in $candidate.Split('/')){"
        + "if($segment -eq '.' -or $segment -eq '..'){exit 2}}"
        + ";$relative=$candidate;break}"
        + ";if($null -eq $relative){exit 2}"
        + ";Write-Output ('VNM_GROUP:'+$relative)";
    var result = installer.execute(
        powershellPath,
        ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", "-"],
        command,
        "UTF-8",
        "UTF-8");
    if (result.length != 2 || result[1] != 0)
        throw new Error(
            "The selected Start Menu folder is outside the supported Programs directories.");

    var output = result[0].replace(/\r?\n$/, "");
    if (output.indexOf("VNM_GROUP:") != 0)
        throw new Error("Windows PowerShell returned an invalid Start Menu folder result.");
    return output.substring("VNM_GROUP:".length);
}

Component.prototype.allUsersShortcutPath = function()
{
    var allUsersPrograms = Component.prototype.normalizedStartMenuPath(
        installer.value("AllUsersStartMenuProgramsPath"));
    if (allUsersPrograms == "")
        throw new Error("The all-users Start Menu Programs directory is unavailable.");

    // IFW 4.11 stores the final GUI selection below the user Programs root.
    // Retain that relative group while keeping the shortcut's adopted
    // all-users scope.
    var group = Component.prototype.relativeStartMenuGroup();
    if (group != "")
        allUsersPrograms += "/" + group;
    return allUsersPrograms + "/vnm_terminal.lnk";
}

Component.prototype.createOperations = function()
{
    var shortcutPath = Component.prototype.allUsersShortcutPath();
    component.createOperations();
    component.addElevatedOperation(
        "CreateShortcut",
        "@TargetDir@/vnm_terminal.exe",
        shortcutPath);
}
