function Controller()
{
    // IFW loads the control script before it presents the wizard.
    if (installer.isInstaller()) {
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
        Controller.prototype.detectExistingInstallations();
    }
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

    var ambiguousDirectories =
        Controller.prototype.ambiguousInstallationDirectories;
    if (ambiguousDirectories.length > 1) {
        if (!Controller.prototype.reportedAmbiguousInstallations) {
            Controller.prototype.reportedAmbiguousInstallations = true;
            QMessageBox.critical(
                "MultipleExistingInstallations",
                "Multiple installations found",
                "Setup found more than one managed vnm_terminal "
                + "installation:\n\n- "
                + ambiguousDirectories.join("\n- ")
                + "\n\nSetup cannot choose which uninstaller to open. "
                + "Remove the extra installations manually, then run setup "
                + "again.");
        }
        gui.rejectWithoutPrompt();
        return;
    }

    Controller.prototype.offerExistingInstallationUninstaller();
    if (Controller.prototype.existingInstallationDirectory != "")
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

    // A failed detached launch leaves the recovery instructions visible on
    // Welcome. Trying to advance closes this setup instead of exposing IFW's
    // unusable install-over-install path.
    if (Controller.prototype.existingInstallationDirectory != "") {
        gui.rejectWithoutPrompt();
        return;
    }

    var targetDirectoryPage = gui.pageWidgetByObjectName("TargetDirectoryPage");
    targetDirectoryPage.subTitle =
        "Choose where vnm_terminal will be installed.";
}

// Must name the file config.xml declares through MaintenanceToolName. IFW
// refuses any target directory that contains it, which is how it recognizes
// one of its own installations.
Controller.prototype.maintenanceToolFileName = "vnm_terminal_maintenance";

// The one installation whose graphical uninstaller this setup can offer.
Controller.prototype.existingInstallationDirectory = "";

// Every live installation found when setup cannot choose one safely.
Controller.prototype.ambiguousInstallationDirectories = [];

// Introduction can be entered more than once while setup is closing.
Controller.prototype.reportedAmbiguousInstallations = false;

// Introduction may be entered again while setup or its modal dialogs close.
// One process must never prompt for or launch the same handoff twice.
Controller.prototype.existingInstallationHandoffHandled = false;

// The installed package owns this launcher link. Its target preserves the
// selected TargetDir even when that directory was customized.
Controller.prototype.launcherLinkPath = "/usr/local/bin/vnm_terminal";

Controller.prototype.launcherInstallationDirectory = function()
{
    var launcherLinkPath = Controller.prototype.launcherLinkPath;
    if (!installer.fileExists(launcherLinkPath))
        return "";

    var result = installer.execute(
        "/usr/bin/readlink",
        ["-e", "--", launcherLinkPath],
        "",
        "UTF-8",
        "UTF-8");
    if (result.length != 2 || result[1] != 0)
        return "";

    var executablePath = result[0].trim();
    var executableSuffix = "/bin/vnm_terminal";
    if (executablePath.length <= executableSuffix.length ||
        executablePath.substring(
            executablePath.length - executableSuffix.length) !=
            executableSuffix)
    {
        return "";
    }
    return executablePath.substring(
        0, executablePath.length - executableSuffix.length);
}

Controller.prototype.appendExistingInstallationDirectory = function(
    directories, directory)
{
    while (directory.length > 1 && /\/$/.test(directory))
        directory = directory.substring(0, directory.length - 1);

    if (directory.indexOf("/") != 0 ||
        !installer.fileExists(directory + "/"
            + Controller.prototype.maintenanceToolFileName))
    {
        return;
    }

    for (var i = 0; i < directories.length; ++i) {
        if (directories[i] == directory)
            return;
    }
    directories.push(directory);
}

Controller.prototype.detectExistingInstallations = function()
{
    Controller.prototype.existingInstallationDirectory = "";
    Controller.prototype.ambiguousInstallationDirectories = [];
    Controller.prototype.reportedAmbiguousInstallations = false;
    Controller.prototype.existingInstallationHandoffHandled = false;

    var directories = [];
    Controller.prototype.appendExistingInstallationDirectory(
        directories, installer.value("TargetDir"));
    Controller.prototype.appendExistingInstallationDirectory(
        directories, Controller.prototype.launcherInstallationDirectory());

    if (directories.length > 1) {
        Controller.prototype.ambiguousInstallationDirectories = directories;
        return;
    }
    if (directories.length == 0)
        return;

    Controller.prototype.existingInstallationDirectory = directories[0];
}

Controller.prototype.offerExistingInstallationUninstaller = function()
{
    var targetDirectory =
        Controller.prototype.existingInstallationDirectory;
    if (targetDirectory == "" ||
        Controller.prototype.existingInstallationHandoffHandled)
    {
        return;
    }

    Controller.prototype.existingInstallationHandoffHandled = true;
    var maintenanceToolPath =
        targetDirectory + "/" + Controller.prototype.maintenanceToolFileName;
    var response = QMessageBox.question(
        "ExistingInstallationFound",
        "Existing installation found",
        "vnm_terminal is already installed in:\n\n"
            + targetDirectory
            + "\n\nThis offline installer does not replace an existing "
            + "installation directly. Open its uninstaller now? Nothing is "
            + "removed until you confirm removal there. After removal "
            + "finishes, run this setup again.",
        QMessageBox.Yes | QMessageBox.No,
        QMessageBox.Yes);
    if (response == QMessageBox.No) {
        gui.rejectWithoutPrompt();
        return;
    }

    // executeDetached inherits setup's token. Never let an elevated setup
    // launch a maintenance path discovered from user-writable state; restart
    // setup normally so the maintenance GUI requests only its own permissions.
    if (installer.hasAdminRights()) {
        QMessageBox.critical(
            "ElevatedExistingInstallationHandoff",
            "Start setup normally",
            "Setup is running with administrator rights and will not open "
                + "the existing installation's maintenance tool, because it "
                + "would inherit those rights. Start setup normally so the "
                + "maintenance tool can request its own permissions, or open "
                + "this maintenance tool manually:\n\n" + maintenanceToolPath);
        gui.rejectWithoutPrompt();
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
        gui.rejectWithoutPrompt();
        return;
    }

    QMessageBox.critical(
        "ExistingInstallationUninstallerLaunchFailed",
        "Could not open the uninstaller",
        "Setup could not open:\n\n" + maintenanceToolPath
            + "\n\nNo removal was started. Open that maintenance tool "
            + "manually, remove vnm_terminal, close this setup, then run "
            + "setup again.");
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
