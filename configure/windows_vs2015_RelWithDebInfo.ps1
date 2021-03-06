#REQUIRES -Version 2.0
<#
.SYNOPSIS
    Configures and builds ArangoDB
.EXAMPLE
    mkdir arango-build; cd arangod-build; ../arangodb/scripts/configure/<this_file> [-build] [cmake params]
#>
param([switch] $build)
if ($build) { $do_build = $TRUE } else { $do_build = $FALSE }
$Params = $Args

$config = "RelWithDebInfo"

$vc_version = "14.0"
$generator = "Visual Studio 14 2015 Win64"

$script_path = split-path -parent $myinvocation.mycommand.definition
. "$script_path/windows_common.ps1"
