function Controller()
{
    this.requireStoppedSessions();
    installer.aboutCalculateComponentsToInstall.connect(
        this, Controller.prototype.requireStoppedSessions);
    installer.aboutCalculateComponentsToUninstall.connect(
        this, Controller.prototype.requireStoppedSessions);
    // IFW loads the control script before it presents the wizard. Component
    // scripts load later, after the initial page list has already been shown.
    if (installer.isInstaller()) {
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
        Controller.prototype.configureUserLogFile();
        Controller.prototype.promptedExistingInstallationDirectory = "";
        Controller.prototype.replacementStopped = false;
        Controller.prototype.removedInstallationDirectory = "";
    }
}

Controller.prototype.configureUserLogFile = function()
{
    // This absolute device path safely disables file output if the exact-file
    // path probe cannot run or cannot prove a candidate. It can never be
    // resolved relative to TargetDir.
    installer.setValue("LogFileName", "\\\\.\\NUL");

    var powershellPath = Controller.prototype.windowsPowerShellPath();
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
    if (!/^(?:[A-Za-z]:[\\/]|\\\\)/.test(logFileName))
        return;

    // The helper already created this unique file exclusively and closed it.
    // IFW later reopens the same file with ReadWrite | Append | Text.
    // A policy or ACL change after this proof remains an unavoidable race for
    // any external logger; retaining the unique file removes name collisions.
    installer.setValue(
        "LogFileName", installer.toNativeSeparators(logFileName));
}

Controller.prototype.requireStoppedSessions = function()
{
    if (installer.isOfflineGenerator() || installer.isPackageViewer())
        return;

    var target = installer.value("TargetDir").replace(/\\/g, "/").replace(/\/+$/, "");
    var processes = [
        target + "/vnm_terminal.exe",
        target + "/vnm_terminal_runtime/vnm_terminal.exe",
        target + "/vnm_terminal_runtime/x64/OpenConsole.exe",
        target + "/vnm_terminal_runtime/arm64/OpenConsole.exe"
    ];
    for (var index = 0; index < processes.length; ++index) {
        if (installer.isProcessRunning(processes[index])) {
            var message = "vnm_terminal setup cannot continue while vnm_terminal or its terminal sessions "
                + "in this installation folder are running. All sessions in this installation "
                + "must be closed before setup can continue. Setup has not stopped any process.";
            if (!installer.isCommandLineInstance()) {
                QMessageBox.critical("vnm_terminal.runningSessions", "vnm_terminal setup", message, QMessageBox.Ok);
            }
            installer.setCanceled();
            throw new Error(message);
        }
    }
}

Controller.prototype.windowsPowerShellPath = function()
{
    var rootDirectory = installer.toNativeSeparators(installer.value("RootDir"));
    return rootDirectory
        + "Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
}

// Must name the file config.xml declares through MaintenanceToolName. IFW
// refuses any target directory that contains it, which is how it recognizes
// one of its own installations.
Controller.prototype.maintenanceToolFileName = "vnm_terminal_maintenance.exe";

// The folder callback may be re-entered without the user changing TargetDir.
// Remember that exact answer, but let a changed path be evaluated afresh.
Controller.prototype.promptedExistingInstallationDirectory = "";

Controller.prototype.replacementStopped = false;
Controller.prototype.removedInstallationDirectory = "";

Controller.prototype.normalizedTargetDirectory = function()
{
    var targetDirectoryPage = gui.pageWidgetByObjectName("TargetDirectoryPage");
    var targetDirectory = installer.toNativeSeparators(
        targetDirectoryPage.TargetDirectoryLineEdit.text.trim());
    while (targetDirectory.length > 3 && /[\\\/]$/.test(targetDirectory))
        targetDirectory = targetDirectory.substring(0, targetDirectory.length - 1);
    return targetDirectory;
}

