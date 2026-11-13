#!/bin/sh
# sourceid_test.sh <source directory>
# scripts/source-id.sh, which finds at build time the commit a tree is (the word the
# programs show as their source: common/sourceid.h), against trees made here:
#   a tree `git archive` made has the commit in its file SOURCE-ID, and git is not asked
#   (the tree lies inside another repository whose commit is a different one);
#   a checkout says its commit, a "+" behind it while a tracked file is changed, the
#   new commit after a commit;
#   the word is the commit's first seven digits everywhere - in a checkout and in an
#   archive, whatever length git abbreviates to there (core.abbrev, the size of the
#   repository; a forge has its own) - and the archive's bytes do not depend on it;
#   files without .git and with the placeholder say "unknown" - also inside somebody
#   else's repository;
#   the header is written once: a second run leaves the file alone (nothing is compiled
#   again), a new commit changes it, and a run that finds no id keeps the one it has.
# Then the tree under test itself, where it is a git checkout with git at hand: `git
# archive HEAD` of it - the way a release tarball and a test build's zip are made -
# names HEAD. Skipped (77) without git.
src=$1
script=$src/scripts/source-id.sh
fails=0
command -v git > /dev/null 2>&1 || { echo "sourceid: git is not installed, skipped"; exit 77; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-sid.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
# (no configuration of the user's or the system's, no repository of a caller's)
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null HOME="$tmp"
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.org GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.org

is () {     # what, got, wanted
    [ "$2" = "$3" ] && return 0
    fails=$((fails + 1))
    echo "FAIL: $1: '$2', expected '$3'"
}
id_of () { sh "$script" "$1"; }
seven () { git -C "$1" rev-parse "${2:-HEAD}" | cut -c 1-7; }

# a repository with the two files that make the id work in an archive
repo=$tmp/repo
mkdir -p "$repo/scripts"
cp "$src/SOURCE-ID" "$src/.gitattributes" "$repo/"
echo one > "$repo/file"
git -C "$repo" init -q
git -C "$repo" add -A
git -C "$repo" commit -q -m one
c1=$(seven "$repo")
is "the placeholder is in the checkout's SOURCE-ID" "$(cat "$repo/SOURCE-ID")" '$Format:%H$'
is "a checkout" "$(id_of "$repo")" "$c1"
echo two >> "$repo/file"
is "a checkout with a changed file" "$(id_of "$repo")" "$c1+"
echo untracked > "$repo/new"
git -C "$repo" commit -q -a -m two
c2=$(seven "$repo")
[ "$c1" != "$c2" ] || { echo "FAIL: the two commits have one id"; fails=$((fails + 1)); }
is "after a commit (an untracked file is no change)" "$(id_of "$repo")" "$c2"

# the header: written, left alone, changed by a commit, kept where no id is found
h=$tmp/walpe-source-id.h
git -C "$repo" checkout -q "$c1" 2> /dev/null
sh "$script" "$repo" "$h"
is "the header" "$(grep '^#define' "$h")" "#define WALPE_SOURCE_ID \"$c1\""
touch -d '2001-01-01 00:00:00' "$h"
stamp=$(stat -c %Y "$h")
sh "$script" "$repo" "$h"
is "a second run leaves the header alone" "$(stat -c %Y "$h")" "$stamp"
git -C "$repo" checkout -q "$c2" 2> /dev/null
sh "$script" "$repo" "$h"
is "the header after another commit" "$(grep '^#define' "$h")" "#define WALPE_SOURCE_ID \"$c2\""
mkdir "$tmp/plain"
cp "$repo/SOURCE-ID" "$repo/file" "$tmp/plain/"
is "files without .git" "$(id_of "$tmp/plain")" unknown
sh "$script" "$tmp/plain" "$h"
is "no id found: the header keeps the one it has" "$(grep '^#define' "$h")" "#define WALPE_SOURCE_ID \"$c2\""
sh "$script" "$tmp/plain" "$tmp/new.h"
is "no id found, no header yet" "$(grep '^#define' "$tmp/new.h")" '#define WALPE_SOURCE_ID "unknown"'

# an archive of the first commit, unpacked inside the repository (now at the second):
# the file says the commit, the repository around it is not asked
mkdir "$repo/unpacked"
git -C "$repo" archive --prefix=x/ "$c1" | tar -x -C "$repo/unpacked"
is "git archive fills SOURCE-ID in" "$(cat "$repo/unpacked/x/SOURCE-ID")" "$(git -C "$repo" rev-parse "$c1")"
is "a tree made by git archive" "$(id_of "$repo/unpacked/x")" "$c1"
# ... and plain files there, with the placeholder: not the commit of the repository around
mkdir "$repo/copy"
cp "$repo/SOURCE-ID" "$repo/copy/"
is "files with the placeholder inside another repository" "$(id_of "$repo/copy")" unknown
# a zip, as the test builds are sent
if command -v unzip > /dev/null 2>&1; then
    git -C "$repo" archive --format=zip --prefix=z/ -o "$tmp/z.zip" "$c2"
    (cd "$tmp" && unzip -q z.zip)
    is "a zip made by git archive" "$(id_of "$tmp/z")" "$c2"
fi

# a repository in which git abbreviates to another length (core.abbrev here; the number
# of objects does the same, and a forge's archive has the forge's length): the word is
# the same seven digits in the checkout and in an archive, and the archive is the same
# file whatever the length is
git -C "$repo" config core.abbrev 12
is "a checkout where git abbreviates to 12 digits" "$(id_of "$repo")" "$c2"
mkdir "$tmp/long" "$tmp/short"
git -C "$repo" archive --prefix=x/ "$c2" > "$tmp/long.tar"
tar -x -C "$tmp/long" -f "$tmp/long.tar"
is "an archive made where git abbreviates to 12 digits" "$(id_of "$tmp/long/x")" "$c2"
git -C "$repo" config core.abbrev 5
is "a checkout where git abbreviates to 5 digits" "$(id_of "$repo")" "$c2"
git -C "$repo" archive --prefix=x/ "$c2" > "$tmp/short.tar"
tar -x -C "$tmp/short" -f "$tmp/short.tar"
is "an archive made where git abbreviates to 5 digits" "$(id_of "$tmp/short/x")" "$c2"
cmp -s "$tmp/long.tar" "$tmp/short.tar" || { echo "FAIL: the archive of one commit differs with the length git abbreviates to"; fails=$((fails + 1)); }
git -C "$repo" config --unset core.abbrev
# what SOURCE-ID may hold: a commit of seven digits or more (an archive made before the
# file held the whole commit has seven or more of them); anything else does not say
mkdir "$tmp/words"
for w in 0123abc:0123abc 0123abcdef01:0123abc 0123ab:unknown 0123abg:unknown '$Format:%H$:unknown' :unknown; do
    printf '%s\n' "${w%:*}" > "$tmp/words/SOURCE-ID"
    is "SOURCE-ID \"${w%:*}\"" "$(id_of "$tmp/words")" "${w##*:}"
done

# the tree under test: an archive of its HEAD names HEAD
if [ -e "$src/.git" ] && head=$(seven "$src" 2> /dev/null) && [ -n "$head" ]; then
    mkdir "$tmp/export"
    if git -C "$src" cat-file -e HEAD:SOURCE-ID 2> /dev/null && git -C "$src" cat-file -e HEAD:scripts/source-id.sh 2> /dev/null; then
        git -C "$src" archive HEAD SOURCE-ID .gitattributes scripts/source-id.sh | tar -x -C "$tmp/export"
        is "git archive HEAD of this tree: SOURCE-ID" "$(cat "$tmp/export/SOURCE-ID")" "$(git -C "$src" rev-parse HEAD)"
        is "git archive HEAD of this tree: the id" "$(sh "$tmp/export/scripts/source-id.sh" "$tmp/export")" "$head"
    else
        echo "(SOURCE-ID is not committed in this tree yet: the archive of HEAD was not looked at)"
    fi
    case "$(id_of "$src")" in
    "$head" | "$head+") ;;
    *) fails=$((fails + 1)); echo "FAIL: this tree: '$(id_of "$src")', expected $head or $head+" ;;
    esac
else
    # no checkout: then the tree was made by git archive, or its files do not say
    case "$(id_of "$src")" in
    unknown) echo "(this tree is no git checkout and its SOURCE-ID names no commit: \"unknown\")" ;;
    *[!0-9a-f]* | '') fails=$((fails + 1)); echo "FAIL: this tree: '$(id_of "$src")' is no commit and not \"unknown\"" ;;
    esac
fi

if [ $fails != 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "sourceid: ok"
