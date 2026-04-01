#!/usr/bin/env python3
"""
LXTS Analysis Tools
===================
Connect to Redis and visualise engine state,
PnL curves, latency histograms, and fill log.

Requires:
    pip install redis pandas matplotlib rich
"""

import redis
import time
import argparse
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from rich.console import Console
from rich.table import Table
from rich.live import Live
from datetime import datetime
from statistics import mean, quantiles

console = Console()

# ── Redis connection ────────────────────────────────────────

def get_redis(host="127.0.0.1", port=6379):
    r = redis.Redis(host=host, port=port, decode_responses=True)
    r.ping()
    return r

# ── Live dashboard ──────────────────────────────────────────

SYMBOLS = ["AAPL", "SPY", "QQQ", "TSLA", "NVDA"]

def make_table(r: redis.Redis) -> Table:
    table = Table(title="LXTS Engine Dashboard", style="bold")
    table.add_column("Symbol",   style="cyan",    width=8)
    table.add_column("Price",    style="white",   width=10)
    table.add_column("Position", style="yellow",  width=10)
    table.add_column("PnL",      style="green",   width=12)

    for sym in SYMBOLS:
        price = r.get(f"state:{sym}:price") or "—"
        pos   = r.get(f"state:{sym}:pos")   or "0"
        pnl   = r.get(f"state:{sym}:pnl")   or "0.00"
        pnl_f = float(pnl)
        pnl_str = f"[green]+${pnl_f:.2f}[/green]" if pnl_f >= 0 else f"[red]-${abs(pnl_f):.2f}[/red]"
        table.add_row(sym, price, pos, pnl_str)

    seq    = r.get("engine:seq")    or "—"
    status = r.get("engine:status") or "—"
    table.caption = f"seq={seq}  status={status}  {datetime.now().strftime('%H:%M:%S.%f')[:-3]}"
    return table

def run_dashboard(host, port):
    r = get_redis(host, port)
    console.print("[bold green]LXTS Dashboard[/bold green] — Ctrl-C to exit\n")
    with Live(make_table(r), refresh_per_second=5) as live:
        while True:
            live.update(make_table(r))
            time.sleep(0.2)

# ── Fill log viewer ─────────────────────────────────────────

def show_fills(r: redis.Redis, n=50):
    fills = r.lrange("fills", 0, n - 1)
    if not fills:
        fills = r.lrange("trades", 0, n - 1)
    table = Table(title=f"Last {n} Fills")
    table.add_column("#")
    table.add_column("Fill")
    for i, f in enumerate(fills):
        table.add_row(str(i + 1), f)
    console.print(table)

# ── PnL chart ───────────────────────────────────────────────

def plot_pnl(r: redis.Redis, symbol="AAPL"):
    """Animate rolling PnL from Redis by polling."""
    fig, axes = plt.subplots(2, 1, figsize=(12, 7))
    fig.patch.set_facecolor("#0a0a08")
    for ax in axes:
        ax.set_facecolor("#111109")
        ax.tick_params(colors="#a08050")
        ax.spines[:].set_color("#2a2818")

    prices, pnls, times = [], [], []

    def animate(_frame):
        p = r.get(f"state:{symbol}:price")
        q = r.get(f"state:{symbol}:pnl")
        if p and q:
            prices.append(float(p))
            pnls.append(float(q))
            times.append(len(times))

        for ax in axes: ax.clear()

        # Price
        axes[0].plot(times, prices, color="#e8a030", linewidth=1.2)
        axes[0].set_title(f"{symbol} — Mid Price", color="#e8a030")
        axes[0].set_ylabel("Price ($)", color="#a08050")

        # PnL
        color = "#4ecb71" if not pnls or pnls[-1] >= 0 else "#e05050"
        axes[1].fill_between(times, pnls, alpha=0.3, color=color)
        axes[1].plot(times, pnls, color=color, linewidth=1.2)
        axes[1].axhline(0, color="#2a2818", linewidth=0.8)
        axes[1].set_title("Realized P&L", color="#e8a030")
        axes[1].set_ylabel("P&L ($)", color="#a08050")

        for ax in axes:
            ax.set_facecolor("#111109")
            ax.tick_params(colors="#a08050")
            ax.spines[:].set_color("#2a2818")

    ani = animation.FuncAnimation(fig, animate, interval=200, cache_frame_data=False)
    plt.tight_layout()
    plt.show()

# ── Latency histogram ───────────────────────────────────────

def plot_latency_hist(r: redis.Redis):
    fills = r.lrange("fills", 0, 999)
    if not fills:
        fills = r.lrange("trades", 0, 999)
    latencies = []
    for f in fills:
        if "lat=" in f:
            try:
                lat_str = f.split("lat=")[1].replace("us", "")
                latencies.append(int(lat_str))
            except Exception:
                pass

    if not latencies:
        console.print("[red]No latency data found in Redis fills[/red]")
        return

    fig, ax = plt.subplots(figsize=(10, 5))
    fig.patch.set_facecolor("#0a0a08")
    ax.set_facecolor("#111109")
    ax.hist(latencies, bins=40, color="#e8a030", edgecolor="#0a0a08", alpha=0.85)
    p99 = quantiles(latencies, n=100)[98] if len(latencies) >= 100 else max(latencies)
    ax.axvline(mean(latencies), color="#4ecb71", linestyle="--", label=f"Mean {mean(latencies):.0f}µs")
    ax.axvline(p99, color="#e05050", linestyle="--", label=f"p99 {p99:.0f}µs")
    ax.set_title("Order Execution Latency Distribution", color="#e8a030")
    ax.set_xlabel("Latency (µs)", color="#a08050")
    ax.set_ylabel("Count", color="#a08050")
    ax.tick_params(colors="#a08050")
    ax.spines[:].set_color("#2a2818")
    ax.legend(facecolor="#111109", labelcolor="#a08050")
    plt.tight_layout()
    plt.show()

# ── CLI ─────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="LXTS Analysis Tools")
    parser.add_argument("command", choices=["dashboard", "fills", "pnl", "latency"])
    parser.add_argument("--host",   default="127.0.0.1")
    parser.add_argument("--port",   default=6379, type=int)
    parser.add_argument("--symbol", default="AAPL")
    args = parser.parse_args()

    r = get_redis(args.host, args.port)

    if args.command == "dashboard":
        run_dashboard(args.host, args.port)
    elif args.command == "fills":
        show_fills(r)
    elif args.command == "pnl":
        plot_pnl(r, args.symbol)
    elif args.command == "latency":
        plot_latency_hist(r)
