#!/usr/bin/env python3
"""Posts the uncommitted changes of the worktree as "suggested change" comments on a pull request.

usage: post-suggestions.py <owner/repo> <pr-number> <head-sha> <tool>
Run it in a checkout of the PR head after an auto-fixer has modified the worktree (git diff = the fixes).
Needs GH_TOKEN and the gh CLI.

* Every comment carries the hidden marker <!-- us3-suggestion:<tool> -->, so that one tool's
  threads can be identified (and resolved) without touching other bots' suggestions.
* Hunks that overlap a still-open suggestion of ANY of our tools are skipped, so that two fixers
  (e.g. code style and clang-tidy) never post competing suggestions for the same lines.
* Hunks outside the diff of the pull request cannot be commented on by GitHub and are skipped.
"""
import json, re, subprocess, sys

QUERY = """
query($owner:String!, $repo:String!, $pr:Int!, $after:String) {
  repository(owner:$owner, name:$repo) { pullRequest(number:$pr) {
    reviewThreads(first:100, after:$after) {
      pageInfo { hasNextPage endCursor }
      nodes { isResolved isOutdated path line startLine
              comments(first:1) { nodes { body } } }
    } } } }
"""


def run(*args, stdin=None):
    return subprocess.run(args, check=True, capture_output=True, text=True, errors="replace", input=stdin).stdout


def open_ranges(owner, repo, pr):
    """(path, first, last) of unresolved, current suggestion threads of our tools."""
    out, after = [], None
    while True:
        args = ["gh", "api", "graphql", "-f", "query=" + QUERY, "-f", "owner=" + owner, "-f", "repo=" + repo, "-F", "pr=%d" % pr]
        if after:
            args += ["-f", "after=" + after]
        data = json.loads(run(*args))["data"]["repository"]["pullRequest"]["reviewThreads"]
        for t in data["nodes"]:
            c = (t["comments"]["nodes"] or [{"body": ""}])[0]["body"]
            if not t["isResolved"] and not t["isOutdated"] and "<!-- us3-suggestion:" in c and t["line"]:
                out.append((t["path"], t["startLine"] or t["line"], t["line"]))
        if not data["pageInfo"]["hasNextPage"]:
            return out
        after = data["pageInfo"]["endCursor"]


def hunks():
    """Yield (path, first_old_line, last_old_line, new_lines) for every hunk of `git diff -U0`."""
    path, old_lines = None, []
    diff = run("git", "diff", "-U0", "--no-color")
    cur = None
    for l in diff.splitlines():
        if l.startswith("+++ b/"):
            path = l[6:]
        elif l.startswith("@@"):
            if cur:
                yield cur
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@", l)
            start, cnt = int(m.group(1)), int(1 if m.group(2) is None else m.group(2))
            cur = [path, start, cnt, []]
        elif cur and l.startswith("+") and not l.startswith("+++"):
            cur[3].append(l[1:])
    if cur:
        yield cur


def main():
    repo, pr, sha, tool = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    owner, name = repo.split("/")
    taken = open_ranges(owner, name, pr)
    posted = skipped = 0
    for path, start, cnt, new in hunks():
        if cnt == 0:
            # pure insertion: GitHub needs an existing line to anchor on, so replace the line before it
            if start == 0:
                skipped += 1
                continue
            old_text = run("git", "show", "HEAD:" + path).split("\n")[start - 1]
            first, last, new = start, start, [old_text] + new
        else:
            first, last = start, start + cnt - 1
        if any(p == path and not (last < a or first > b) for p, a, b in taken):
            skipped += 1
            continue
        body = "<!-- us3-suggestion:%s -->\n**%s**: suggested change\n```suggestion\n%s\n```\n" % (tool, tool, "\n".join(new))
        if not new:
            body = body.replace("\n\n```", "\n```")   # an empty suggestion block deletes the lines
        args = ["gh", "api", "repos/%s/pulls/%d/comments" % (repo, pr), "-f", "body=" + body, "-f", "commit_id=" + sha,
                "-f", "path=" + path, "-F", "line=%d" % last, "-f", "side=RIGHT"]
        if last > first:
            args += ["-F", "start_line=%d" % first, "-f", "start_side=RIGHT"]
        try:
            run(*args)
            posted += 1
            taken.append((path, first, last))
        except subprocess.CalledProcessError:
            skipped += 1   # outside the diff of the pull request
    print("%s: posted %d suggestion(s), skipped %d (overlapping, unchanged-by-PR or unanchorable)" % (tool, posted, skipped))


if __name__ == "__main__":
    main()
