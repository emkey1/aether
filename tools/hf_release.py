#!/usr/bin/env python3
"""Publish a release folder to a Hugging Face model repo, with a check before anything becomes public.

    python3 tools/hf_release.py upload  REPO FOLDER [--revision BRANCH] [--large] [--tag-main TAG] -m MESSAGE
    python3 tools/hf_release.py verify  REPO FOLDER [--revision BRANCH]      # writes FOLDER/../<name>.verified.json
    python3 tools/hf_release.py promote REPO FOLDER --from-revision BRANCH -m MESSAGE   # an existing repo: main moves
    python3 tools/hf_release.py publish REPO FOLDER                          # a new repo: private -> public
    python3 tools/hf_release.py readme  REPO FILE -m MESSAGE                 # replace README.md only, checked
    python3 tools/hf_release.py status  REPO                                 # visibility, refs, downloads, likes

The order is fixed by what each step refuses:
- `upload` creates a missing repo PRIVATE, and never writes to `main` of a repo that is already public: that
  goes to a branch (`--revision`), and `--tag-main` first tags the published main so it stays downloadable.
- `verify` compares every local file with the Hub (sha256 for LFS files, git blob id for the rest) and lists
  anything on the Hub that is not local. Its record names the commit it checked.
- `promote` and `publish` refuse without a passing record for the same folder contents, and `promote` also
  refuses when the branch has moved since the check.

The token is read from $HF_TOKEN and from nowhere else; it is never written or printed. A release makes or
changes public content, so each of upload-to-public, promote, publish and readme is the owner's decision.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys
import time

SKIP_LOCAL = (".cache",)
SKIP_REMOTE = {".gitattributes"}


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 24), b""):
            h.update(block)
    return h.hexdigest()


def blob_id(data: bytes) -> str:
    """The git blob id of a file's bytes, which the Hub reports for files stored outside LFS."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def local_files(folder: pathlib.Path) -> list[str]:
    out = []
    for p in sorted(folder.rglob("*")):
        rel = p.relative_to(folder)
        if p.is_file() and not rel.parts[0].startswith(".") and rel.parts[0] not in SKIP_LOCAL:
            out.append(rel.as_posix())
    return out


def compare(folder: pathlib.Path, remote: dict[str, dict]) -> tuple[list[str], dict[str, str]]:
    """Problems (empty when the Hub matches the folder) and each local file's digest.

    `remote` maps a path to {"size", "lfs_sha256" or None, "blob_id"}."""
    problems, digests = [], {}
    names = local_files(folder)
    if not names:
        problems.append(f"no files in {folder}")
    for name in names:
        path, entry = folder / name, remote.get(name)
        if entry is None:
            problems.append(f"missing on the Hub: {name}")
            continue
        if entry.get("lfs_sha256"):
            digest = sha256_file(path)
            ok = entry["lfs_sha256"] == digest and entry.get("size") == path.stat().st_size
        else:
            digest = blob_id(path.read_bytes())
            ok = entry.get("blob_id") == digest
        digests[name] = digest
        if not ok:
            problems.append(f"content differs: {name}")
    for name in sorted(set(remote) - set(names) - SKIP_REMOTE):
        problems.append(f"on the Hub but not local: {name}")
    return problems, digests


def record_path(folder: pathlib.Path) -> pathlib.Path:
    return folder.parent / f"{folder.name}.verified.json"


def check_record(folder: pathlib.Path, repo: str, revision: str) -> dict:
    """The passing record for this repo, revision and folder contents, or exit."""
    path = record_path(folder)
    if not path.exists():
        raise SystemExit(f"refusing: no verify record at {path}; run verify first")
    rec = json.loads(path.read_text(encoding="utf-8"))
    if rec.get("repo") != repo or rec.get("revision") != revision or rec.get("result") != "pass":
        raise SystemExit(f"refusing: {path} is not a passing check of {repo}@{revision}")
    now = {n: (sha256_file(folder / n) if len(d) == 64 else blob_id((folder / n).read_bytes()))
           for n, d in rec["files"].items() if (folder / n).exists()}
    if now != rec["files"] or sorted(now) != local_files(folder):
        raise SystemExit(f"refusing: {folder} changed since it was verified; run verify again")
    return rec


# --------------------------------------------------------------------------- Hub calls

def hub():
    token = os.environ.get("HF_TOKEN")
    if not token:
        raise SystemExit("HF_TOKEN is not set (pipe it in for this command; never store it)")
    from huggingface_hub import HfApi
    return HfApi(token=token)


def remote_tree(api, repo: str, revision: str) -> dict[str, dict]:
    out = {}
    for f in api.list_repo_tree(repo, revision=revision, recursive=True, expand=True):
        if hasattr(f, "size"):
            out[f.path] = {"size": f.size, "lfs_sha256": f.lfs.sha256 if f.lfs else None, "blob_id": f.blob_id}
    return out


def head_of(api, repo: str, revision: str) -> str:
    refs = api.list_repo_refs(repo)
    for ref in list(refs.branches) + list(refs.tags):
        if ref.name == revision:
            return ref.target_commit
    raise SystemExit(f"{repo} has no branch or tag {revision!r}")


def exists(api, repo: str) -> bool:
    from huggingface_hub.utils import RepositoryNotFoundError
    try:
        api.model_info(repo)
        return True
    except RepositoryNotFoundError:
        return False


