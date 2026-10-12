#!/usr/bin/env python3
"""Resolves the CODE STYLE (clang-format) suggestion threads of a pull request that are already fixed.

usage: resolve-suggestion-threads.py <owner/repo> <pr-number> <checkout-dir>
Needs GH_TOKEN and the gh CLI. Only review threads that were opened by a bot (cpp-linter-action) with a
clang-format suggestion are considered ("### clang-format" heading in the first comment); clang-tidy
suggestions, other bots and human reviewers are never touched. Such a thread is resolved if it is
unresolved and either became outdated (its lines were changed) or the current file already contains
the suggested text.
"""
MARKER = "### clang-format"
import json, os, re, subprocess, sys, time

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
    """Suggested replacement text; "" for a pure deletion ("Please remove the line(s)"); None if no suggestion."""
    m = re.search(r"```suggestion[^\n]*\n(.*?)```", body, re.S)
    if m:
        return m.group(1).rstrip("\n")
    return "" if "Please remove the line(s)" in body else None


def already_applied(root, t, suggestion):
    """True if the lines the thread points at already equal the suggested text."""
    if t["line"] is None:
        return False
    try:
        lines = open(os.path.join(root, t["path"]), errors="replace").read().split("\n")
    except OSError:
        return False
    first = (t["startLine"] or t["line"]) - 1
    return suggestion != "" and "\n".join(lines[first:t["line"]]) == suggestion


def main():
    repo, pr, root = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    owner, name = repo.split("/")
    time.sleep(int(os.environ.get("RESOLVE_DELAY", "8")))   # GitHub marks threads outdated asynchronously after a push
    after, resolved = None, 0
    while True:
        vars = dict(owner=owner, repo=name, pr=pr)
        if after:
            vars["after"] = after
        data = gh_graphql(QUERY, **vars)["data"]["repository"]["pullRequest"]["reviewThreads"]
        for t in data["nodes"]:
            c = (t["comments"]["nodes"] or [None])[0]
            if c is None:
                continue
            author = c["author"] or {}
            is_bot = author.get("__typename") == "Bot" or str(author.get("login", "")).startswith("github-actions")
            sug = suggestion_text(c["body"])
            take = (not t["isResolved"]) and is_bot and MARKER in c["body"] and sug is not None
            done = take and (t["isOutdated"] or already_applied(root, t, sug))
            print("thread %s:%s resolved=%s outdated=%s author=%s/%s clang-format-suggestion=%s -> %s" % (
                t["path"], t["line"], t["isResolved"], t["isOutdated"], author.get("__typename"), author.get("login"),
                take, "resolve" if done else "keep"))
            if done:
                gh_graphql(RESOLVE, id=t["id"])
                resolved += 1
        if not data["pageInfo"]["hasNextPage"]:
            break
        after = data["pageInfo"]["endCursor"]
    print("resolved %d suggestion thread(s)" % resolved)


if __name__ == "__main__":
    main()
