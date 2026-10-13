function Component()
{
    // Declare the packaged base before IFW caches its disk-space estimate.
    // Its component already accounts for the bytes that become the uninstaller.
    if (installer.isInstaller()) {
        installer.setInstallerBaseBinary("@TargetDir@/installerbase.exe");
    }
    installer.installationStarted.connect(
        this,
        Component.prototype.onInstallationStarted);
}

Component.prototype.onInstallationStarted = function()
{
    if (component.updateRequested() || component.installationRequested()) {
        component.installerbaseBinaryPath = "@TargetDir@/installerbase.exe";
        installer.setInstallerBaseBinary(component.installerbaseBinaryPath);
    }
}
