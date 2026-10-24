#!/usr/bin/env python3
"""Find Home Assistant (8123) and MQTT (1883) hosts on the local /24.

Usage: uv run python tools/scan_lan.py [--base 192.168.0]
"""
import argparse
import asyncio


async def probe(ip: str, port: int, sem: asyncio.Semaphore):
    async with sem:
        try:
            _, w = await asyncio.wait_for(asyncio.open_connection(ip, port), timeout=0.4)
            w.close()
            return (ip, port)
        except Exception:
            return None


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="192.168.0")
    args = parser.parse_args()

    sem = asyncio.Semaphore(256)
    tasks = [probe(f"{args.base}.{i}", p, sem) for i in range(1, 255) for p in (1883, 8123)]
    results = await asyncio.gather(*tasks)
    hits = sorted(r for r in results if r)
    for ip, port in hits:
        label = "Home Assistant" if port == 8123 else "MQTT broker"
        print(f"{ip}:{port}  OPEN  ({label})")
    if not hits:
        print(f"no MQTT(1883) or HA(8123) hosts found on {args.base}.0/24")


if __name__ == "__main__":
    asyncio.run(main())
