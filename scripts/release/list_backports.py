#!/usr/bin/env python3
# Copyright (c) 2022, Meta
#
# SPDX-License-Identifier: Apache-2.0

"""Query issues in a release branch

This script searches for issues referenced via pull-requests in a release
branch in order to simplify tracking changes such as automated backports,
manual backports, security fixes, and stability fixes.

A formatted report is printed to standard output either in JSON or
reStructuredText.

Since an issue is required for all changes to release branches, merged PRs
must reference at least one issue in the body using one of the closing
keywords GitHub supports, e.g. "Fixes #1234". This script will throw an error
if a PR has been made without an associated issue.

Usage:
    ./scripts/release/list_backports.py \
        -t ~/.ghtoken \
        -b v2.7-branch \
        -s 2021-12-15 -e 2022-04-22 \
        -P 45074 -P 45868 -P 44918 -P 41234 -P 41174 \
        -j | jq . | tee /tmp/backports.json

    GITHUB_TOKEN="<secret>" \
    ./scripts/release/list_backports.py \
        -b v3.0-branch \
        -p 43381 \
        -j | jq . | tee /tmp/backports.json
"""

import argparse
from datetime import datetime, timedelta
import io
import json
import logging
import os
import re
import sys

# Requires PyGithub
from github import Auth, Github
from github.GithubException import UnknownObjectException

# Keywords GitHub accepts to link a pull request to an issue, see
# https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/linking-a-pull-request-to-an-issue
# They are case insensitive and may be followed by an optional colon.
CLOSING_KEYWORDS = r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\b[:]?\s*"

# Markdown container prefixes that may hold nested code blocks.
BLOCK_QUOTE_MARKER = re.compile(r" {0,3}> ?")
LIST_MARKER = re.compile(r"(?:[-+*]|\d{1,9}[.)])( +|$)")


def is_escaped(text, index):
    """Return True when the character at index is preceded by an odd backslash run."""
    backslashes = 0
    i = index - 1

    while i >= 0 and text[i] == "\\":
        backslashes += 1
        i -= 1

    return backslashes % 2 == 1


def find_code_span_closer(text, start, run_length):
    """Return the index of the next backtick run of exactly run_length, or -1.

    A code span never spans a blank line, so the search stops at a block
    boundary.
    """
    i = start

    while i < len(text):
        if text.startswith("\n\n", i):
            return -1

        if text[i] != "`":
            i += 1
            continue

        run_start = i
        while i < len(text) and text[i] == "`":
            i += 1

        if i - run_start == run_length:
            return run_start

    return -1


def remove_inline_constructs(text):
    """Remove inline code spans and HTML comments in a single pass.

    Unmatched backtick runs and escaped backticks are kept as literal text,
    while an unclosed HTML comment runs to the end of the text.
    """
    out = []
    i = 0

    while i < len(text):
        if text.startswith("<!--", i):
            closing = text.find("-->", i + 4)
            if closing == -1:
                break

            i = closing + 3
            continue

        if text[i] != "`" or is_escaped(text, i):
            out.append(text[i])
            i += 1
            continue

        run_start = i
        while i < len(text) and text[i] == "`":
            i += 1
        run_length = i - run_start

        closing = find_code_span_closer(text, i, run_length)
        if closing == -1:
            out.append(text[run_start:i])
            continue

        i = closing + run_length

    return "".join(out)


def split_block_quote_prefix(line):
    """Return the block quote depth of a line and its remaining content."""
    content = line.expandtabs(4)
    depth = 0

    while True:
        match = BLOCK_QUOTE_MARKER.match(content)
        if match is None:
            return depth, content

        content = content[match.end() :]
        depth += 1


def strip_list_markers(content, base_indent):
    """Blank out leading list markers, returning the item content and its indent.

    More than four spaces after a marker are item content, not padding, so only
    one space is consumed and the remainder is left to indent the content.
    """
    indent = base_indent
    found = False

    while True:
        stripped = content.lstrip(" ")
        marker_indent = len(content) - len(stripped)
        if marker_indent > indent + 3:
            break

        match = LIST_MARKER.match(stripped)
        if match is None:
            break

        spaces = len(match.group(1))
        marker_length = match.end() - spaces
        padding = spaces if 1 <= spaces <= 4 else 1

        indent = marker_indent + marker_length + padding
        content = " " * indent + stripped[marker_length + padding :]
        found = True

    return found, indent, content


