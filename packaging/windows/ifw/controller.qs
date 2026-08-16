function Controller()
{
    // IFW loads the control script before it presents the wizard. Component
    // scripts load later, after the initial page list has already been shown.
    if (installer.isInstaller()) {
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
        Controller.prototype.configureUserLogFile();
    }
}

Controller.prototype.configureUserLogFile = function()
{
    // This absolute device path safely disables file output if the exact-file
    // path probe cannot run or cannot prove a candidate. It can never be
    // resolved relative to TargetDir.
    installer.setValue("LogFileName", "\\\\.\\NUL");

    var rootDirectory = installer.toNativeSeparators(installer.value("RootDir"));
    var powershellPath = rootDirectory
        + "Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    var probeScript = installer.readFile(
        ":/metadata/installer-theme/log_path_probe.ps1", "UTF-8");
    if (!installer.fileExists(powershellPath) || probeScript == "")
        return;

    var result = installer.execute(
        powershellPath,
        ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", "-"],
        probeScript,
        "UTF-8",
        "UTF-8");
    if (result.length != 2 || result[1] != 0)
        return;

    var logFileName = result[0].trim();
    if (!/^(?:[A-Za-z]:[\\\\/]|\\\\\\\\)/.test(logFileName))
        return;

    // The helper already created this unique file exclusively and closed it.
    // IFW later reopens the same file with ReadWrite | Append | Text.
    // A policy or ACL change after this proof remains an unavoidable race for
    // any external logger; retaining the unique file removes name collisions.
    installer.setValue(
        "LogFileName", installer.toNativeSeparators(logFileName));
}

Controller.prototype.ReadyForInstallationPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var summaryPage = gui.pageWidgetByObjectName("ReadyForInstallationPage");
    summaryPage.subTitle = "Review your choices before installation.";

    // IFW's component size excludes the maintenance-tool resources that its
    // labeled SpaceWidget includes. Hide the ambiguous, unlabeled tree value.
    var installComponentsTreeview =
        gui.findChild(summaryPage, "InstallComponentsTreeview");
    installComponentsTreeview.hideColumn(5);
}

Controller.prototype.IntroductionPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var introductionPage = gui.pageWidgetByObjectName("IntroductionPage");

    introductionPage.title = "Welcome";
    introductionPage.subTitle = "Install vnm_terminal on this computer.";
    introductionPage.MessageLabel.setText(
        "<div class=\"BrandPresentation\" style=\"color:#E0E0E0;\">"
        + "<span style=\"color:#999999;\">vnm_terminal</span>"
        + "<br /><span style=\"font-size:20px; font-weight:600;\">"
        + "A focused terminal for the desktop.</span>"
        + "<br /><br /><span>This setup will install vnm_terminal "
        + "and its required runtime.</span>"
        + "</div>");
}

Controller.prototype.TargetDirectoryPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var targetDirectoryPage = gui.pageWidgetByObjectName("TargetDirectoryPage");
    targetDirectoryPage.subTitle =
        "Choose where vnm_terminal will be installed.";

    Controller.prototype.offerToRemoveExistingInstallation();
}

// Must name the file config.xml declares through MaintenanceToolName. IFW
// refuses any target directory that contains it, which is how it recognizes
// one of its own installations.
Controller.prototype.maintenanceToolFileName = "vnm_terminal_maintenance.exe";

// An offline installer has no update mode, so installing this version over an
// existing one means removing that installation first. Its own maintenance
// tool owns the removal: it undoes the recorded operations, drops the Start
// Menu shortcut, and unregisters the Windows uninstall entry. Extracting over
// the files instead would strand that entry, because every installation
// registers itself under a freshly generated UUID.
Controller.prototype.offerToRemoveExistingInstallation = function()
{
    var targetDirectory =
        installer.toNativeSeparators(installer.value("TargetDir"));
    var maintenanceToolPath =
        targetDirectory + "\\" + Controller.prototype.maintenanceToolFileName;
    if (!installer.fileExists(maintenanceToolPath))
        return;

    var answer = QMessageBox.question(
        "RemoveExistingInstallation",
        "Existing installation",
        "vnm_terminal is already installed in " + targetDirectory + ".\n\n"
        + "Setup can remove that installation and then install this version "
        + "into the same folder. Removal needs the same permissions as the "
        + "installation and can take a few moments.\n\n"
        + "Remove the existing installation?",
        QMessageBox.Yes | QMessageBox.No);
    if (answer != QMessageBox.Yes)
        return;

    var removed = Controller.prototype.removeInstallation(
        maintenanceToolPath, targetDirectory);
    if (removed)
        return;

    QMessageBox.critical(
        "ExistingInstallationRetained",
        "Error",
        "Setup could not remove the installation in " + targetDirectory
        + ".\n\nClose vnm_terminal if it is running and try again, remove it "
        + "through Windows Settings, or choose a different folder.");
}

