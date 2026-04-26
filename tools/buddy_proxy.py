#!/usr/bin/env python3
"""
Thin HTTP proxy: localhost:9876 → ESP32 WiFi HTTP server.

Claude Code blocks HTTP hooks that resolve to private IPs, so hooks
point to localhost and this proxy forwards them to the device.

Usage:
  python3 tools/buddy_proxy.py [--target http://192.168.11.90]
  python3 tools/buddy_proxy.py [--target http://claude-buddy.local]

The target defaults to http://claude-buddy.local (mDNS). If mDNS is not
available on your system, pass the device IP explicitly.

Fallback when buddy is unreachable:
  - /permission  → native GUI dialog (kdialog → zenity → console fallback)
  - all others   → 502 (async hooks, silently ignored by Claude Code)

Requires: aiohttp
  pip install aiohttp
"""

import argparse
import asyncio
import shutil
import sys
from aiohttp import web, ClientSession, ClientTimeout, ClientError

DEFAULT_TARGET   = "http://claude-buddy.local"
DEFAULT_PORT     = 9876
CONNECT_TIMEOUT  = 4.0    # fail fast when buddy is off the network
PERMISSION_READ  = 130.0  # /permission blocks until user taps (up to 120s)
OTHER_READ       = 15.0   # generous for normal hooks

ALLOW = '{"decision":{"behavior":"allow"}}'
DENY  = '{"decision":{"behavior":"deny"}}'


async def _dialog_permission(tool: str, hint: str) -> str:
    """Show a native GUI dialog for permission. Returns ALLOW or DENY JSON."""
    title = "Claude — разрешить?"
    msg   = f"Разрешить инструмент: {tool}"
    if hint:
        msg += f"\n{hint}"

    # Try kdialog (KDE)
    if shutil.which("kdialog"):
        proc = await asyncio.create_subprocess_exec(
            "kdialog", "--yesno", msg, "--title", title,
        )
        code = await proc.wait()
        return ALLOW if code == 0 else DENY

    # Try zenity (GNOME / generic GTK)
    if shutil.which("zenity"):
        proc = await asyncio.create_subprocess_exec(
            "zenity", "--question",
            "--title", title,
            "--text", msg,
            "--ok-label", "Разрешить",
            "--cancel-label", "Отклонить",
        )
        code = await proc.wait()
        return ALLOW if code == 0 else DENY

    # Last resort: terminal dialog via stdin (blocks the proxy event loop,
    # but only when buddy is already unreachable so there's no better option)
    print(f"\n[proxy] buddy недоступен — локальный запрос разрешения", flush=True)
    print(f"  Инструмент: {tool}", flush=True)
    if hint:
        print(f"  Команда:    {hint}", flush=True)
    try:
        answer = await asyncio.get_event_loop().run_in_executor(
            None, lambda: input("  Разрешить? [y/N]: ")
        )
        return ALLOW if answer.strip().lower() in ("y", "yes", "д", "да") else DENY
    except EOFError:
        return DENY


def _parse_json_field(body: bytes, key: str) -> str:
    """Minimal JSON field extractor — avoids a full json import for a tiny payload."""
    import json
    try:
        d = json.loads(body)
        v = d.get(key, "")
        if isinstance(v, dict):
            # tool_input — grab first string value as hint
            for val in v.values():
                if isinstance(val, str):
                    return val[:120]
            return ""
        return str(v)[:120]
    except Exception:
        return ""


async def forward(request: web.Request, session: ClientSession, target: str) -> web.Response:
    is_permission = request.path.rstrip("/") == "/permission"
    read_timeout  = PERMISSION_READ if is_permission else OTHER_READ

    url  = target.rstrip("/") + request.path
    body = await request.read()
    headers = {"Content-Type": request.content_type or "application/json"}

    try:
        timeout = ClientTimeout(connect=CONNECT_TIMEOUT, sock_read=read_timeout)
        async with session.request(
            request.method, url,
            data=body, headers=headers,
            timeout=timeout,
            allow_redirects=False,
        ) as resp:
            data = await resp.read()
            return web.Response(
                status=resp.status,
                body=data,
                content_type=resp.content_type or "application/json",
            )
    except ClientError as e:
        print(f"[proxy] buddy unreachable ({request.path}): {e}", flush=True)
        if is_permission:
            tool = _parse_json_field(body, "tool_name")
            hint = _parse_json_field(body, "tool_input")
            print(f"[proxy] /permission fallback → GUI dialog (tool={tool!r})", flush=True)
            decision = await _dialog_permission(tool, hint)
            return web.Response(status=200, text=decision, content_type="application/json")
        return web.Response(status=502, text=f"buddy unreachable: {e}")


async def main(target: str, port: int) -> None:
    async with ClientSession(timeout=ClientTimeout(total=PERMISSION_READ + 10)) as session:
        app = web.Application()
        app.router.add_route("*", "/{path_info:.*}",
                             lambda req: forward(req, session, target))

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", port)
        await site.start()
        print(f"[proxy] listening on http://127.0.0.1:{port} → {target}", flush=True)
        await asyncio.Event().wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", default=DEFAULT_TARGET,
                        help=f"ESP32 HTTP server URL (default: {DEFAULT_TARGET})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"Local port to listen on (default: {DEFAULT_PORT})")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.target, args.port))
    except KeyboardInterrupt:
        sys.exit(0)