def strip_non_text_markdown_regions(body):
    """Strip markdown regions where GitHub does not resolve closing keywords."""
    text_lines = []
    in_fenced_code_block = False
    fenced_code_char = ""
    fenced_code_length = 0
    fenced_code_indent = 0
    fenced_code_depth = 0
    in_indented_code_block = False
    base_indent = 0
    quote_depth = 0
    previous_line_blank = True

    def end_block():
        """Mark a block boundary so inline constructs do not pair across it."""
        if text_lines and text_lines[-1] != "":
            text_lines.append("")

    for line in body.splitlines():
        depth, content = split_block_quote_prefix(line)
        blank = content.strip() == ""
        indent = len(content) - len(content.lstrip(" "))

        if in_fenced_code_block:
            if depth == fenced_code_depth:
                if re.fullmatch(
                    rf" {{0,{fenced_code_indent + 3}}}"
                    rf"{re.escape(fenced_code_char)}{{{fenced_code_length},}}[ \t]*",
                    content,
                ):
                    in_fenced_code_block = False
                continue
            if blank:
                continue
            in_fenced_code_block = False

        if depth != quote_depth:
            end_block()
            quote_depth = depth
            base_indent = 0
            in_indented_code_block = False
            previous_line_blank = True

        if blank:
            end_block()
            previous_line_blank = True
            continue

        if in_indented_code_block:
            if indent >= base_indent + 4:
                previous_line_blank = False
                continue
            in_indented_code_block = False

        found, list_indent, content = strip_list_markers(content, base_indent)
        if found:
            base_indent = list_indent
            indent = len(content) - len(content.lstrip(" "))
            previous_line_blank = True
        else:
            base_indent = min(base_indent, indent)

        if previous_line_blank and indent >= base_indent + 4:
            end_block()
            in_indented_code_block = True
            previous_line_blank = False
            continue

        match = re.match(r" *(`{3,}|~{3,})(.*)", content)
        if (
            match
            and indent - base_indent <= 3
            and not (match.group(1)[0] == "`" and "`" in match.group(2))
        ):
            fence = match.group(1)
            end_block()
            in_fenced_code_block = True
            fenced_code_char = fence[0]
            fenced_code_length = len(fence)
            fenced_code_indent = indent
            fenced_code_depth = depth
            previous_line_blank = False
            continue

        text_lines.append(content)
        previous_line_blank = False

    return remove_inline_constructs("\n".join(text_lines))


# https://gist.github.com/monkut/e60eea811ef085a6540f
def valid_date_type(arg_date_str):
    """custom argparse *date* type for user dates values given from the
    command line"""
    try:
        return datetime.strptime(arg_date_str, "%Y-%m-%d").replace(tzinfo=datetime.UTC)
    except ValueError:
        msg = "Given Date ({0}) not valid! Expected format, YYYY-MM-DD!".format(arg_date_str)
        raise argparse.ArgumentTypeError(msg)


def parse_args():
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument(
        '-t',
        '--token',
        dest='tokenfile',
        help='File containing GitHub token (alternatively, use GITHUB_TOKEN env variable)',
        metavar='FILE',
    )
    parser.add_argument(
        '-b',
        '--base',
        dest='base',
        help='branch (base) for PRs (e.g. v2.7-branch)',
        metavar='BRANCH',
        required=True,
    )
    parser.add_argument(
        '-j',
        '--json',
        dest='json',
        action='store_true',
        help='print output in JSON rather than RST',
    )
    parser.add_argument(
        '-s',
        '--start',
        dest='start',
        help='start date (YYYY-mm-dd)',
        metavar='START_DATE',
        type=valid_date_type,
    )
    parser.add_argument(
        '-e',
        '--end',
        dest='end',
        help='end date (YYYY-mm-dd)',
        metavar='END_DATE',
        type=valid_date_type,
    )
    parser.add_argument("-o", "--org", default="zephyrproject-rtos", help="Github organization")
    parser.add_argument(
        '-p',
        '--include-pull',
        dest='includes',
        help='include pull request (can be specified multiple times)',
        metavar='PR',
        type=int,
        action='append',
        default=[],
    )
    parser.add_argument(
        '-P',
        '--exclude-pull',
        dest='excludes',
        help='exlude pull request (can be specified multiple times, helpful for version bumps and release notes)',
        metavar='PR',
        type=int,
        action='append',
        default=[],
    )
    parser.add_argument("-r", "--repo", default="zephyr", help="Github repository")
    parser.add_argument(
        '-c',
        '--check',
        dest='check',
        action='store_true',
        help='check mode: verify a single PR has an associated issue',
    )
    parser.add_argument(
        '--comment',
        dest='comment',
        action='store_true',
        help='in check mode, post a comment on the PR when an issue is missing, and '
        'remove any such comment once one is added. Requires a token with write access '
        'to pull requests, which CI only has for pull requests from a branch of this '
        'repository, not from a fork',
    )

    args = parser.parse_args()

    if args.comment and not args.check:
        logging.error('--comment requires --check')
        return None

    if args.check:
        if len(args.includes) != 1:
            logging.error('--check requires exactly one -p/--include-pull PR number')
            return None
    elif args.includes:
        if getattr(args, 'start'):
            logging.error('the --start argument should not be used with --include-pull')
            return None
        if getattr(args, 'end'):
            logging.error('the --end argument should not be used with --include-pull')
            return None
    else:
        if not getattr(args, 'start'):
            logging.error('if --include-pr PR is not used, --start START_DATE is required')
            return None

        if not getattr(args, 'end'):
            setattr(args, 'end', datetime.now(datetime.UTC))

        if args.end < args.start:
            logging.error(f'end date {args.end} is before start date {args.start}')
            return None

    if args.tokenfile:
        with open(args.tokenfile, 'r') as file:
            token = file.read()
            token = token.strip()
    else:
        if 'GITHUB_TOKEN' not in os.environ:
            raise ValueError('No credentials specified')
        token = os.environ['GITHUB_TOKEN']

    setattr(args, 'token', token)

    return args