def cmd_upload(args) -> int:
    api, folder = hub(), args.folder
    print("account:", api.whoami()["name"], flush=True)
    if not exists(api, args.repo):
        api.create_repo(args.repo, private=True, exist_ok=True, repo_type="model")
        print(f"{args.repo}: created private", flush=True)
    public = not api.model_info(args.repo).private
    if public and args.revision == "main":
        raise SystemExit(f"refusing: {args.repo} is public; upload to a branch (--revision), verify, then promote")
    if args.tag_main:
        if args.tag_main not in {t.name for t in api.list_repo_refs(args.repo).tags}:
            api.create_tag(args.repo, tag=args.tag_main, revision=head_of(api, args.repo, "main"),
                           tag_message=f"main before: {args.message}")
            print(f"{args.repo}: tagged main as {args.tag_main}", flush=True)
    if args.revision != "main":
        api.create_branch(args.repo, branch=args.revision, exist_ok=True)
    started = time.time()
    if args.large:
        api.upload_large_folder(repo_id=args.repo, folder_path=str(folder), repo_type="model",
                                revision=args.revision, num_workers=args.workers)
    else:
        api.upload_folder(repo_id=args.repo, folder_path=str(folder), repo_type="model", revision=args.revision,
                          ignore_patterns=[".cache/**"], commit_message=args.message)
    print(f"{args.repo}@{args.revision}: uploaded in {time.time() - started:.0f}s; public={public}", flush=True)
    print("UPLOAD DONE", flush=True)
    return 0


def cmd_verify(args) -> int:
    api = hub()
    commit = head_of(api, args.repo, args.revision)
    problems, digests = compare(args.folder, remote_tree(api, args.repo, args.revision))
    for line in problems:
        print("PROBLEM:", line, flush=True)
    rec = {"repo": args.repo, "revision": args.revision, "commit": commit, "files": digests,
           "result": "fail" if problems else "pass",
           "checked_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    record_path(args.folder).write_text(json.dumps(rec, indent=2) + "\n", encoding="utf-8")
    print(f"VERIFY {'FAIL (' + str(len(problems)) + ')' if problems else 'PASS'}: {len(digests)} files, "
          f"{args.repo}@{args.revision} {commit[:8]}", flush=True)
    return 1 if problems else 0


def cmd_promote(args) -> int:
    api = hub()
    rec = check_record(args.folder, args.repo, args.from_revision)
    if head_of(api, args.repo, args.from_revision) != rec["commit"]:
        raise SystemExit(f"refusing: {args.from_revision} moved since it was verified; run verify again")
    # The LFS objects are on the Hub already, so only pointers and small files move.
    commit = api.upload_folder(repo_id=args.repo, folder_path=str(args.folder), repo_type="model", revision="main",
                               ignore_patterns=[".cache/**"], commit_message=args.message)
    problems, _ = compare(args.folder, remote_tree(api, args.repo, "main"))
    for line in problems:
        print("PROBLEM:", line, flush=True)
    print(f"{args.repo}: main -> {commit.oid[:8]}; {'PROMOTE FAILED CHECK' if problems else 'PROMOTE DONE'}", flush=True)
    return 1 if problems else 0


def cmd_publish(args) -> int:
    api = hub()
    rec = check_record(args.folder, args.repo, "main")
    if head_of(api, args.repo, "main") != rec["commit"]:
        raise SystemExit("refusing: main moved since it was verified; run verify again")
    api.update_repo_settings(args.repo, private=False)
    private = api.model_info(args.repo).private
    print(f"{args.repo}: private = {private}; {'PUBLISHED' if not private else 'STILL PRIVATE'}", flush=True)
    return 1 if private else 0


def cmd_readme(args) -> int:
    api = hub()
    data = args.file.read_bytes()
    commit = api.upload_file(path_or_fileobj=data, path_in_repo="README.md", repo_id=args.repo, repo_type="model",
                             revision=args.revision, commit_message=args.message)
    landed = remote_tree(api, args.repo, args.revision).get("README.md", {}).get("blob_id")
    ok = landed == blob_id(data)
    print(f"{args.repo}@{args.revision}: commit {commit.oid[:8]}, README {'OK' if ok else 'MISMATCH'}", flush=True)
    return 0 if ok else 1


def cmd_status(args) -> int:
    api = hub()
    info = api.model_info(args.repo)
    refs = api.list_repo_refs(args.repo)
    print(json.dumps({
        "repo": args.repo, "private": info.private, "downloads": info.downloads, "likes": info.likes,
        "last_modified": str(info.last_modified), "files": len(info.siblings or []),
        "branches": {b.name: b.target_commit[:8] for b in refs.branches},
        "tags": {t.name: t.target_commit[:8] for t in refs.tags},
        "discussions": sum(1 for _ in api.get_repo_discussions(args.repo)),
    }, indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    def add(name, fn, folder=True):
        p = sub.add_parser(name)
        p.add_argument("repo")
        if folder:
            p.add_argument("folder", type=pathlib.Path)
        p.set_defaults(fn=fn)
        return p

    p = add("upload", cmd_upload)
    p.add_argument("--revision", default="main")
    p.add_argument("--large", action="store_true", help="resumable multi-commit upload for weight folders")
    p.add_argument("--workers", type=int, default=4)
    p.add_argument("--tag-main", help="tag the current main with this name first (once)")
    p.add_argument("-m", "--message", required=True)
    add("verify", cmd_verify).add_argument("--revision", default="main")
    p = add("promote", cmd_promote)
    p.add_argument("--from-revision", required=True)
    p.add_argument("-m", "--message", required=True)
    add("publish", cmd_publish)
    p = add("readme", cmd_readme, folder=False)
    p.add_argument("file", type=pathlib.Path)
    p.add_argument("--revision", default="main")
    p.add_argument("-m", "--message", required=True)
    add("status", cmd_status, folder=False)
    args = ap.parse_args(argv)
    if getattr(args, "folder", None) is not None and not args.folder.is_dir():
        raise SystemExit(f"not a folder: {args.folder}")
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
