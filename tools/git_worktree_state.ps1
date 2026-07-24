# Shared release-script worktree check. `git status --porcelain` can report a
# tracked text file as modified after a tool rewrites only its working-tree line
# endings, even though Git's normalized content is byte-identical to the index.
# Use Git's actual diff machinery and add untracked, non-ignored paths explicitly.
function Get-GitWorktreeState {
    [CmdletBinding()]
    param(
        [string]$Repository = '.'
    )

    Push-Location $Repository
    try {
        $unstaged = @(& git diff --name-only --ignore-submodules=none --)
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect unstaged Git changes" }

        $staged = @(& git diff --cached --name-only --ignore-submodules=none --)
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect staged Git changes" }

        $untracked = @(& git ls-files --others --exclude-standard)
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect untracked Git files" }

        $paths = @($unstaged + $staged + $untracked |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Sort-Object -Unique)
        [pscustomobject]@{
            Dirty = $paths.Count -gt 0
            Paths = $paths
            Status = $paths -join "`n"
        }
    } finally {
        Pop-Location
    }
}
