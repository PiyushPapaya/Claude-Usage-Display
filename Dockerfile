# Container for the Claude usage server.
#
# The server reads your Claude OAuth token from ~/.claude/.credentials.json and
# (when the token is near expiry) shells out to `claude` to refresh it. Neither
# exists inside the image, so mount your host credentials in and expect the
# token to eventually expire unless you refresh it on the host:
#
#   docker build -t claude-usage .
#   docker run --rm -p 8080:8080 \
#     -v "$HOME/.claude:/root/.claude:ro" \
#     claude-usage
#
# Then open http://localhost:8080/ . Note: the auto token-refresh won't work in
# the container (no `claude` CLI); keep a logged-in `claude` on the host, or
# re-run `claude` there when you see "401 - run claude".

FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY server.py dashboard.html ./

EXPOSE 8080
ENV USAGE_HOST=0.0.0.0 USAGE_PORT=8080

CMD ["python", "server.py"]
