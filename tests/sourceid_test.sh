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
#   a checkout says its own commit also where the environment names another repository
#   (GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE: a build started by a hook of that one);
#   the header is written once: a second run leaves the file alone (nothing is compiled
#   again), a new commit changes it, and a run that finds no id keeps the one it has.
#   the Arch recipe (contrib/packaging/arch/PKGBUILD) builds a copy of the tree that has
#   no .git: its prepare() hands the copy the id of the tree, with the "+" of a changed
#   checkout (run with bash where there is one);
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

# the build is started with another repository named in the environment - by a hook of
# that repository, which git gives GIT_DIR (and a commit hook GIT_INDEX_FILE): the
# checkout still says its own commit, and it is not "changed" by the other one's index
mkdir "$tmp/other"
git -C "$tmp/other" init -q
echo other > "$tmp/other/o"
git -C "$tmp/other" add -A
git -C "$tmp/other" commit -q -m other
is "a checkout, GIT_DIR names another repository" "$(GIT_DIR="$tmp/other/.git" sh "$script" "$repo")" "$c2"
is "a checkout, GIT_DIR and GIT_WORK_TREE name another repository" \
    "$(GIT_DIR="$tmp/other/.git" GIT_WORK_TREE="$tmp/other" sh "$script" "$repo")" "$c2"
is "a checkout, GIT_DIR=. as in a hook of a bare repository" "$(GIT_DIR=. sh "$script" "$repo")" "$c2"
is "a checkout, GIT_INDEX_FILE names another repository's index" \
    "$(GIT_INDEX_FILE="$tmp/other/.git/index" sh "$script" "$repo")" "$c2"

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
for w in 0123abc:0123abc 0123abcdef01:0123abc 0123ab:unknown 0123abg:unknown '$Format:%H$:unknown' :unknown \
    0123abc+:0123abc+ 0123abcdef01+:0123abc+ 0123abc++:unknown +:unknown; do
    printf '%s\n' "${w%:*}" > "$tmp/words/SOURCE-ID"
    is "SOURCE-ID \"${w%:*}\"" "$(id_of "$tmp/words")" "${w##*:}"
done

# the Arch recipe: makepkg builds a copy that prepare() makes of the tree's files, without
# .git. The copy says what the tree says - from a checkout, a changed checkout and a tree
# made by git archive (the recipe of the tree under test, on trees made here)
recipe=$src/contrib/packaging/arch/PKGBUILD
if command -v bash > /dev/null 2>&1 && [ -f "$recipe" ]; then
    pk=$tmp/pk
    mkdir -p "$pk/tree/contrib/packaging/arch" "$pk/tree/contrib/plasma" "$pk/tree/scripts"
    for f in LICENSE README.md CHANGELOG.md meson_options.txt; do echo x > "$pk/tree/$f"; done
    for d in src gst data protocols licenses docs; do mkdir "$pk/tree/$d"; echo x > "$pk/tree/$d/x"; done
    printf "project('walpe', 'c',\n  version: '0.0.1')\n" > "$pk/tree/meson.build"
    echo x > "$pk/tree/contrib/plasma/x"
    cp "$script" "$pk/tree/scripts/"
    cp "$src/SOURCE-ID" "$src/.gitattributes" "$pk/tree/"
    cp "$recipe" "$pk/tree/contrib/packaging/arch/"
    git -C "$pk/tree" init -q
    git -C "$pk/tree" add -A
    git -C "$pk/tree" commit -q -m recipe
    pc=$(seven "$pk/tree")
    prepared () {   # TREE NAME: the id of the copy prepare() makes of TREE
        rm -rf "$pk/$2"
        mkdir "$pk/$2"
        if (cd "$1/contrib/packaging/arch" && unset WALPE_SRC && srcdir="$pk/$2" bash -c '. ./PKGBUILD && prepare') > "$pk/$2.out" 2>&1; then
            id_of "$pk/$2/walpe"
        else
            echo "prepare failed: $(tail -n 3 "$pk/$2.out" | tr '\n' ' ')"
        fi
    }
    is "the Arch recipe's copy of a checkout" "$(prepared "$pk/tree" a)" "$pc"
    echo changed >> "$pk/tree/README.md"
    is "the Arch recipe's copy of a changed checkout" "$(prepared "$pk/tree" b)" "$pc+"
    git -C "$pk/tree" checkout -q README.md
    mkdir "$pk/arch"
    git -C "$pk/tree" archive --prefix=t/ HEAD | tar -x -C "$pk/arch"
    is "the Arch recipe's copy of a tree made by git archive" "$(prepared "$pk/arch/t" c)" "$pc"
    rm -f "$pk/arch/t/SOURCE-ID"
    is "the Arch recipe's copy of files that do not say" "$(prepared "$pk/arch/t" d)" unknown
else
    echo "(no bash, or no contrib/packaging/arch/PKGBUILD: the Arch recipe's copy was not looked at)"
fi

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
