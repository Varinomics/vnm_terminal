function Controller()
{
    // IFW loads the control script before it presents the wizard. Component
    // scripts load later, after the initial page list has already been shown.
    if (installer.isInstaller()) {
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
        installer.setDefaultPageVisible(QInstaller.ReadyForInstallation, false);
        Controller.prototype.configureUserLogFile();
        Controller.prototype.adoptExistingInstallation();
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

// The installation this run replaces, empty when this run replaces none. Read
// by the pages that have to say so.
Controller.prototype.replacedInstallationDirectory = "";

// Component scripts load after discovery, so the replacement validator reads
// the selected installation through this installer-owned process value.
Controller.prototype.replacementTargetValueName =
    "VnmReplacementTargetDirectory";

// Every live installation found when setup cannot choose one safely.
Controller.prototype.ambiguousInstallationDirectories = [];

// Introduction can be entered more than once while setup is closing.
Controller.prototype.reportedAmbiguousInstallations = false;

Controller.prototype.registeredInstallationDirectories = function()
{
    var powershellPath = Controller.prototype.windowsPowerShellPath();
    if (!installer.fileExists(powershellPath))
        return [];

    // Qt IFW owns these uninstall records and records the selected TargetDir
    // as InstallLocation, including when the user chose a custom directory.
    var command = "[Console]::OutputEncoding="
        + "[Text.UTF8Encoding]::new($false)"
        + ";$roots=@("
        + "'Registry::HKEY_CURRENT_USER\\Software\\Microsoft\\Windows"
        + "\\CurrentVersion\\Uninstall',"
        + "'Registry::HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows"
        + "\\CurrentVersion\\Uninstall')"
        + ";foreach($root in $roots){"
        + "if(-not(Test-Path -LiteralPath $root)){continue}"
        + ";foreach($key in (Get-ChildItem -LiteralPath $root "
        + "-ErrorAction SilentlyContinue)){"
        + "$item=Get-ItemProperty -LiteralPath $key.PSPath "
        + "-ErrorAction SilentlyContinue"
        + ";if($null -eq $item){continue}"
        + ";if($item.DisplayName -cne 'vnm_terminal'){continue}"
        + ";if($item.Publisher -cne 'Varinomics Ltd'){continue}"
        + ";$location=[string]$item.InstallLocation"
        + ";if([string]::IsNullOrWhiteSpace($location)){continue}"
        + ";Write-Output ('VNM_INSTALL:'+$location)}}";
    var result = installer.execute(
        powershellPath,
        ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command],
        "",
        "UTF-8",
        "UTF-8");
    if (result.length != 2 || result[1] != 0)
        return [];

    var directories = [];
    var lines = result[0].split(/\r?\n/);
    for (var i = 0; i < lines.length; ++i) {
        if (lines[i].indexOf("VNM_INSTALL:") != 0)
            continue;
        directories.push(lines[i].substring("VNM_INSTALL:".length).trim());
    }
    return directories;
}

Controller.prototype.appendExistingInstallationDirectory = function(
    directories, directory)
{
    var nativeDirectory = installer.toNativeSeparators(directory);
    while (nativeDirectory.length > 3 &&
        /[\\\\\/]$/.test(nativeDirectory))
    {
        nativeDirectory = nativeDirectory.substring(
            0, nativeDirectory.length - 1);
    }

    if (!/^(?:[A-Za-z]:[\\/]|\\\\)/.test(nativeDirectory) ||
        !installer.fileExists(nativeDirectory + "\\"
            + Controller.prototype.maintenanceToolFileName))
    {
        return;
    }

    for (var i = 0; i < directories.length; ++i) {
        if (directories[i].toLowerCase() == nativeDirectory.toLowerCase())
            return;
    }
    directories.push(nativeDirectory);
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
    installer.setValue(Controller.prototype.replacementTargetValueName, "");

    var directories = [];
    Controller.prototype.appendExistingInstallationDirectory(
        directories, installer.value("TargetDir"));

    var registeredDirectories =
        Controller.prototype.registeredInstallationDirectories();
    for (var i = 0; i < registeredDirectories.length; ++i) {
        Controller.prototype.appendExistingInstallationDirectory(
            directories, registeredDirectories[i]);
    }

    if (directories.length > 1) {
        Controller.prototype.ambiguousInstallationDirectories = directories;
        return;
    }
    if (directories.length == 0)
        return;

    var targetDirectory = directories[0];
    var configuredTargetDirectory =
        installer.toNativeSeparators(installer.value("TargetDir"));
    if (targetDirectory != configuredTargetDirectory)
        installer.setValue("TargetDir", targetDirectory);

    Controller.prototype.replacedInstallationDirectory = targetDirectory;
    installer.setValue(
        Controller.prototype.replacementTargetValueName, targetDirectory);
    installer.setDefaultPageVisible(QInstaller.TargetDirectory, false);
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
                "Setup found multiple vnm_terminal installations:\n\n- "
                + ambiguousDirectories.join("\n- ")
                + "\n\nRemove the unwanted copies, then run setup again.");
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