Controller.prototype.runReplacementHelper = function(targetDirectory, action)
{
    var powershellPath = Controller.prototype.windowsPowerShellPath();
    var supportScript = installer.readFile(
        ":/metadata/installer-theme/replacement_support.ps1", "UTF-8");
    if (!installer.fileExists(powershellPath) || supportScript == "") {
        return ["not-started\nSetup cannot run its replacement helper on this computer.", 2];
    }
    var invocation = "\n[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)\n"
        + "$script:VnmTerminalReplacementRemovalMayHaveStarted = $true\n"
        // The leading blank line terminates the library; the final statement
        // must stay on one line for Windows PowerShell's stdin/EOF parser.
        + "try { Invoke-VnmTerminalSetupReplacement -TargetDirectory '"
        + targetDirectory.replace(/'/g, "''") + "' -Action '" + action + "' } "
        + "catch { if (!$script:VnmTerminalReplacementRemovalMayHaveStarted) { "
        + "[Console]::Out.WriteLine('not-started'); [Console]::Out.WriteLine($_.Exception.Message); exit 2 }; "
        + "[Console]::Out.WriteLine($_.Exception.Message); exit 1 }\n";
    return installer.execute(
        powershellPath,
        ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", "-"],
        supportScript + invocation,
        "UTF-8",
        "UTF-8");
}

Controller.prototype.offerSelectedInstallationReplacement = function()
{
    if (Controller.prototype.replacementStopped) {
        return;
    }
    var targetDirectory = Controller.prototype.normalizedTargetDirectory();
    var maintenanceToolPath = targetDirectory + "\\"
        + Controller.prototype.maintenanceToolFileName;
    if (!installer.fileExists(maintenanceToolPath)) {
        Controller.prototype.promptedExistingInstallationDirectory = "";
        return;
    }
    if (targetDirectory.toLowerCase() ==
        Controller.prototype.promptedExistingInstallationDirectory.toLowerCase())
    {
        return;
    }

    // Permission is requested separately for the validated maintenance image.
    // A selected image must never inherit an elevated setup token.
    if (installer.hasAdminRights()) {
        QMessageBox.critical(
            "ElevatedExistingInstallationReplacement",
            "Start setup normally",
            "Close this setup and start it normally. The existing maintenance "
                + "tool needs a separate permission request. No removal was started.");
        return;
    }
    var validation = Controller.prototype.runReplacementHelper(targetDirectory, "Validate");
    if (validation.length != 2 || validation[1] != 0 || validation[0].trim() != "validated") {
        QMessageBox.critical(
            "ExistingInstallationValidationFailed",
            "Replacement could not be prepared",
            "No removal was started.\n\n"
                + (validation.length == 2 ? validation[0].replace(/^not-started\r?\n/, "")
                    : "The validation helper could not run.")
                + "\n\nCorrect the reported problem and return to this page to retry.");
        return;
    }
    Controller.prototype.promptedExistingInstallationDirectory = targetDirectory;
    var response = QMessageBox.question(
        "ExistingInstallationReplacement",
        "Replace the existing installation",
        "vnm_terminal is installed in:\n\n" + targetDirectory
            + "\n\nRemove it now and continue installation in this setup? "
            + "Close vnm_terminal and all surviving terminals first. "
            + "Removal ends the previous installation and cannot be undone. "
            + "Your user settings outside the installation folder are preserved. "
            + "If you cancel or installation fails afterward, the previous "
            + "application remains removed. Removal cannot be canceled after "
            + "you select Yes; you can cancel its permission request. "
            + "Setup waits while removal runs. "
            + "Select No to keep the existing installation.",
        QMessageBox.Yes | QMessageBox.No,
        QMessageBox.No);
    if (response != QMessageBox.Yes) {
        return;
    }
    var targetDirectoryPage = gui.pageWidgetByObjectName("TargetDirectoryPage");
    var removal = Controller.prototype.runReplacementHelper(targetDirectory, "Remove");
    if (removal.length == 2 && removal[1] == 2 && /^not-started\r?\n/.test(removal[0])) {
        Controller.prototype.promptedExistingInstallationDirectory = "";
        QMessageBox.critical(
            "ExistingInstallationRemovalNotStarted",
            "No removal was started",
            removal[0].replace(/^not-started\r?\n/, "")
                + "\n\nThe previous installation is unchanged. "
                + "Correct the reported problem and return to this page to retry.");
        return;
    }
    if (removal.length != 2 || removal[1] != 0 || removal[0].trim() != "removed") {
        Controller.prototype.replacementStopped = true;
        QMessageBox.critical(
            "ExistingInstallationRemovalFailed",
            "Replacement has not started",
            "Removal did not complete. The previous installation may have changed.\n\n"
                + (removal.length == 2 ? removal[0] : "The removal helper could not run.")
                + "\n\nUser settings outside the installation folder are preserved. "
                + "This setup will close. Wait for any removal still running to end, "
                + "then resolve the reported problem before starting setup again.");
        gui.rejectWithoutPrompt();
        return;
    }
    Controller.prototype.removedInstallationDirectory = targetDirectory;
    installer.setValue("TargetDir", targetDirectory);
    // A text change makes the native page re-evaluate the now-vacant folder.
    targetDirectoryPage.TargetDirectoryLineEdit.setText(targetDirectory + "\\");
    targetDirectoryPage.TargetDirectoryLineEdit.setText(targetDirectory);
    targetDirectoryPage.MessageLabel.setText(
        "The previous installation was removed. Continue with this setup. "
            + "Your user settings outside its folder are preserved.");
}

