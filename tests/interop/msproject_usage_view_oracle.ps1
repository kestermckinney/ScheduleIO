# Copyright (C) 2026 Paul McKinney
# SPDX-License-Identifier: GPL-3.0-only

param(
    [Parameter(Mandatory = $true)] [string] $InputPath,
    [Parameter(Mandatory = $true)] [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
$project = $null

function Assert-Equal([string] $Label, $Actual, $Expected) {
    if ($Actual -ne $Expected) {
        throw "${Label}: expected $Expected, Microsoft Project reported $Actual"
    }
}

try {
    $project = New-Object -ComObject MSProject.Application
    $project.Visible = $false
    $project.DisplayAlerts = $false
    $project.FileOpenEx((Resolve-Path -LiteralPath $InputPath).Path)

    $project.ViewApply('Resource Usage')
    $resourceTable = $project.ActiveProject.Views.Item('Resource Usage').Table
    Assert-Equal 'Resource Usage table' ($resourceTable.Name -replace '&', '') 'Usage'
    Assert-Equal 'Resource Name width' $resourceTable.TableFields.Item(3).Width 31
    Assert-Equal 'Resource Work width' $resourceTable.TableFields.Item(4).Width 17

    $project.ViewApply('Task Usage')
    $taskTable = $project.ActiveProject.Views.Item('Task Usage').Table
    Assert-Equal 'Task Usage table' ($taskTable.Name -replace '&', '') 'Usage'
    Assert-Equal 'Task Name width' $taskTable.TableFields.Item(4).Width 29
    Assert-Equal 'Task Work width' $taskTable.TableFields.Item(5).Width 15

    $project.FileSaveAs($OutputPath)
    $project.FileCloseEx(1)
    $project.Quit()
    $project = $null
    Write-Output 'Microsoft Project opened, inspected, and natively resaved the usage-view MPP.'
}
finally {
    if ($null -ne $project) {
        try { $project.FileCloseEx(1) } catch {}
        try { $project.Quit() } catch {}
    }
}
