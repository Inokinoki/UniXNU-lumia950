"""Authenticate GitHub API requests made by the build scripts on CI.

`scripts/prepare-xnu-source.py` asks api.github.com for the DriverKit and TrustCache
directory listings. Unauthenticated calls are rate limited per IP, and shared runner
IPs are usually over the limit, so the kernel step dies with `HTTP Error 403: rate
limit exceeded` before it compiles anything.

Python imports `sitecustomize` at startup when it is on PYTHONPATH, so this file is
picked up by every `python3` the build invokes, including the script run by
`build-xnu-4k-baseline.sh`. The credential is added only for api.github.com requests
that do not already carry one, and it is never written anywhere.

Without GITHUB_TOKEN this module does nothing, so local builds are unaffected.
"""
import os
import urllib.parse
import urllib.request

_token = os.environ.get("GITHUB_TOKEN")

if _token:
    _original_urlopen = urllib.request.urlopen

    def _authenticated_urlopen(url, *args, **kwargs):
        request = url if isinstance(url, urllib.request.Request) else urllib.request.Request(url)
        try:
            host = urllib.parse.urlsplit(request.full_url).hostname
        except ValueError:
            host = None
        if host == "api.github.com" and not request.has_header("Authorization"):
            request.add_header("Authorization", f"Bearer {_token}")
        return _original_urlopen(request, *args, **kwargs)

    urllib.request.urlopen = _authenticated_urlopen
