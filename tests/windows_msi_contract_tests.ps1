[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MsiPath
)

$ErrorActionPreference = 'Stop'

function Assert-MsiContract {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "MSI contract violation: $Message"
    }
}

function Get-MsiRows {
    param(
        [Parameter(Mandatory = $true)]
        $Database,

        [Parameter(Mandatory = $true)]
        [string]$Table,

        [Parameter(Mandatory = $true)]
        [string[]]$Columns
    )

    $selection = ($Columns | ForEach-Object { "``$_``" }) -join ', '
    $view = $Database.OpenView("SELECT $selection FROM ``$Table``")
    $rows = @()

    try {
        [void]$view.Execute()
        while ($true) {
            $record = $view.Fetch()
            if ($null -eq $record) {
                break
            }

            try {
                $row = [ordered]@{}
                for ($columnIndex = 0;
                    $columnIndex -lt $Columns.Count;
                    ++$columnIndex) {
                    $row[$Columns[$columnIndex]] =
                        $record.StringData($columnIndex + 1)
                }
                $rows += [pscustomobject]$row
            }
            finally {
                [void][Runtime.InteropServices.Marshal]::ReleaseComObject(
                    $record)
            }
        }
    }
    finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
    }

    return $rows
}

function Get-OnlyMsiRow {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Rows,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Predicate,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $matches = @($Rows | Where-Object $Predicate)
    Assert-MsiContract ($matches.Count -eq 1) `
        "expected exactly one $Description row, found $($matches.Count)"
    return $matches[0]
}

$resolvedMsiPath = (Resolve-Path -LiteralPath $MsiPath).Path
$installer = $null
$database = $null

try {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase($resolvedMsiPath, 0)

    $properties = @(Get-MsiRows $database 'Property' @(
        'Property', 'Value'))
    $propertyValues = @{}
    foreach ($property in $properties) {
        $propertyValues[$property.Property] = $property.Value
    }

    Assert-MsiContract ($propertyValues['ALLUSERS'] -eq '1') `
        'the package must install per-machine'
    Assert-MsiContract (
        $propertyValues['WIXUI_EXITDIALOGOPTIONALCHECKBOX'] -eq '1') `
        'launch-after-install must be selected by default'
    Assert-MsiContract (
        $propertyValues['WIXUI_EXITDIALOGOPTIONALCHECKBOXTEXT'] -eq
            'Launch vnm_terminal') `
        'the completion-page launch option has the wrong label'
    Assert-MsiContract (
        $propertyValues['WixShellExecTarget'] -eq
            '[#CM_FP_bin.vnm_terminal.exe]') `
        'launch-after-install must target the installed executable'
    Assert-MsiContract (
        -not $propertyValues.ContainsKey('INSTALLLEVEL') -or
        $propertyValues['INSTALLLEVEL'] -eq '1') `
        'the feature install level must preserve the authored defaults'

    $features = @(Get-MsiRows $database 'Feature' @(
        'Feature', 'Feature_Parent', 'Level', 'Display', 'Attributes'))
    $expectedFeatures = @{
        'ProductFeature' = @('', '1')
        'VnmTerminalStartMenuFeature' = @('ProductFeature', '1')
        'VnmTerminalSystemPathFeature' = @('ProductFeature', '2')
        'VnmTerminalDesktopFeature' = @('ProductFeature', '2')
    }
    Assert-MsiContract ($features.Count -eq $expectedFeatures.Count) `
        "expected $($expectedFeatures.Count) features, found $($features.Count)"
    foreach ($featureId in $expectedFeatures.Keys) {
        $feature = Get-OnlyMsiRow $features {
            $_.Feature -eq $featureId
        } "feature '$featureId'"
        Assert-MsiContract (
            $feature.Feature_Parent -eq $expectedFeatures[$featureId][0]) `
            "feature '$featureId' has the wrong parent"
        Assert-MsiContract (
            $feature.Level -eq $expectedFeatures[$featureId][1]) `
            "feature '$featureId' has the wrong default level"
        if ($featureId -eq 'ProductFeature') {
            Assert-MsiContract (
                ([int]$feature.Attributes -band 16) -ne 0) `
                'the runtime feature must remain mandatory'
        }
        else {
            Assert-MsiContract ([int]$feature.Display -gt 0) `
                "integration feature '$featureId' must remain visible"
            Assert-MsiContract (
                ([int]$feature.Attributes -band 16) -eq 0) `
                "integration feature '$featureId' must remain selectable"
            Assert-MsiContract (
                ([int]$feature.Attributes -band 8) -ne 0) `
                "integration feature '$featureId' must not be advertised"
        }
    }

    $featureComponents = @(Get-MsiRows $database 'FeatureComponents' @(
        'Feature_', 'Component_'))
    $expectedFeatureComponents = @{
        'VnmTerminalStartMenuFeature' = 'VnmTerminalStartMenuShortcuts'
        'VnmTerminalSystemPathFeature' = 'VnmTerminalSystemPath'
        'VnmTerminalDesktopFeature' = 'VnmTerminalDesktopShortcut'
    }
    $integrationFeatureComponents = @($featureComponents | Where-Object {
        $expectedFeatureComponents.Values -contains $_.Component_
    })
    Assert-MsiContract (
        $integrationFeatureComponents.Count -eq
            $expectedFeatureComponents.Count) `
        'each optional integration must own exactly one component'
    foreach ($featureId in $expectedFeatureComponents.Keys) {
        $featureComponent = Get-OnlyMsiRow $integrationFeatureComponents {
            $_.Feature_ -eq $featureId
        } "feature-component '$featureId'"
        Assert-MsiContract (
            $featureComponent.Component_ -eq
                $expectedFeatureComponents[$featureId]) `
            "feature '$featureId' owns the wrong component"
    }

    $components = @(Get-MsiRows $database 'Component' @(
        'Component', 'ComponentId', 'Directory_', 'Attributes', 'KeyPath'))
    $expectedComponents = @{
        'VnmTerminalStartMenuShortcuts' = @(
            '{D53177D9-0752-46DC-87FC-065711185E1A}',
            'VnmTerminalProgramMenuFolder')
        'VnmTerminalSystemPath' = @(
            '{3DA2C1E4-CC06-4BD0-8B9E-219A7AD40D7A}', 'CM_DP_bin')
        'VnmTerminalDesktopShortcut' = @(
            '{855835E3-3467-4C8E-8A1C-8AD2BC1D162A}', 'DesktopFolder')
    }
    foreach ($componentId in $expectedComponents.Keys) {
        $component = Get-OnlyMsiRow $components {
            $_.Component -eq $componentId
        } "component '$componentId'"
        Assert-MsiContract (
            $component.ComponentId -eq $expectedComponents[$componentId][0]) `
            "component '$componentId' changed its permanent GUID"
        Assert-MsiContract (
            $component.Directory_ -eq $expectedComponents[$componentId][1]) `
            "component '$componentId' has the wrong directory"
        Assert-MsiContract (([int]$component.Attributes -band 256) -ne 0) `
            "component '$componentId' must remain 64-bit"
    }

    $directories = @(Get-MsiRows $database 'Directory' @(
        'Directory', 'Directory_Parent', 'DefaultDir'))
    $installRoot = Get-OnlyMsiRow $directories {
        $_.Directory -eq 'INSTALL_ROOT'
    } 'installation root directory'
    Assert-MsiContract (
        $installRoot.Directory_Parent -eq 'ProgramFiles64Folder' -and
        ($installRoot.DefaultDir -split '\|')[-1] -eq 'vnm_terminal') `
        'the default installation root must remain under 64-bit Program Files'
    $programMenuDirectory = Get-OnlyMsiRow $directories {
        $_.Directory -eq 'VnmTerminalProgramMenuFolder'
    } 'Start Menu directory'
    Assert-MsiContract (
        $programMenuDirectory.Directory_Parent -eq 'ProgramMenuFolder' -and
        ($programMenuDirectory.DefaultDir -split '\|')[-1] -eq
            'vnm_terminal') `
        'the Start Menu feature must own the vnm_terminal program group'
    $desktopDirectory = Get-OnlyMsiRow $directories {
        $_.Directory -eq 'DesktopFolder'
    } 'desktop directory'
    Assert-MsiContract ($desktopDirectory.Directory_Parent -eq 'TARGETDIR') `
        'the desktop shortcut must use the standard desktop directory'

    $shortcuts = @(Get-MsiRows $database 'Shortcut' @(
        'Shortcut', 'Directory_', 'Component_', 'Target', 'Arguments'))
    $expectedShortcuts = @{
        'VnmTerminalStartMenuShortcut' = @(
            'VnmTerminalProgramMenuFolder',
            'VnmTerminalStartMenuShortcuts',
            '[CM_DP_bin]vnm_terminal.exe', '')
        'VnmTerminalUninstallShortcut' = @(
            'VnmTerminalProgramMenuFolder',
            'VnmTerminalStartMenuShortcuts',
            '[SystemFolder]msiexec.exe', '/x [ProductCode]')
        'VnmTerminalDesktopShortcutLink' = @(
            'DesktopFolder',
            'VnmTerminalDesktopShortcut',
            '[CM_DP_bin]vnm_terminal.exe', '')
    }
    Assert-MsiContract ($shortcuts.Count -eq $expectedShortcuts.Count) `
        "expected $($expectedShortcuts.Count) shortcuts, found $($shortcuts.Count)"
    foreach ($shortcutId in $expectedShortcuts.Keys) {
        $shortcut = Get-OnlyMsiRow $shortcuts {
            $_.Shortcut -eq $shortcutId
        } "shortcut '$shortcutId'"
        $expectedShortcut = $expectedShortcuts[$shortcutId]
        Assert-MsiContract (
            $shortcut.Directory_ -eq $expectedShortcut[0] -and
            $shortcut.Component_ -eq $expectedShortcut[1] -and
            $shortcut.Target -eq $expectedShortcut[2] -and
            $shortcut.Arguments -eq $expectedShortcut[3]) `
            "shortcut '$shortcutId' has the wrong ownership or target"
    }

    $environmentRows = @(Get-MsiRows $database 'Environment' @(
        'Environment', 'Name', 'Value', 'Component_'))
    Assert-MsiContract ($environmentRows.Count -eq 1) `
        "expected one environment update, found $($environmentRows.Count)"
    $pathEntry = Get-OnlyMsiRow $environmentRows {
        $_.Environment -eq 'VnmTerminalSystemPathEntry'
    } 'system PATH environment'
    Assert-MsiContract (
        $pathEntry.Name -eq '=-*PATH' -and
        $pathEntry.Value -eq '[~];[CM_DP_bin]' -and
        $pathEntry.Component_ -eq 'VnmTerminalSystemPath') `
        'the system PATH entry must append safely and be removed on uninstall'

    $registryRows = @(Get-MsiRows $database 'Registry' @(
        'Registry', 'Root', 'Key', 'Name', 'Component_'))
    $expectedRegistryRoots = @{
        'VnmTerminalStartMenuShortcuts' = '1'
        'VnmTerminalDesktopShortcut' = '1'
        'VnmTerminalSystemPath' = '2'
    }
    foreach ($componentId in $expectedRegistryRoots.Keys) {
        $marker = Get-OnlyMsiRow $registryRows {
            $_.Component_ -eq $componentId -and $_.Name -eq 'Installed'
        } "registry marker for '$componentId'"
        Assert-MsiContract (
            $marker.Root -eq $expectedRegistryRoots[$componentId]) `
            "registry marker for '$componentId' has the wrong hive"
        $component = Get-OnlyMsiRow $components {
            $_.Component -eq $componentId
        } "component '$componentId'"
        Assert-MsiContract ($component.KeyPath -eq $marker.Registry) `
            "registry marker for '$componentId' must remain its key path"
    }

    $customActions = @(Get-MsiRows $database 'CustomAction' @(
        'Action', 'Type', 'Source', 'Target'))
    $launchAction = Get-OnlyMsiRow $customActions {
        $_.Action -eq 'VnmTerminalLaunchAfterInstall'
    } 'launch custom action'
    Assert-MsiContract (
        $launchAction.Type -eq '65' -and
        $launchAction.Source -eq 'WixCA' -and
        $launchAction.Target -eq 'WixShellExec') `
        'launch-after-install must use the immediate impersonated shell action'

    $setExpectedRootAction = Get-OnlyMsiRow $customActions {
        $_.Action -eq 'VnmTerminalSetExpectedInstallRoot'
    } 'expected-install-root custom action'
    Assert-MsiContract (
        $setExpectedRootAction.Type -eq '51' -and
        $setExpectedRootAction.Source -eq
            'VNM_TERMINAL_EXPECTED_INSTALL_ROOT') `
        'the PATH safety guard must set its expected-root property'
    $installRootName = ($installRoot.DefaultDir -split '\|')[-1]
    $expectedInstallRoot =
        "[$($installRoot.Directory_Parent)]$installRootName\"
    Assert-MsiContract (
        $setExpectedRootAction.Target -eq $expectedInstallRoot) `
        'the PATH safety guard must compare against the protected install root'

    $rejectUnsafePathAction = Get-OnlyMsiRow $customActions {
        $_.Action -eq 'VnmTerminalRejectUnsafePathLocation'
    } 'unsafe-PATH rejection custom action'
    Assert-MsiContract ($rejectUnsafePathAction.Type -eq '19') `
        'an unsafe PATH destination must terminate installation'

    $executeSequence = @(Get-MsiRows $database 'InstallExecuteSequence' @(
        'Action', 'Condition', 'Sequence'))
    $migration = Get-OnlyMsiRow $executeSequence {
        $_.Action -eq 'MigrateFeatureStates'
    } 'feature-state migration sequence'
    $setExpectedRoot = Get-OnlyMsiRow $executeSequence {
        $_.Action -eq 'VnmTerminalSetExpectedInstallRoot'
    } 'expected-install-root sequence'
    $rejectUnsafePath = Get-OnlyMsiRow $executeSequence {
        $_.Action -eq 'VnmTerminalRejectUnsafePathLocation'
    } 'unsafe-PATH rejection sequence'
    $installValidate = Get-OnlyMsiRow $executeSequence {
        $_.Action -eq 'InstallValidate'
    } 'InstallValidate sequence'
    Assert-MsiContract (
        [int]$migration.Sequence -lt [int]$setExpectedRoot.Sequence -and
        [int]$setExpectedRoot.Sequence -lt [int]$rejectUnsafePath.Sequence -and
        [int]$rejectUnsafePath.Sequence -lt [int]$installValidate.Sequence) `
        'the PATH guard must run after feature migration and before validation'
    Assert-MsiContract (
        $rejectUnsafePath.Condition -match
            '&VnmTerminalSystemPathFeature\s*=\s*3' -and
        $rejectUnsafePath.Condition -match
            'INSTALL_ROOT\s*~<>\s*VNM_TERMINAL_EXPECTED_INSTALL_ROOT') `
        'the PATH guard must cover requested and migrated feature state'

    $controlEvents = @(Get-MsiRows $database 'ControlEvent' @(
        'Dialog_', 'Control_', 'Event', 'Argument', 'Condition', 'Ordering'))
    $launchEvent = Get-OnlyMsiRow $controlEvents {
        $_.Dialog_ -eq 'ExitDialog' -and
        $_.Control_ -eq 'Finish' -and
        $_.Event -eq 'DoAction' -and
        $_.Argument -eq 'VnmTerminalLaunchAfterInstall'
    } 'completion-page launch event'
    Assert-MsiContract (
        $launchEvent.Condition -eq
            'WIXUI_EXITDIALOGOPTIONALCHECKBOX = 1 AND NOT Installed' -and
        $launchEvent.Ordering -eq '1') `
        'completion-page launch must be opt-out and run before closing'
    $closeEvent = Get-OnlyMsiRow $controlEvents {
        $_.Dialog_ -eq 'ExitDialog' -and
        $_.Control_ -eq 'Finish' -and
        $_.Event -eq 'EndDialog' -and
        $_.Argument -eq 'Return'
    } 'completion-page close event'
    Assert-MsiContract (
        [int]$launchEvent.Ordering -lt [int]$closeEvent.Ordering) `
        'launch-after-install must run before the completion dialog closes'
    $quietLaunchActions = @($executeSequence | Where-Object {
        $_.Action -eq 'VnmTerminalLaunchAfterInstall'
    })
    Assert-MsiContract ($quietLaunchActions.Count -eq 0) `
        'launch-after-install must not run during silent installation'

    $controls = @(Get-MsiRows $database 'Control' @(
        'Dialog_', 'Control', 'Type', 'Property'))
    $selectionTree = @($controls | Where-Object {
        $_.Dialog_ -eq 'CustomizeDlg' -and $_.Type -eq 'SelectionTree'
    })
    Assert-MsiContract ($selectionTree.Count -eq 1) `
        'the installer UI must expose the optional MSI features'
    $launchCheckBox = Get-OnlyMsiRow $controls {
        $_.Dialog_ -eq 'ExitDialog' -and
        $_.Control -eq 'OptionalCheckBox'
    } 'completion-page launch check box'
    Assert-MsiContract (
        $launchCheckBox.Type -eq 'CheckBox' -and
        $launchCheckBox.Property -eq
            'WIXUI_EXITDIALOGOPTIONALCHECKBOX') `
        'launch-after-install must remain an interactive opt-out control'

    $controlConditions = @(Get-MsiRows $database 'ControlCondition' @(
        'Dialog_', 'Control_', 'Action', 'Condition'))
    $launchCheckBoxShow = Get-OnlyMsiRow $controlConditions {
        $_.Dialog_ -eq 'ExitDialog' -and
        $_.Control_ -eq 'OptionalCheckBox' -and
        $_.Action -eq 'Show'
    } 'completion-page launch check box visibility'
    Assert-MsiContract (
        $launchCheckBoxShow.Condition -eq
            'WIXUI_EXITDIALOGOPTIONALCHECKBOXTEXT AND NOT Installed') `
        'the launch check box must be visible after successful installation'

    $uiSequence = @(Get-MsiRows $database 'InstallUISequence' @(
        'Action', 'Condition', 'Sequence'))
    [void](Get-OnlyMsiRow $uiSequence {
        $_.Action -eq 'MigrateFeatureStates'
    } 'interactive feature-state migration sequence')

    $upgradeRows = @(Get-MsiRows $database 'Upgrade' @(
        'UpgradeCode', 'Attributes', 'ActionProperty'))
    $migratingUpgradeRows = @($upgradeRows | Where-Object {
        $_.UpgradeCode -eq '{F2D514D2-2D09-4DDB-A857-B27F65DD8BC0}' -and
        (([int]$_.Attributes -band 1) -ne 0)
    })
    Assert-MsiContract ($migratingUpgradeRows.Count -ge 1) `
        'major upgrades must migrate the selected integration features'
}
finally {
    if ($null -ne $database) {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database)
    }
    if ($null -ne $installer) {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    }
}

Write-Host "Windows MSI contract passed: $resolvedMsiPath"
