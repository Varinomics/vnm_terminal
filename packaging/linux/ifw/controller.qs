function Controller()
{
    // IFW loads the control script before it presents the wizard.
    if (installer.isInstaller()) {
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
        Controller.prototype.promptedExistingInstallationDirectory = "";
        Controller.prototype.closeRequested = false;
    }
}

// Must name the file config.xml declares through MaintenanceToolName. IFW
// refuses any target directory that contains it, which is how it recognizes
// one of its own installations.
Controller.prototype.maintenanceToolFileName = "vnm_terminal_maintenance";

// The folder callback may be re-entered without the user changing TargetDir.
// Remember that exact answer, but let a changed path be evaluated afresh.
Controller.prototype.promptedExistingInstallationDirectory = "";

// IFW may re-enter a page callback while rejectWithoutPrompt is taking effect.
// Every such callback repeats the close request without repeating the launch.
Controller.prototype.closeRequested = false;

Controller.prototype.continueRequestedClose = function()
{
    if (!Controller.prototype.closeRequested)
        return false;
    gui.rejectWithoutPrompt();
    return true;
}

Controller.prototype.normalizedTargetDirectory = function()
{
    var targetDirectory = installer.value("TargetDir");
    while (targetDirectory.length > 1 && /\/$/.test(targetDirectory))
        targetDirectory = targetDirectory.substring(0, targetDirectory.length - 1);
    return targetDirectory;
}

Controller.prototype.offerSelectedInstallationUninstaller = function()
{
    var targetDirectory = Controller.prototype.normalizedTargetDirectory();
    var maintenanceToolPath =
        targetDirectory + "/" + Controller.prototype.maintenanceToolFileName;
    if (!installer.fileExists(maintenanceToolPath)) {
        Controller.prototype.promptedExistingInstallationDirectory = "";
        return;
    }
    if (targetDirectory ==
        Controller.prototype.promptedExistingInstallationDirectory)
    {
        return;
    }

    Controller.prototype.promptedExistingInstallationDirectory = targetDirectory;
    var response = QMessageBox.question(
        "ExistingInstallationFound",
        "Existing installation found",
        "vnm_terminal is already installed in:\n\n"
            + targetDirectory
            + "\n\nThis offline installer does not replace an existing "
            + "installation directly. Open its uninstaller now? Nothing is "
            + "removed until you confirm removal there. After removal "
            + "finishes, run this setup again. Select No to stay on this "
            + "page and choose another folder. A side-by-side copy is best "
            + "effort only: installations share the global launcher, desktop "
            + "entry, and user settings, so they are not independent.",
        QMessageBox.Yes | QMessageBox.No,
        QMessageBox.Yes);
    if (response == QMessageBox.No)
        return;

    // executeDetached inherits setup's token. Never let an elevated setup
    // launch a maintenance path selected from user-writable state. The folder
    // page remains usable so the user can choose another target.
    if (installer.hasAdminRights()) {
        QMessageBox.critical(
            "ElevatedExistingInstallationHandoff",
            "Start setup normally",
            "Setup is running with administrator rights and will not open "
                + "the existing installation's maintenance tool, because it "
                + "would inherit those rights. Start setup normally so the "
                + "maintenance tool can request its own permissions, or open "
                + "this maintenance tool manually:\n\n" + maintenanceToolPath
                + "\n\nYou can also choose another folder on this page.");
        return;
    }

    // The installed maintenance GUI owns its records, confirmation, and
    // elevation. With the inherited-token constraint above, this intentional
    // two-step handoff avoids deleting the old installation before the user
    // commits in its uninstaller. It does not promise transactional recovery;
    // setup is rerun after removal completes.
    if (installer.executeDetached(
            maintenanceToolPath,
            ["--start-uninstaller"],
            targetDirectory))
    {
        Controller.prototype.closeRequested = true;
        gui.rejectWithoutPrompt();
        return;
    }

    QMessageBox.critical(
        "ExistingInstallationUninstallerLaunchFailed",
        "Could not open the uninstaller",
        "Setup could not open:\n\n" + maintenanceToolPath
            + "\n\nNo removal was started. Open that maintenance tool "
            + "manually, or choose another folder on this page. After "
            + "removing vnm_terminal, run setup again.");
}

Controller.prototype.ReadyForInstallationPageCallback = function()
{
    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
        return;

    var summaryPage = gui.pageWidgetByObjectName("ReadyForInstallationPage");
    summaryPage.subTitle = "Review your choices before installation.";
}

Controller.prototype.IntroductionPageCallback = function()
{
    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
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
        + "and its required runtime.</span></div>");
}

Controller.prototype.TargetDirectoryPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var targetDirectoryPage = gui.pageWidgetByObjectName("TargetDirectoryPage");
    targetDirectoryPage.subTitle =
        "Choose where vnm_terminal will be installed.";
    if (Controller.prototype.continueRequestedClose())
        return;

    // Deliberately offer only the folder displayed when this page is entered.
    // This avoids startup scans and policy lockouts. Later edits belong to
    // IFW's validation; a free folder can proceed, while another managed
    // folder may receive IFW's native refusal. Side-by-side copies are best
    // effort because the global launcher, desktop entry, and settings are
    // shared. The installed GUI owns removal, confirmation, and elevation;
    // this manual two-step is preferred over non-transactional replacement.
    Controller.prototype.offerSelectedInstallationUninstaller();
}

Controller.prototype.LicenseAgreementPageCallback = function()
{
    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
        return;

    var licenseAgreementPage = gui.pageWidgetByObjectName("LicenseAgreementPage");
    licenseAgreementPage.subTitle =
        "Review and accept the license to continue.";
}

Controller.prototype.PerformInstallationPageCallback = function()
{
    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
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
    if (Controller.prototype.continueRequestedClose())
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