Controller.prototype.removeInstallation = function(
    maintenanceToolPath, targetDirectory)
{
    var result = installer.execute(
        maintenanceToolPath,
        ["purge", "--accept-messages", "--confirm-command"]);
    if (result.length != 2 || result[1] != 0)
        return false;

    // Windows cannot delete a running executable, so the maintenance tool
    // hands its own removal, and the directory's, to a detached script that
    // starts once its process has exited. That script reports to nobody, and
    // IFW rejects the directory as soon as this page is left, which leaves the
    // directory itself as the only available completion signal.
    Controller.prototype.waitForDirectoryRemoval(targetDirectory);
    return !installer.fileExists(targetDirectory);
}

Controller.prototype.waitForDirectoryRemoval = function(directory)
{
    var rootDirectory = installer.toNativeSeparators(installer.value("RootDir"));
    var powershellPath = rootDirectory
        + "Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    var command = "$deadline = [DateTime]::UtcNow.AddSeconds(30)"
        + ";while ([DateTime]::UtcNow -lt $deadline -and "
        + "(Test-Path -LiteralPath "
        + Controller.prototype.powershellLiteral(directory) + "))"
        + "{Start-Sleep -Milliseconds 250}";
    installer.execute(
        powershellPath,
        ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", "-"],
        command,
        "UTF-8",
        "UTF-8");
}

Controller.prototype.powershellLiteral = function(value)
{
    return "'" + value.replace(/'/g, "''") + "'";
}

Controller.prototype.LicenseAgreementPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var licenseAgreementPage = gui.pageWidgetByObjectName("LicenseAgreementPage");
    licenseAgreementPage.subTitle =
        "Review and accept the license to continue.";
}

Controller.prototype.StartMenuDirectoryPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var startMenuDirectoryPage =
        gui.pageWidgetByObjectName("StartMenuDirectoryPage");
    startMenuDirectoryPage.subTitle =
        "Choose where Start Menu shortcuts will appear.";
}

Controller.prototype.PerformInstallationPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var performInstallationPage =
        gui.pageWidgetByObjectName("PerformInstallationPage");
    performInstallationPage.subTitle =
        "Installing vnm_terminal. Please wait.";
}

Controller.prototype.escapeHtml = function(text)
{
    return text
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/\"/g, "&quot;")
        .replace(/'/g, "&#39;");
}

Controller.prototype.FinishedPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var finishedPage = gui.pageWidgetByObjectName("FinishedPage");
    var frameworkMessage = finishedPage.MessageLabel.text;
    var heading;
    var detail;

    if (installer.status == QInstaller.Success) {
        finishedPage.title = "Finished";
        finishedPage.subTitle = "Installation completed successfully.";
        heading = "vnm_terminal is ready.";
        detail = "Installation completed successfully.";
    }
    else
    if (installer.status == QInstaller.Canceled) {
        finishedPage.title = "Installation canceled";
        finishedPage.subTitle = "Setup stopped at your request.";
        heading = "Installation was canceled.";
        detail = "No successful installation was recorded.<br /><br />"
            + Controller.prototype.escapeHtml(frameworkMessage);
    }
    else
    if (installer.status == QInstaller.Unfinished) {
        finishedPage.title = "Installation incomplete";
        finishedPage.subTitle = "Setup ended before installation completed.";
        heading = "Installation did not complete.";
        detail = "Setup ended before installation could be completed."
            + "<br /><br />"
            + Controller.prototype.escapeHtml(frameworkMessage);
    }
    else {
        finishedPage.title = "Installation failed";
        finishedPage.subTitle = "Setup could not complete the installation.";
        heading = "Installation failed.";
        detail = "Close setup and try again.<br /><br />"
            + Controller.prototype.escapeHtml(frameworkMessage);
    }

    finishedPage.MessageLabel.setText(
        "<div class=\"BrandFinished\" style=\"color:#E0E0E0;\">"
        + "<span style=\"font-size:20px; font-weight:600;\">"
        + heading + "</span>"
        + "<br /><br /><span>" + detail + "</span>"
        + "</div>");

    if (installer.status != QInstaller.Success)
        finishedPage.RunItCheckBox.hide();
}