class Backport(object):
    def __init__(self, repo, base, pulls):
        self._base = base
        self._repo = repo
        self._issues = []
        self._pulls = pulls

        self._pulls_without_an_issue = []
        self._pulls_with_invalid_issues = {}

    @staticmethod
    def by_date_range(repo, base, start_date, end_date, excludes):
        """Create a Backport object with the provided repo,
        base, start datetime object, and end datetime objects, and
        list of excluded PRs"""

        pulls = []

        unfiltered_pulls = repo.get_pulls(base=base, state='closed')
        for p in unfiltered_pulls:
            if not p.merged:
                # only consider merged backports
                continue

            if p.closed_at < start_date or p.closed_at >= end_date + timedelta(1):
                # only concerned with PRs within time window
                continue

            if p.number in excludes:
                # skip PRs that have been explicitly excluded
                continue

            pulls.append(p)

        # paginated_list.sort() does not exist
        pulls = sorted(pulls, key=lambda x: x.number)

        return Backport(repo, base, pulls)

    @staticmethod
    def by_included_prs(repo, base, includes):
        """Create a Backport object with the provided repo,
        base, and list of included PRs"""

        pulls = []

        for i in includes:
            try:
                p = repo.get_pull(i)
            except Exception:
                p = None

            if not p:
                logging.error(f'{i} is not a valid pull request')
                return None

            if p.base.ref != base:
                logging.error(f'{i} is not a valid pull request for base {base} ({p.base.label})')
                return None

            pulls.append(p)

        # paginated_list.sort() does not exist
        pulls = sorted(pulls, key=lambda x: x.number)

        return Backport(repo, base, pulls)

    @staticmethod
    def sanitize_title(title):
        # TODO: sanitize titles such that they are suitable for both JSON and ReStructured Text
        # could also automatically fix titles like "Automated backport of PR #1234"
        return title

    def print(self):
        for i in self.get_issues():
            title = Backport.sanitize_title(i.title)
            # * :github:`38972` - logging: Cleaning references to tracing in logging
            print(f'* :github:`{i.number}` - {title}')

    def print_json(self):
        issue_objects = []
        for i in self.get_issues():
            obj = {}
            obj['id'] = i.number
            obj['title'] = Backport.sanitize_title(i.title)
            obj['url'] = (
                f'https://github.com/{self._repo.organization.login}/{self._repo.name}/pull/{i.number}'
            )
            issue_objects.append(obj)

        print(json.dumps(issue_objects))

    def get_pulls(self):
        return self._pulls

    def get_issues(self):
        """Return GitHub issues fixed in the provided date window"""
        if self._issues:
            return self._issues

        issue_map = {}
        self._pulls_without_an_issue = []
        self._pulls_with_invalid_issues = {}

        issue_re = re.compile(
            CLOSING_KEYWORDS
            + r"(?:#|https://github\.com/"
            + rf"{re.escape(self._repo.organization.login)}/{re.escape(self._repo.name)}"
            + r"/issues/)([1-9][0-9]*)\b",
            re.IGNORECASE,
        )

        for p in self._pulls:
            # check for issues in this pr
            issues_for_this_pr = {}
            body = strip_non_text_markdown_regions(p.body or '')
            with io.StringIO(body) as buf:
                for line in buf.readlines():
                    line = line.strip()
                    for match in issue_re.finditer(line):
                        issue_number = int(match[1])
                        try:
                            issue = self._repo.get_issue(issue_number)
                        except UnknownObjectException:
                            issue = None
                        if not issue:
                            self._pulls_with_invalid_issues.setdefault(p.number, []).append(
                                issue_number
                            )
                            logging.error(
                                f'https://github.com/{self._repo.organization.login}/{self._repo.name}/pull/{p.number} references invalid issue number {issue_number}'
                            )
                            continue
                        issues_for_this_pr[issue_number] = issue

            # report prs missing issues later
            if len(issues_for_this_pr) == 0:
                logging.error(
                    f'https://github.com/{self._repo.organization.login}/{self._repo.name}/pull/{p.number} does not have an associated issue'
                )
                self._pulls_without_an_issue.append(p)
                continue

            # FIXME: when we have upgrade to python3.9+, use "issue_map | issues_for_this_pr"
            issue_map = {**issue_map, **issues_for_this_pr}

        issues = list(issue_map.values())

        # paginated_list.sort() does not exist
        issues = sorted(issues, key=lambda x: x.number)

        self._issues = issues

        return self._issues

    def get_pulls_without_issues(self):
        if self._pulls_without_an_issue:
            return self._pulls_without_an_issue

        self.get_issues()

        return self._pulls_without_an_issue

    def get_pulls_with_invalid_issues(self):
        if self._pulls_with_invalid_issues:
            return self._pulls_with_invalid_issues

        self.get_issues()

        return self._pulls_with_invalid_issues