// IFW may re-enter page callbacks while a failed removal closes setup.
Controller.prototype.continueRequestedClose = function()
{
    if (!Controller.prototype.replacementStopped)
        return false;
    gui.rejectWithoutPrompt();
    return true;
}

Controller.prototype.IntroductionPageCallback = function()
{
    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
        return;

    var introductionPage = gui.pageWidgetByObjectName("IntroductionPage");
    introductionPage.title = "Welcome";
    introductionPage.subTitle = "vnm_terminal installation.";
    introductionPage.MessageLabel.setText(
        "<div style=\"color:#E0E0E0;\">"
        + "<span style=\"font-size:20px; font-weight:600;\">"
        + "vnm_terminal</span>"
        + "<br /><br /><span>vnm_terminal and its required runtime "
        + "will be installed on this computer.</span></div>");
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

    targetDirectoryPage.MessageLabel.setText(
        "Setup can remove a validated existing vnm_terminal installation and continue here. "
        + "Your settings outside the installation folder are preserved.");
    // Offer only on page entry; editing the path must not interrupt typing.
    Controller.prototype.offerSelectedInstallationReplacement();
}

Controller.prototype.ReadyForInstallationPageCallback = function()
{
    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
        return;

    var summaryPage = gui.pageWidgetByObjectName("ReadyForInstallationPage");
    summaryPage.subTitle = "Review your choices before installation.";
    if (Controller.prototype.removedInstallationDirectory != "") {
        summaryPage.subTitle =
            "The previous installation was removed. Canceling or a later failure "
                + "will not restore it.";
    }

    // IFW's component size excludes the maintenance-tool resources that its
    // labeled SpaceWidget includes. Hide the ambiguous, unlabeled tree value.
    var installComponentsTreeview =
        gui.findChild(summaryPage, "InstallComponentsTreeview");
    installComponentsTreeview.hideColumn(5);
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

Controller.prototype.StartMenuDirectoryPageCallback = function()
{
    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
        return;

    var startMenuDirectoryPage =
        gui.pageWidgetByObjectName("StartMenuDirectoryPage");
    startMenuDirectoryPage.subTitle =
        "Choose where Start Menu shortcuts will appear.";
}

Controller.prototype.PerformInstallationPageCallback = function()
{
    var performInstallationPage =
        gui.pageWidgetByObjectName("PerformInstallationPage");
    var detailsButton = gui.findChild(performInstallationPage, "DetailsButton");
    var detailsBrowser = gui.findChild(performInstallationPage, "DetailsBrowser");
    // Use IFW's toggle to keep the image panel and completion state in sync.
    if (!detailsBrowser.visible)
        detailsButton.click();
    detailsButton.hide();

    if (!installer.isInstaller())
        return;
    if (Controller.prototype.continueRequestedClose())
        return;

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

    if (Controller.prototype.removedInstallationDirectory != "") {
        detail += "<br /><br />The previous installation was removed from "
            + Controller.prototype.escapeHtml(Controller.prototype.removedInstallationDirectory)
            + ". User settings outside that folder were preserved.";
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
