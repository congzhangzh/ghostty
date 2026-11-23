# Ghostty shell integration for PowerShell (pwsh 7 and Windows PowerShell 5.1).
#
# Ghostty for Windows loads it by itself when the shell is pwsh or powershell
# (after your profile, with -NoExit -EncodedCommand, so the execution policy
# does not apply). To load it by hand, e.g. from your profile:
#
#   . "$env:GHOSTTY_RESOURCES_DIR\shell-integration\powershell\ghostty.ps1"
#
# It wraps your prompt function, whatever made it (your profile, oh-my-posh,
# starship), adding:
#   - OSC 133 marks around the prompt: jump between prompts
#     (jump_to_prompt), select a command's output;
#   - OSC 7 with the current folder: new tabs and splits start there (also
#     in \\wsl.localhost\<distro> folders).

if ($global:__GhosttyIntegrated) { return }
$global:__GhosttyIntegrated = $true
$global:__GhosttyPrompt = $function:prompt

function global:prompt {
    # The previous command's result, read before anything else changes it.
    $ok = $?
    $code = if ($ok) { 0 } elseif ($global:LASTEXITCODE) { $global:LASTEXITCODE } else { 1 }
    $e = [char]27
    $s = ''
    if ($global:__GhosttyRan) { $s += "$e]133;D;$code$e\" }
    $global:__GhosttyRan = $true
    $s += "$e]133;A$e\"
    $loc = $ExecutionContext.SessionState.Path.CurrentLocation
    if ($loc.Provider.Name -eq 'FileSystem') {
        # kitty-shell-cwd takes the path as it is: C:\dir, \\server\share.
        $p = $loc.ProviderPath
        if (-not $p.StartsWith('/')) { $p = '/' + $p }
        $s += "$e]7;kitty-shell-cwd://localhost$p$e\"
    }
    $orig = if ($global:__GhosttyPrompt) { & $global:__GhosttyPrompt } else { "PS $loc$('>' * ($NestedPromptLevel + 1)) " }
    $s + ($orig -join '') + "$e]133;B$e\"
}