BACKPORT_CHECK_MARKER = '<!-- backport-issue-check -->'

MISSING_ISSUE_COMMENT = (
    BACKPORT_CHECK_MARKER + '\n'
    'This pull request to a release branch does not have an associated GitHub issue.\n\n'
    'Please update the pull request description to include a reference to the issue '
    'being fixed, for example:\n\n'
    '```\nFixes #<issue_number>\n```\n\n'
    'For stable releases, all changes MUST have a reference to an issue or a published '
    'advisory documenting:\n'
    '- the issue being fixed\n'
    '- its severity/impact\n'
    '\nSee https://docs.zephyrproject.org/latest/project/release_process.html#issue-tracking-during-feature-freeze '
    'for more details.'
)


def clear_bot_comments(pr):
    """Delete any previous bot comments on pr that contain the marker."""
    try:
        for c in pr.get_issue_comments():
            if BACKPORT_CHECK_MARKER in c.body:
                try:
                    c.delete()
                except Exception as e:
                    logging.warning(f'failed to delete comment {c.id} on PR #{pr.number}: {e}')
    except Exception as e:
        logging.warning(f'failed to list comments on PR #{pr.number}: {e}')


def post_missing_issue_comment(pr):
    """Replace any previous bot comment on pr with a fresh one."""
    clear_bot_comments(pr)
    try:
        pr.create_issue_comment(MISSING_ISSUE_COMMENT)
    except Exception as e:
        logging.warning(f'failed to post comment on PR #{pr.number}: {e}')


def main():
    args = parse_args()

    if not args:
        return os.EX_DATAERR

    try:
        gh = Github(auth=Auth.Token(args.token))
    except Exception:
        logging.error('failed to authenticate with GitHub')
        return os.EX_DATAERR

    try:
        repo = gh.get_repo(args.org + '/' + args.repo)
    except Exception:
        logging.error('failed to obtain Github repository')
        return os.EX_DATAERR

    bp = None
    if args.includes:
        bp = Backport.by_included_prs(repo, args.base, set(args.includes))
    else:
        bp = Backport.by_date_range(repo, args.base, args.start, args.end, set(args.excludes))

    if not bp:
        return os.EX_DATAERR

    pulls_with_invalid_issues = bp.get_pulls_with_invalid_issues()
    if pulls_with_invalid_issues:
        logging.error('The following PRs link to invalid issues:')
        for pr_number, lst in pulls_with_invalid_issues.items():
            logging.error(
                f'\nhttps://github.com/{repo.organization.login}/{repo.name}/pull/{pr_number}: {lst}'
            )
        return os.EX_DATAERR

    pulls_without_issues = bp.get_pulls_without_issues()
    if pulls_without_issues:
        if args.comment:
            for p in pulls_without_issues:
                post_missing_issue_comment(p)
        logging.error(
            'Please ensure the body of each PR to a release branch contains "Fixes #1234"'
        )
        logging.error('The following PRs are lacking associated issues:')
        for p in pulls_without_issues:
            logging.error(
                f'https://github.com/{repo.organization.login}/{repo.name}/pull/{p.number}'
            )
        return os.EX_DATAERR

    if args.check:
        if args.comment:
            # clean up any previous failure comments now that the PR is valid
            for p in bp.get_pulls():
                clear_bot_comments(p)
        return os.EX_OK

    if args.json:
        bp.print_json()
    else:
        bp.print()

    return os.EX_OK


if __name__ == '__main__':
    sys.exit(main())
