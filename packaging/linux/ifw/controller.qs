function Controller()
{
    // IFW loads the control script before it presents the wizard.
    if (installer.isInstaller()) {
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
        Controller.prototype.adoptExistingInstallation();
    }
}

Controller.prototype.ReadyForInstallationPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var summaryPage = gui.pageWidgetByObjectName("ReadyForInstallationPage");
    summaryPage.subTitle = Controller.prototype.replacedInstallationDirectory
        ? "Setup will replace the installation in "
            + Controller.prototype.replacedInstallationDirectory + "."
        : "Review your choices before installation.";
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
                "Only one managed vnm_terminal installation is supported. "
                + "Setup cannot safely repair or remove this ambiguous state "
                + "automatically.\n\nSetup found:\n- "
                + ambiguousDirectories.join("\n- ")
                + "\n\nRemove all listed installations and any remaining "
                + "shared launcher state before running setup for a fresh "
                + "installation. If cleanup or removal fails, contact "
                + "Varinomics support.");
        }
        gui.rejectWithoutPrompt();
        return;
    }

    var introductionPage = gui.pageWidgetByObjectName("IntroductionPage");

    var replacedDirectory = Controller.prototype.replacedInstallationDirectory;
    introductionPage.title = "Welcome";
    introductionPage.subTitle = replacedDirectory
        ? "Replace the installed vnm_terminal with this version."
        : "Install vnm_terminal on this computer.";
    introductionPage.MessageLabel.setText(
        "<div class=\"BrandPresentation\" style=\"color:#E0E0E0;\">"
        + "<span style=\"color:#999999;\">vnm_terminal</span>"
        + "<br /><span style=\"font-size:20px; font-weight:600;\">"
        + "A focused terminal for the desktop.</span>"
        + "<br /><br /><span>"
        + (replacedDirectory
            ? "vnm_terminal is already installed in "
                + Controller.prototype.escapeHtml(replacedDirectory)
                + ". This setup will remove that installation and install "
                + "this version in its place."
            : "This setup will install vnm_terminal and its required runtime.")
        + "</span></div>");
}

Controller.prototype.TargetDirectoryPageCallback = function()
{
    if (!installer.isInstaller())
        return;

    var targetDirectoryPage = gui.pageWidgetByObjectName("TargetDirectoryPage");
    targetDirectoryPage.subTitle =
        "Choose where vnm_terminal will be installed.";
}

// Must name the file config.xml declares through MaintenanceToolName. IFW
// refuses any target directory that contains it, which is how it recognizes
// one of its own installations.
Controller.prototype.maintenanceToolFileName = "vnm_terminal_maintenance";

// The installation this run replaces, empty when this run replaces none. Read
// by the pages that have to say so and by the removal itself.
Controller.prototype.replacedInstallationDirectory = "";

// Set when that removal fails. The run is stopped, and the finished page has
// to report that reason rather than the cancellation it looks like.
Controller.prototype.replacementFailed = false;

// Every live installation found when setup cannot choose one safely.
Controller.prototype.ambiguousInstallationDirectories = [];

// Introduction can be entered more than once while setup is closing.
Controller.prototype.reportedAmbiguousInstallations = false;

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

// An offline installer has no update mode, and IFW refuses a target directory
// that already holds one of its installations. That refusal belongs to the
// installation folder page, so an upgrade can only stay inside a single run if
// the folder stops being a question: the installed copy becomes the target,
// the wizard says so, and the page that would refuse it is not shown.
Controller.prototype.adoptExistingInstallation = function()
{
    Controller.prototype.ambiguousInstallationDirectories = [];
    Controller.prototype.reportedAmbiguousInstallations = false;

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

    var targetDirectory = directories[0];
    if (targetDirectory != installer.value("TargetDir"))
        installer.setValue("TargetDir", targetDirectory);

    Controller.prototype.replacedInstallationDirectory = targetDirectory;
    installer.setDefaultPageVisible(QInstaller.TargetDirectory, false);
    installer.installationStarted.connect(
        Controller.prototype.replaceExistingInstallation);
}

// The replaced installation's own maintenance tool owns the removal: it undoes
// the recorded operations, drops the launcher symlink and the desktop entry,
// and deletes the installation directory. Extracting over the files instead
// would leave behind every file the previous version owned and this one does
// not.
//
// installationStarted is the last moment that is still ahead of every file the
// framework writes and already past the decision to install: the framework
// emits it before it creates the target directory, and only reaches it once
// the summary page, which states this removal, has been accepted.
Controller.prototype.replaceExistingInstallation = function()
{
    var targetDirectory = Controller.prototype.replacedInstallationDirectory;
    var maintenanceToolPath =
        targetDirectory + "/" + Controller.prototype.maintenanceToolFileName;
    if (Controller.prototype.removeInstallation(
            maintenanceToolPath, targetDirectory))
    {
        return;
    }

    Controller.prototype.replacementFailed = true;

    // The framework owns the target directory once a run has started, and
    // clears the installation record out of it when the run is abandoned.
    // Point it at a directory of this run's own before abandoning it, so that
    // the cleanup cannot alter whatever files or records the failed purge left
    // behind.
    installer.setValue(
        "TargetDir",
        installer.value("HomeDir") + "/vnm_terminal_setup_stopped");
    QMessageBox.critical(
        "ExistingInstallationRemovalFailed",
        "Error",
        "Setup could not complete removal of the installation in "
        + targetDirectory
        + ", and stopped without installing this version.\n\nThe previous "
        + "installation may now be incomplete. Start setup with "
        + "the privileges that directory needs, or remove the installation "
        + "with " + maintenanceToolPath + " first.");
    installer.setCanceled();
}

Controller.prototype.removeInstallation = function(
    maintenanceToolPath, targetDirectory)
{
    var result = installer.execute(
        maintenanceToolPath,
        ["purge", "--accept-messages", "--confirm-command"]);
    if (result.length != 2 || result[1] != 0)
        return false;

    // A running executable can be unlinked here, so the maintenance tool
    // deletes itself and then the installation directory before its own
    // process exits: the directory is a settled result by the time the purge
    // returns, and needs no wait of its own.
    return !installer.fileExists(targetDirectory);
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

    if (Controller.prototype.replacementFailed) {
        finishedPage.title = "Installation stopped";
        finishedPage.subTitle = "Setup did not install this version.";
        heading = "This version was not installed.";
        detail = "Setup stopped because removal of the installation "
            + "in " + Controller.prototype.escapeHtml(
                Controller.prototype.replacedInstallationDirectory)
            + " did not complete. The previous installation may now be "
            + "incomplete.";
    }
    else
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
