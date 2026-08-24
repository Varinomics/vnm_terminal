function Component()
{
    if (!installer.isInstaller())
        return;

    if (!installer.addWizardPage(
            component,
            "ReplacementCommitPage",
            QInstaller.PerformInstallation))
    {
        throw new Error("Could not add the installation summary page.");
    }
    installer.setValidatorForCustomPage(
        component,
        "ReplacementCommitPage",
        "validateReplacementCommitPage");
}

Component.prototype.replacementTargetValueName =
    "VnmReplacementTargetDirectory";
Component.prototype.replacementRemovalCompleted = false;
Component.prototype.replacementCommitted = false;
Component.prototype.maintenanceToolFileName =
    "vnm_terminal_maintenance.exe";

Component.prototype.cleanedTargetDirectory = function(directory)
{
    var targetDirectory = installer.toNativeSeparators(directory);
    while (targetDirectory.length > 3 &&
        /[\\\/]$/.test(targetDirectory))
    {
        targetDirectory = targetDirectory.substring(
            0,
            targetDirectory.length - 1);
    }
    return targetDirectory;
}

Component.prototype.DynamicReplacementCommitPageCallback = function()
{
    var page = gui.pageWidgetByObjectName(
        "DynamicReplacementCommitPage");
    var targetDirectory = Component.prototype.cleanedTargetDirectory(
        installer.value("TargetDir"));
    var replacementDirectory = installer.value(
        Component.prototype.replacementTargetValueName);

    page.TargetLabel.text = targetDirectory;
    if (replacementDirectory != "")
    {
        page.HeadingLabel.text = "Ready to replace vnm_terminal";
        page.SummaryLabel.text =
            "Setup will remove the existing installation and install this version in the same folder.";
        page.CommitLabel.text =
            "Close vnm_terminal, then select Install. Windows authorization may be required to remove the old installation and install this version.";
    }
    else
    {
        page.HeadingLabel.text = "Ready to install vnm_terminal";
        page.SummaryLabel.text =
            "Setup is ready to install vnm_terminal in this folder.";
        page.CommitLabel.text = "Select Install to begin.";
    }
    gui.setButtonText(buttons.NextButton, "Install");
}

Component.prototype.runningTargetPaths = function(targetDirectory)
{
    var launcherPath = targetDirectory + "\\vnm_terminal.exe";
    var runtimePath = targetDirectory
        + "\\vnm_terminal_runtime\\vnm_terminal.exe";
    var runningPaths = [];

    if (installer.isProcessRunning(launcherPath))
        runningPaths.push(launcherPath);
    if (installer.isProcessRunning(runtimePath))
        runningPaths.push(runtimePath);
    return runningPaths;
}

Component.prototype.confirmTargetProcessesStopped = function(targetDirectory)
{
    var runningPaths = Component.prototype.runningTargetPaths(
        targetDirectory);
    if (runningPaths.length == 0)
        return true;

    var response = QMessageBox.warning(
        "ExistingInstallationStillRunning",
        "Close vnm_terminal",
        "Setup cannot replace files while vnm_terminal is running:\n\n"
            + runningPaths.join("\n")
            + "\n\nClose the listed process and select Retry, or select Cancel to exit setup.",
        QMessageBox.Retry | QMessageBox.Cancel,
        QMessageBox.Retry);
    if (response == QMessageBox.Cancel)
        gui.rejectWithoutPrompt();
    return false;
}

Component.prototype.runMaintenancePurge = function(targetDirectory)
{
    var maintenanceTool = targetDirectory + "\\"
        + Component.prototype.maintenanceToolFileName;
    if (!/^(?:[A-Za-z]:[\\/]|\\\\)/.test(maintenanceTool) ||
        !installer.fileExists(maintenanceTool))
    {
        return { state: "notStarted" };
    }

    var rootDirectory = installer.toNativeSeparators(
        installer.value("RootDir"));
    var powershellPath = rootDirectory
        + "Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (!installer.fileExists(powershellPath))
        return { state: "notStarted" };

    var literal = Component.prototype.powershellLiteral;
    var command = "$ErrorActionPreference='Stop'"
        + ";try{$process=Start-Process"
        + " -FilePath " + literal(maintenanceTool)
        + " -ArgumentList @(" + literal("purge") + ","
        + literal("--accept-messages") + ","
        + literal("--confirm-command") + ")"
        + " -Verb RunAs -WindowStyle Hidden -Wait -PassThru"
        + " -ErrorAction Stop"
        + ";Write-Output ('VNM_PURGE_EXIT:'"
        + "+[string]$process.ExitCode);exit 0}"
        + "catch{Write-Output 'VNM_PURGE_START_FAILED';exit 2}";

    var result = installer.execute(
        powershellPath,
        ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command]);
    if (result.length != 2)
        return { state: "notStarted" };

    var output = result[0].trim();
    if (output == "VNM_PURGE_START_FAILED")
        return { state: "notStarted" };

    var exitCodeMatch = /^VNM_PURGE_EXIT:(-?\d+)$/.exec(output);
    if (result[1] != 0 || exitCodeMatch == null)
        return { state: "failed" };

    var exitCode = parseInt(exitCodeMatch[1], 10);
    if (exitCode != 0)
        return { state: "failed", exitCode: exitCode };
    return { state: "succeeded", exitCode: exitCode };
}

