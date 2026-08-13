function Component()
{
}

Component.prototype.createOperations = function()
{
    component.createOperations();
    component.addElevatedOperation(
        "CreateShortcut",
        "@TargetDir@/vnm_terminal.exe",
        "@AllUsersStartMenuProgramsPath@/@StartMenuDir@/vnm_terminal.lnk");
}
