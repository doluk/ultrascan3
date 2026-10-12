#!/usr/bin/env python3
"""Resolves the CODE STYLE suggestion threads of a pull request that are already fixed.

usage: resolve-suggestion-threads.py <owner/repo> <pr-number> <checkout-dir>
Needs GH_TOKEN and the gh CLI. Only threads created by scripts/post-suggestions.py with the marker
<!-- us3-suggestion:code-style --> are considered (suggestions of other bots/tools and comments of
reviewers are never touched). Such a thread is resolved if it is unresolved and either became outdated
(its lines were changed) or the current file already contains the suggested text.
"""
MARKER = "<!-- us3-suggestion:code-style -->"
import json, os, re, subprocess, sys

QUERY = """
query($owner:String!, $repo:String!, $pr:Int!, $after:String) {
  repository(owner:$owner, name:$repo) { pullRequest(number:$pr) {
    reviewThreads(first:100, after:$after) {
      pageInfo { hasNextPage endCursor }
      nodes { id isResolved isOutdated path line startLine
              comments(first:1) { nodes { body author { login __typename } } } }
    } } } }
"""
RESOLVE = "mutation($id:ID!) { resolveReviewThread(input:{threadId:$id}) { thread { isResolved } } }"


def gh_graphql(query, **vars):
    args = ["gh", "api", "graphql", "-f", "query=" + query]
    for k, v in vars.items():
        args += ["-F" if isinstance(v, int) else "-f", "%s=%s" % (k, v)]
    return json.loads(subprocess.run(args, check=True, capture_output=True, text=True).stdout)


def suggestion_text(body):
    m = re.search(r"```suggestion[^\n]*\n(.*?)```", body, re.S)
    return None if m is None else m.group(1).rstrip("\n")


def already_applied(root, t, suggestion):
    """True if the lines the thread points at already equal the suggested text."""
    if t["line"] is None:
        return False
    try:
        lines = open(os.path.join(root, t["path"]), errors="replace").read().split("\n")
    except OSError:
        return False
    first = (t["startLine"] or t["line"]) - 1
    return "\n".join(lines[first:t["line"]]) == suggestion


def main():
    repo, pr, root = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    owner, name = repo.split("/")
    after, resolved = None, 0
    while True:
        vars = dict(owner=owner, repo=name, pr=pr)
        if after:
            vars["after"] = after
        data = gh_graphql(QUERY, **vars)["data"]["repository"]["pullRequest"]["reviewThreads"]
        for t in data["nodes"]:
            c = (t["comments"]["nodes"] or [None])[0]
            if t["isResolved"] or c is None:
                continue
            sug = suggestion_text(c["body"])
            if MARKER not in c["body"] or sug is None:
                continue
            if t["isOutdated"] or already_applied(root, t, sug):
                gh_graphql(RESOLVE, id=t["id"])
                resolved += 1
        if not data["pageInfo"]["hasNextPage"]:
            break
        after = data["pageInfo"]["endCursor"]
    print("resolved %d suggestion thread(s)" % resolved)


if __name__ == "__main__":
    main()
