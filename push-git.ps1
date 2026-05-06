param(
 [Parameter(Position = 0)]
 [string]$Message = "Update project"
)

$ErrorActionPreference = "Stop"

try {
 $repoRoot = git rev-parse --show-toplevel 2>$null
 if (-not $repoRoot) {
 throw "Current folder is not inside a git repository."
 }

 Set-Location $repoRoot.Trim()

 $remoteName = "origin"
 $remoteUrl = git remote get-url $remoteName 2>$null
 if (-not $remoteUrl) {
 throw "Git remote '$remoteName' is not configured."
 }

 $branchName = git rev-parse --abbrev-ref HEAD 2>$null
 if (-not $branchName -or $branchName -eq "HEAD") {
 throw "Cannot determine the current branch. Please checkout a branch first."
 }

 $statusLines = @(git status --porcelain)
 if ($statusLines.Count -eq 0) {
 Write-Host "No changes to commit. Working tree is clean." -ForegroundColor Yellow
 exit 0
 }

 Write-Host "Repository : $repoRoot" -ForegroundColor Cyan
 Write-Host "Remote  : $remoteUrl" -ForegroundColor Cyan
 Write-Host "Branch  : $branchName" -ForegroundColor Cyan
 Write-Host "Message : $Message" -ForegroundColor Cyan

 git add -A
 git commit -m $Message
 git push $remoteName $branchName

 Write-Host "Push completed successfully." -ForegroundColor Green
}
catch {
 Write-Error $_.Exception.Message
 exit 1
}
