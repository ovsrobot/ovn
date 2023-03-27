# Tries to guess the latest stable branch in a git repo and checks it out.
# Assumes the CWD is inside a clone of the repo.  It also assumes stable
# branch names follow the "branch-x.y" convention.
function checkout_latest_stable_branch()
{
    local branch=$(git branch -a -l '*branch-*' | \
        sed 's/remotes\/origin\///' | sort -V | tail -1)
    git checkout $branch
}
