/* sourceid.h - which source this program was built from
 *
 * Test builds and the release they are made after all answer the same
 * version: the version says what was released, not which tree was built.
 * walpe_source_id () is the commit of the tree, abbreviated as git does
 * ("077a0dd"), with a "+" behind it when the build was made in a checkout
 * with changed files, or "unknown" (built outside git from files that do
 * not say). It is found by scripts/source-id.sh when the program is built:
 * from the file SOURCE-ID in a tree `git archive` made, from git in a
 * checkout. Nothing of the build is in it - no date, no host - so the same
 * tree gives the same programs.
 *
 * The word lives in one generated header that only sourceid.c reads, and
 * sourceid.c is compiled into the programs that show it (walpe-daemon,
 * walpe-ctl, walpe-tray), not into the library: a new commit compiles
 * this file again and links those programs, nothing else. */
#pragma once

const char *walpe_source_id (void);
