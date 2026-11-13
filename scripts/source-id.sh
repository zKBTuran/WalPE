#!/bin/sh
# source-id.sh SOURCE_DIR [HEADER]
# The source a build is made from, as one word: the first seven digits of the commit.
# (Seven everywhere: the length git abbreviates to differs with core.abbrev, with the
# number of objects of the repository and from forge to forge, and a test build's name
# carries seven.)
# Printed, or with HEADER written as `#define WALPE_SOURCE_ID "<word>"` - only when
# the file would change, so that a build of an unchanged tree compiles nothing.
#   - a tree made by `git archive` (a release tarball, a test build's zip): the file
#     SOURCE-ID has the commit in it, whole (.gitattributes: export-subst of %H, so the
#     archive of a commit is the same bytes wherever it is made); git is not asked.
#     A recipe that builds a copy of the files without .git writes the word of the tree
#     it copied there, "+" included (contrib/packaging/arch/PKGBUILD);
#   - a git checkout, where that file still has its placeholder: `git rev-parse
#     HEAD`, with a "+" behind it when tracked files differ from that commit.
#     Only where SOURCE_DIR itself has the .git: a copy of the files inside somebody
#     else's repository is not that repository's commit;
#   - neither: "unknown". A HEADER that has an id keeps it then (a build directory
#     whose step is run again by another user, for whom git refuses the repository).
# The id is a property of the source: no date, no host, nothing of the build.
src=${1:?usage: source-id.sh SOURCE_DIR [HEADER]}
out=${2:-}

id=
if [ -r "$src/SOURCE-ID" ]; then
    IFS= read -r id < "$src/SOURCE-ID" || true
    plus=
    case "$id" in *+) id=${id%+}; plus=+ ;; esac
    # a placeholder that was not filled in, or anything that is no commit (seven digits
    # or more: an archive made while the file held git's abbreviation has that many)
    case "$id" in '' | *[!0-9a-f]*) id= ;; esac
    [ "${#id}" -ge 7 ] && [ "${#id}" -le 64 ] || id=
    [ -z "$id" ] || id=$(printf '%.7s' "$id")$plus
fi
if [ -z "$id" ] && [ -e "$src/.git" ] && command -v git > /dev/null 2>&1; then
    # the repository is the one SOURCE_DIR has, not one the caller's environment names (a
    # build started by a hook of another repository is given that one's GIT_DIR, a commit
    # hook its index too: the id was that repository's commit, or a "+" from its index)
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR GIT_NAMESPACE GIT_PREFIX
    id=$(git -C "$src" rev-parse HEAD 2> /dev/null) || id=
    case "$id" in '' | *[!0-9a-f]*) id= ;; esac
    [ "${#id}" -ge 7 ] || id=
    if [ -n "$id" ]; then
        id=$(printf '%.7s' "$id")
        git -C "$src" diff --quiet HEAD -- > /dev/null 2>&1
        [ $? -ne 1 ] || id=$id+
    fi
fi

if [ -z "$out" ]; then
    echo "${id:-unknown}"
    exit 0
fi
if [ -z "$id" ]; then
    if grep -q '^#define WALPE_SOURCE_ID "[0-9a-f][0-9a-f]*+\{0,1\}"$' "$out" 2> /dev/null; then exit 0; fi
    id=unknown
fi
new="/* made by scripts/source-id.sh at every build, rewritten only when the id changes */
#define WALPE_SOURCE_ID \"$id\""
if [ -f "$out" ] && [ "$(cat "$out")" = "$new" ]; then exit 0; fi
printf '%s\n' "$new" > "$out.tmp.$$" && mv -f "$out.tmp.$$" "$out"
