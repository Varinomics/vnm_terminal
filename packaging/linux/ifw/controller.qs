function Controller()
{
    if (installer.isInstaller())
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
}

Controller.prototype.ReadyForInstallationPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var summaryPage = gui.pageWidgetByObjectName("ReadyForInstallationPage");
    summaryPage.subTitle = "Review your choices before installation.";
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
Controller.prototype.maintenanceToolFileName = "vnm_terminal_maintenance";

// An offline installer has no update mode, so installing this version over an
// existing one means removing that installation first. Its own maintenance
// tool owns the removal: it undoes the recorded operations, drops the launcher
// symlink and the desktop entry, and deletes the installation directory.
// Extracting over the files instead would leave every file the previous
// version owned and this one does not.
Controller.prototype.offerToRemoveExistingInstallation = function()
{
    var targetDirectory = installer.value("TargetDir");
    var maintenanceToolPath =
        targetDirectory + "/" + Controller.prototype.maintenanceToolFileName;
    if (!installer.fileExists(maintenanceToolPath))
        return;

    var answer = QMessageBox.question(
        "RemoveExistingInstallation",
        "Existing installation",
        "vnm_terminal is already installed in " + targetDirectory + ".\n\n"
        + "Setup can remove that installation and then install this version "
        + "into the same directory. Removal can take a few moments.\n\n"
        + "Remove the existing installation?",
        QMessageBox.Yes | QMessageBox.No);
    if (answer != QMessageBox.Yes)
        return;

    // The maintenance tool deletes itself and then the installation directory
    // before its own process exits, so the directory is a settled result by
    // the time this call returns.
    var result = installer.execute(
        maintenanceToolPath,
        ["purge", "--accept-messages", "--confirm-command"]);
    if (result.length == 2 && result[1] == 0 &&
        !installer.fileExists(targetDirectory))
    {
        return;
    }

    QMessageBox.critical(
        "ExistingInstallationRetained",
        "Error",
        "Setup could not remove the installation in " + targetDirectory
        + ".\n\nRun this installer with the privileges that directory needs, "
        + "remove the installation with " + maintenanceToolPath
        + ", or choose a different directory.");
}

Controller.prototype.LicenseAgreementPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var licenseAgreementPage = gui.pageWidgetByObjectName("LicenseAgreementPage");
    licenseAgreementPage.subTitle =
        "Review and accept the license to continue.";
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
