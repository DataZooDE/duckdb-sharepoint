#!/usr/bin/env bash
# Fail if anything credential-shaped is tracked by git, or if a token-shaped
# string appears in the sources. .gitignore only protects people who never ran
# `git add -f`.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# 1. Tracked files matching a secret-file pattern.
while IFS= read -r f; do
	case "$f" in
		# `.example` files exist to show the shape (as in duckdb-gdrive); the
		# value check below still runs over them.
		*.example) ;;
		# .env.sharepoint* covers .env.sharepoint.delegated, where
		# scripts/delegated_login.py writes a user's refresh token.
		*.pem|*.pfx|*.client_secret.json|azure-app-*.json|.env.sharepoint*)
			echo "ERROR: credential-shaped file is tracked by git: $f"
			fail=1
			;;
	esac
done < <(git ls-files)

# 2. A pasted Azure AD client secret or bearer token in tracked *source* files.
#    Entra client secrets and JWTs are long; a real one never belongs in source.
#    Placeholders ('...', 'not-a-real-secret', all-zero GUIDs) are allowed.
if git grep -nEI "eyJ[A-Za-z0-9_-]{20,}\." -- 'src/**' 'test/**' 'e2e/**' >/dev/null 2>&1; then
	echo "ERROR: a JWT-shaped string appears in a source/test file:"
	git grep -nEI "eyJ[A-Za-z0-9_-]{20,}\." -- 'src/**' 'test/**' 'e2e/**' || true
	fail=1
fi

# An Entra ID refresh token ("0.A..." / "1.A...", several hundred characters)
# pasted into a tracked file -- e.g. a delegated token copied out of
# .env.sharepoint.delegated into a test.
if git grep -nEI "(^|[^A-Za-z0-9_.-])[01]\.A[A-Za-z0-9_-]{200,}" -- 'src/**' 'test/**' 'e2e/**' 'scripts/**' 'docs/**' \
	'README.md' >/dev/null 2>&1; then
	echo "ERROR: a refresh-token-shaped string appears in a tracked file:"
	git grep -nlEI "(^|[^A-Za-z0-9_.-])[01]\.A[A-Za-z0-9_-]{200,}" -- 'src/**' 'test/**' 'e2e/**' 'scripts/**' \
		'docs/**' 'README.md' || true
	fail=1
fi
for f in $(git ls-files); do
	case "$f" in *.example) ;; *) continue ;; esac
	if grep -qE '^SHAREPOINT_USER_REFRESH_TOKEN=.+' "$f"; then
		echo "ERROR: $f has a value for SHAREPOINT_USER_REFRESH_TOKEN (keep it empty)"
		fail=1
	fi
done

# An example env file must carry placeholders, never a real client secret.
if git ls-files --error-unmatch .env.sharepoint.example >/dev/null 2>&1 &&
	grep -qE '^SHAREPOINT_CLIENT_SECRET=.+' .env.sharepoint.example; then
	echo "ERROR: .env.sharepoint.example has a value for SHAREPOINT_CLIENT_SECRET (keep it empty)"
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	echo "check_no_credentials: clean"
fi
exit "$fail"
