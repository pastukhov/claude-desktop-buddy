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

Requires: aiohttp
  pip install aiohttp
"""

import argparse
import asyncio
import sys
from aiohttp import web, ClientSession, ClientTimeout, ClientError

DEFAULT_TARGET = "http://claude-buddy.local"
DEFAULT_PORT   = 9876


async def forward(request: web.Request, session: ClientSession, target: str) -> web.Response:
    url = target.rstrip("/") + request.path
    body = await request.read()
    headers = {"Content-Type": request.content_type or "application/json"}
    try:
        async with session.request(
            request.method, url,
            data=body, headers=headers,
            timeout=ClientTimeout(total=130),  # permission can block up to 120s
            allow_redirects=False,
        ) as resp:
            data = await resp.read()
            return web.Response(
                status=resp.status,
                body=data,
                content_type=resp.content_type or "application/json",
            )
    except ClientError as e:
        print(f"[proxy] error forwarding {request.method} {request.path}: {e}", flush=True)
        return web.Response(status=502, text=f"buddy unreachable: {e}")


async def main(target: str, port: int) -> None:
    timeout = ClientTimeout(total=130)
    async with ClientSession(timeout=timeout) as session:
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
