function Component()
{
}

Component.prototype.createOperations = function()
{
    component.createOperations();
    component.addElevatedOperation(
        "Execute",
        "/bin/ln",
        "-s",
        "@TargetDir@/bin/vnm_terminal",
        "/usr/local/bin/vnm_terminal",
        "UNDOEXECUTE",
        "/usr/bin/unlink",
        "/usr/local/bin/vnm_terminal");
    component.addElevatedOperation(
        "CreateDesktopEntry",
        "/usr/local/share/applications/com.varinomics.vnm-terminal.desktop",
        "Type=Application\n"
            + "Name=vnm_terminal\n"
            + "GenericName=Terminal Emulator\n"
            + "Comment=Run command-line shells and applications\n"
            + "Exec=/usr/local/bin/vnm_terminal\n"
            + "Icon=@TargetDir@/share/icons/com.varinomics.vnm-terminal.png\n"
            + "Categories=System;TerminalEmulator;\n"
            + "Keywords=shell;prompt;command;commandline;\n"
            + "StartupNotify=true\n"
            + "Terminal=false");
}
