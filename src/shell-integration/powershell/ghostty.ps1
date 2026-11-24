# Ghostty shell integration for PowerShell (pwsh 7 and Windows PowerShell 5.1).
#
# Ghostty for Windows loads it by itself when the shell is pwsh or powershell
# (after your profile, with -NoExit -EncodedCommand, so the execution policy
# does not apply). To load it by hand, e.g. from your profile:
#
#   . "$env:GHOSTTY_RESOURCES_DIR\shell-integration\powershell\ghostty.ps1"
#
# It never changes your execution policy or profile files, and a failure in
# it falls back to your own prompt. It wraps your prompt function, whatever
# made it (your profile, oh-my-posh, starship), adding:
#   - OSC 133 marks around the prompt: jump between prompts
#     (jump_to_prompt), select a command's output; redraw=0, so a resize
#     keeps the prompt and the command being typed;
#   - OSC 7 with the current folder: new tabs and splits start there (also
#     in \\wsl.localhost\<distro> folders).

if ($global:__GhosttyIntegrated) { return }
$global:__GhosttyIntegrated = $true
$global:__GhosttyPrompt = $function:prompt

function global:prompt {
    # The previous command's result, read before anything else changes it.
    $ok = $?
    # Your own prompt first, as it would be without Ghostty.
    # (try is a statement in PowerShell: $( ) makes it a value, in 5.1 too.)
    $orig = $(try {
        if ($global:__GhosttyPrompt) { (& $global:__GhosttyPrompt) -join '' }
        else { "PS $($ExecutionContext.SessionState.Path.CurrentLocation)$('>' * ($NestedPromptLevel + 1)) " }
    } catch { 'PS> ' })
    # Ghostty's marks around it; any failure here leaves your prompt as is.
    try {
        $code = if ($ok) { 0 } elseif ($global:LASTEXITCODE) { $global:LASTEXITCODE } else { 1 }
        $e = [char]27
        $s = ''
        if ($global:__GhosttyRan) { $s += "$e]133;D;$code$e\" }
        $global:__GhosttyRan = $true
        # redraw=0: PowerShell does not redraw its prompt on a resize (Windows
        # has no SIGWINCH), so Ghostty must not clear it (a split, a zoom).
        $s += "$e]133;A;redraw=0$e\"
        $loc = $ExecutionContext.SessionState.Path.CurrentLocation
        if ($loc.Provider.Name -eq 'FileSystem') {
            # kitty-shell-cwd takes the path as it is: C:\dir, \\server\share.
            $p = $loc.ProviderPath
            if (-not $p.StartsWith('/')) { $p = '/' + $p }
            $s += "$e]7;kitty-shell-cwd://localhost$p$e\"
        }
        $s + $orig + "$e]133;B$e\"
    } catch { $orig }
}