Component.prototype.waitForDirectoryRemoval = function(targetDirectory)
{
    var rootDirectory = installer.toNativeSeparators(
        installer.value("RootDir"));
    var powershellPath = rootDirectory
        + "Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (!installer.fileExists(powershellPath))
        return false;

    var literal = Component.prototype.powershellLiteral(targetDirectory);
    var command = "$path=" + literal
        + ";$deadline=(Get-Date).AddSeconds(30)"
        + ";while(Test-Path -LiteralPath $path){"
        + "if((Get-Date) -ge $deadline){exit 2}"
        + ";Start-Sleep -Milliseconds 100}"
        + ";exit 0";
    var waitResult = installer.execute(
        powershellPath,
        ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command]);
    return waitResult.length == 2 && waitResult[1] == 0;
}

Component.prototype.reportReplacementFailure = function(
    targetDirectory, detail)
{
    var detailText = detail == "" ? "" : " " + detail;
    QMessageBox.critical(
        "ExistingInstallationRemovalFailed",
        "Could not replace the existing installation",
        "Setup could not confirm that the existing installation in "
            + targetDirectory + " was fully removed. "
            + "This version has not been installed, and the previous installation may now be incomplete. "
            + "Close setup, remove the remaining installation manually, and then run setup again."
            + detailText);
}

Component.prototype.reportRemovalNotStarted = function(targetDirectory)
{
    QMessageBox.critical(
        "ExistingInstallationRemovalNotStarted",
        "Removal authorization was not completed",
        "Windows did not start the existing installation's removal tool in "
            + targetDirectory + ". The existing installation is presumed unchanged, and this version has not been installed. "
            + "You can select Install to try again or close setup.");
}

Component.prototype.reportNewInstallerAuthorizationFailure = function(
    detail)
{
    var message =
        "The existing installation was removed, but setup was not given administrator permission to install this version. "
        + "This version has not been installed. Run setup again to complete installation.";
    if (detail != "")
        message += "\n\n" + detail;
    QMessageBox.critical(
        "NewInstallerAuthorizationFailed",
        "Administrator permission is required",
        message);
}

Component.prototype.validateReplacementCommitPage = function()
{
    if (Component.prototype.replacementCommitted)
        return true;

    var replacementDirectory = installer.value(
        Component.prototype.replacementTargetValueName);
    if (replacementDirectory == "")
        return true;

    if (installer.hasAdminRights())
    {
        QMessageBox.critical(
            "ReplacementSetupAlreadyElevated",
            "Start setup normally",
            "Automatic replacement cannot run when setup was started as administrator. Close setup and start it normally, without selecting Run as administrator, so Windows can authorize the installed removal tool separately.");
        return false;
    }

    var targetDirectory = Component.prototype.cleanedTargetDirectory(
        installer.value("TargetDir"));
    var expectedDirectory = Component.prototype.cleanedTargetDirectory(
        replacementDirectory);
    if (targetDirectory.toLowerCase() != expectedDirectory.toLowerCase())
    {
        QMessageBox.critical(
            "ExistingInstallationTargetChanged",
            "The installation folder changed",
            "Setup cannot safely replace the existing installation because its installation folder changed. Close setup and run it again.");
        return false;
    }

    if (!Component.prototype.replacementRemovalCompleted)
    {
        if (!Component.prototype.confirmTargetProcessesStopped(
                targetDirectory))
        {
            return false;
        }

        var purgeResult = Component.prototype.runMaintenancePurge(
            targetDirectory);
        if (purgeResult.state == "notStarted")
        {
            Component.prototype.reportRemovalNotStarted(targetDirectory);
            return false;
        }
        if (purgeResult.state != "succeeded")
        {
            var purgeDetail = purgeResult.exitCode === undefined
                ? ""
                : "The removal tool returned exit code "
                    + purgeResult.exitCode + ".";
            Component.prototype.reportReplacementFailure(
                targetDirectory, purgeDetail);
            return false;
        }
        if (!Component.prototype.waitForDirectoryRemoval(targetDirectory))
        {
            Component.prototype.reportReplacementFailure(
                targetDirectory,
                "The removal tool exited successfully, but setup could not confirm that its folder was removed.");
            return false;
        }
        Component.prototype.replacementRemovalCompleted = true;
    }

    try
    {
        if (!installer.gainAdminRights())
        {
            Component.prototype.reportNewInstallerAuthorizationFailure("");
            return false;
        }
    }
    catch (error)
    {
        Component.prototype.reportNewInstallerAuthorizationFailure(
            String(error));
        return false;
    }

    Component.prototype.replacementCommitted = true;
    return true;
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
