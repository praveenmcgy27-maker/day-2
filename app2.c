#!/usr/bin/env python3
"""
╔══════════════════════════════════════════╗
║   Weather & System Dashboard  CLI Tool   ║
║   Libraries: requests · psutil · datetime║
╚══════════════════════════════════════════╝

Usage:
    python weather_sysinfo.py
    python weather_sysinfo.py --city "London"
    python weather_sysinfo.py --city "Tokyo" --units imperial
"""

import sys
import os
import platform
import argparse
import datetime

# ── Third-party libraries (pip install requests psutil) ──────────────────────
try:
    import requests
except ImportError:
    sys.exit("❌  Missing library: run  pip install requests")

try:
    import psutil
except ImportError:
    sys.exit("❌  Missing library: run  pip install psutil")

# ─────────────────────────────────────────────────────────────────────────────
#  ANSI colour / style helpers  (no extra deps needed)
# ─────────────────────────────────────────────────────────────────────────────

RESET  = "\033[0m"
BOLD   = "\033[1m"
DIM    = "\033[2m"

# Foreground colours
FG = {
    "black":   "\033[30m",
    "red":     "\033[91m",
    "green":   "\033[92m",
    "yellow":  "\033[93m",
    "blue":    "\033[94m",
    "magenta": "\033[95m",
    "cyan":    "\033[96m",
    "white":   "\033[97m",
    "gray":    "\033[90m",
}

# Background colours
BG = {
    "blue":    "\033[44m",
    "cyan":    "\033[46m",
    "green":   "\033[42m",
    "yellow":  "\033[43m",
    "red":     "\033[41m",
    "magenta": "\033[45m",
    "black":   "\033[40m",
    "gray":    "\033[100m",
}

def c(text, fg=None, bg=None, bold=False, dim=False):
    """Wrap text in ANSI codes."""
    code = ""
    if bold:  code += BOLD
    if dim:   code += DIM
    if fg:    code += FG.get(fg, "")
    if bg:    code += BG.get(bg, "")
    return f"{code}{text}{RESET}"


# ─────────────────────────────────────────────────────────────────────────────
#  Table renderer
# ─────────────────────────────────────────────────────────────────────────────

def render_table(title: str, rows: list[tuple[str, str]], title_color="cyan", accent="cyan"):
    """
    Render a two-column key/value table with Unicode box-drawing borders.

    rows: list of (label, value) tuples
          Use ("---", "") for a horizontal divider row.
    """
    # Measure column widths
    key_w   = max((len(r[0]) for r in rows if r[0] != "---"), default=10)
    val_w   = max((len(r[1]) for r in rows if r[0] != "---"), default=20)
    key_w   = max(key_w, 18)
    val_w   = max(val_w, 30)

    total_w = key_w + val_w + 7          # borders + padding

    # Title bar
    title_str  = f"  {title}  "
    pad_left   = (total_w - len(title_str)) // 2
    pad_right  = total_w - len(title_str) - pad_left

    top_bar    = "─" * pad_left + title_str + "─" * pad_right

    print()
    print(c(f"╭{top_bar}╮", fg=title_color, bold=True))
    print(c(f"│ {'PROPERTY':<{key_w}} │ {'VALUE':<{val_w}} │", fg="gray", bold=True))
    print(c(f"├{'─'*(key_w+2)}┼{'─'*(val_w+2)}┤", fg=title_color))

    for label, value in rows:
        if label == "---":
            print(c(f"├{'─'*(key_w+2)}┼{'─'*(val_w+2)}┤", fg="gray", dim=True))
            continue
        # Colour-code certain value ranges
        val_fg = _value_colour(label, value, accent)
        label_str = c(f" {label:<{key_w}} ", fg="white",  bold=False)
        value_str = c(f" {value:<{val_w}} ", fg=val_fg,   bold=True)
        border    = c("│", fg=title_color)
        print(f"{border}{label_str}{border}{value_str}{border}")

    print(c(f"╰{'─'*(key_w+2)}┴{'─'*(val_w+2)}╯", fg=title_color))


def _value_colour(label: str, value: str, default: str) -> str:
    """Pick a colour for a value based on heuristics."""
    lbl = label.lower()
    try:
        num = float(value.rstrip(" °%CFKmphkmhPa").split()[0])
    except (ValueError, IndexError):
        num = None

    if "cpu" in lbl or "ram" in lbl or "memory" in lbl or "disk" in lbl:
        if num is not None:
            if num >= 85:  return "red"
            if num >= 60:  return "yellow"
            return "green"

    if "temp" in lbl or "feels" in lbl:
        return "yellow"

    if "humidity" in lbl:
        return "cyan"

    if "condition" in lbl or "description" in lbl:
        return "magenta"

    return default


# ─────────────────────────────────────────────────────────────────────────────
#  Weather  (Open-Meteo — FREE, no API key needed)
# ─────────────────────────────────────────────────────────────────────────────

GEOCODE_URL = "https://geocoding-api.open-meteo.com/v1/search"
WEATHER_URL = "https://api.open-meteo.com/v1/forecast"

WMO_CODES = {
    0:  "Clear sky ☀️",
    1:  "Mainly clear 🌤️",  2: "Partly cloudy ⛅", 3: "Overcast ☁️",
    45: "Foggy 🌫️",         48: "Icy fog 🌫️",
    51: "Light drizzle 🌦️", 53: "Drizzle 🌦️",      55: "Heavy drizzle 🌧️",
    61: "Light rain 🌧️",    63: "Rain 🌧️",          65: "Heavy rain 🌧️",
    71: "Light snow 🌨️",    73: "Snow 🌨️",          75: "Heavy snow ❄️",
    80: "Rain showers 🌦️",  81: "Showers 🌧️",       82: "Violent showers ⛈️",
    95: "Thunderstorm ⛈️",  96: "Hail storm 🌩️",    99: "Heavy hail storm 🌩️",
}

def fetch_weather(city: str, units: str = "metric") -> dict:
    """
    Geocode city → fetch current weather from Open-Meteo.
    Returns a flat dict ready for display, or raises RuntimeError.
    """
    # 1. Geocode
    try:
        geo = requests.get(
            GEOCODE_URL,
            params={"name": city, "count": 1, "language": "en", "format": "json"},
            timeout=8,
        )
        geo.raise_for_status()
    except requests.exceptions.ConnectionError:
        raise RuntimeError("No internet connection. Please check your network.")
    except requests.exceptions.Timeout:
        raise RuntimeError("Geocoding API timed out. Try again later.")
    except requests.exceptions.HTTPError as e:
        raise RuntimeError(f"Geocoding API error: {e}")

    geo_data = geo.json()
    if not geo_data.get("results"):
        raise RuntimeError(f"City not found: '{city}'. Check spelling and try again.")

    loc       = geo_data["results"][0]
    lat, lon  = loc["latitude"], loc["longitude"]
    full_name = f"{loc['name']}, {loc.get('country', '')}"
    timezone  = loc.get("timezone", "UTC")

    # 2. Weather  (always fetch metric; convert if needed)
    try:
        wx = requests.get(
            WEATHER_URL,
            params={
                "latitude":             lat,
                "longitude":            lon,
                "current":              (
                    "temperature_2m,apparent_temperature,relative_humidity_2m,"
                    "wind_speed_10m,weathercode,surface_pressure"
                ),
                "temperature_unit":     "celsius",
                "wind_speed_unit":      "kmh",
                "timezone":             timezone,
            },
            timeout=8,
        )
        wx.raise_for_status()
    except requests.exceptions.ConnectionError:
        raise RuntimeError("No internet connection. Please check your network.")
    except requests.exceptions.Timeout:
        raise RuntimeError("Weather API timed out. Try again later.")
    except requests.exceptions.HTTPError as e:
        raise RuntimeError(f"Weather API error: {e}")

    curr = wx.json()["current"]

    temp    = curr["temperature_2m"]
    feels   = curr["apparent_temperature"]
    wind    = curr["wind_speed_10m"]
    humid   = curr["relative_humidity_2m"]
    press   = curr["surface_pressure"]
    wcode   = curr["weathercode"]

    # Optional unit conversion
    if units == "imperial":
        temp   = temp  * 9/5 + 32
        feels  = feels * 9/5 + 32
        wind   = wind  * 0.621371
        t_unit = "°F"
        w_unit = "mph"
    elif units == "kelvin":
        temp   = temp  + 273.15
        feels  = feels + 273.15
        t_unit = " K"
        w_unit = "km/h"
    else:
        t_unit = "°C"
        w_unit = "km/h"

    return {
        "city":        full_name,
        "lat_lon":     f"{lat:.4f}° N, {lon:.4f}° E",
        "condition":   WMO_CODES.get(wcode, f"Code {wcode}"),
        "temperature": f"{temp:.1f} {t_unit}",
        "feels_like":  f"{feels:.1f} {t_unit}",
        "humidity":    f"{humid} %",
        "wind_speed":  f"{wind:.1f} {w_unit}",
        "pressure":    f"{press:.0f} hPa",
    }


# ─────────────────────────────────────────────────────────────────────────────
#  System information  (psutil + platform + datetime)
# ─────────────────────────────────────────────────────────────────────────────

def get_system_info() -> dict:
    """Collect CPU, RAM, disk, OS, and time information."""
    # CPU
    cpu_pct     = psutil.cpu_percent(interval=0.5)
    cpu_count_l = psutil.cpu_count(logical=True)
    cpu_count_p = psutil.cpu_count(logical=False) or "?"
    try:
        cpu_freq = psutil.cpu_freq()
        freq_str = f"{cpu_freq.current:.0f} MHz" if cpu_freq else "N/A"
    except Exception:
        freq_str = "N/A"

    # RAM
    ram        = psutil.virtual_memory()
    ram_total  = ram.total  / (1024**3)
    ram_used   = ram.used   / (1024**3)
    ram_pct    = ram.percent

    # Swap
    swap       = psutil.swap_memory()
    swap_total = swap.total / (1024**3)
    swap_used  = swap.used  / (1024**3)

    # Disk  (root partition)
    try:
        disk       = psutil.disk_usage("/")
        disk_total = disk.total / (1024**3)
        disk_used  = disk.used  / (1024**3)
        disk_pct   = disk.percent
        disk_str   = f"{disk_used:.1f} GB / {disk_total:.1f} GB  ({disk_pct}%)"
    except Exception:
        disk_str = "N/A"

    # OS / platform
    os_name   = platform.system()
    os_rel    = platform.release()
    arch      = platform.machine()
    hostname  = platform.node()
    py_ver    = platform.python_version()

    # Boot time
    boot_ts   = datetime.datetime.fromtimestamp(psutil.boot_time())
    uptime    = datetime.datetime.now() - boot_ts
    h, rem    = divmod(int(uptime.total_seconds()), 3600)
    m, s      = divmod(rem, 60)
    uptime_str = f"{h}h {m}m {s}s"

    # Date / time  (stdlib datetime)
    now       = datetime.datetime.now()
    date_str  = now.strftime("%A, %d %B %Y")
    time_str  = now.strftime("%H:%M:%S")
    tz_str    = datetime.datetime.now(datetime.timezone.utc).astimezone().tzname() or "Local"

    return {
        # Time
        "date":          date_str,
        "time":          f"{time_str}  ({tz_str})",
        # CPU
        "cpu_usage":     f"{cpu_pct} %",
        "cpu_cores":     f"{cpu_count_p} physical · {cpu_count_l} logical",
        "cpu_frequency": freq_str,
        # RAM
        "ram_usage":     f"{ram_pct} %",
        "ram_used":      f"{ram_used:.2f} GB / {ram_total:.2f} GB",
        "swap":          f"{swap_used:.2f} GB / {swap_total:.2f} GB",
        # Disk
        "disk_root":     disk_str,
        # OS
        "os":            f"{os_name} {os_rel}  [{arch}]",
        "hostname":      hostname,
        "python":        py_ver,
        "uptime":        uptime_str,
    }


# ─────────────────────────────────────────────────────────────────────────────
#  Banner
# ─────────────────────────────────────────────────────────────────────────────

BANNER = r"""
  __        __         _   _             
  \ \      / /__  __ _| |_| |__   ___ _ __ 
   \ \ /\ / / _ \/ _` | __| '_ \ / _ \ '__|
    \ V  V /  __/ (_| | |_| | | |  __/ |   
     \_/\_/ \___|\__,_|\__|_| |_|\___|_|   
      +  System  Dashboard  CLI  v1.0       
"""

def print_banner():
    lines = BANNER.strip("\n").split("\n")
    colours = ["cyan","cyan","cyan","cyan","cyan","blue"]
    for i, line in enumerate(lines):
        print(c(line, fg=colours[min(i, len(colours)-1)], bold=True))
    print()


# ─────────────────────────────────────────────────────────────────────────────
#  Main
# ─────────────────────────────────────────────────────────────────────────────

def parse_args():
    parser = argparse.ArgumentParser(
        description="Display weather + system info in a pretty terminal table.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python weather_sysinfo.py\n"
            "  python weather_sysinfo.py --city 'New York'\n"
            "  python weather_sysinfo.py --city Tokyo --units imperial\n"
        )
    )
    parser.add_argument(
        "--city", "-c",
        type=str,
        default=None,
        help="City name to fetch weather for (prompts if omitted)",
    )
    parser.add_argument(
        "--units", "-u",
        choices=["metric", "imperial", "kelvin"],
        default="metric",
        help="Temperature/wind units  (default: metric)",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    # Clear screen for a fresh dashboard feel
    os.system("cls" if os.name == "nt" else "clear")

    print_banner()

    # ── Get city ──────────────────────────────────────────────────────────────
    city = args.city
    if not city:
        try:
            city = input(
                c("  Enter city name: ", fg="cyan", bold=True)
            ).strip()
        except (KeyboardInterrupt, EOFError):
            print(c("\n\n  Bye! 👋\n", fg="yellow"))
            sys.exit(0)

    if not city:
        print(c("\n  ⚠  No city entered. Exiting.\n", fg="red"))
        sys.exit(1)

    unit_label = {"metric": "°C / km/h", "imperial": "°F / mph", "kelvin": "K / km/h"}
    print(c(f"\n  📡  Fetching weather for '{city}' ({unit_label[args.units]}) …\n",
            fg="gray", dim=True))

    # ── Fetch weather ─────────────────────────────────────────────────────────
    try:
        weather = fetch_weather(city, args.units)
    except RuntimeError as err:
        print(c(f"\n  ❌  Weather Error: {err}\n", fg="red", bold=True))
        sys.exit(1)

    # ── Collect system info ───────────────────────────────────────────────────
    sysinfo = get_system_info()

    # ── Render: Date & Time ───────────────────────────────────────────────────
    dt_rows = [
        ("📅  Date",       sysinfo["date"]),
        ("🕐  Time",       sysinfo["time"]),
        ("⏱  Uptime",     sysinfo["uptime"]),
    ]
    render_table("🗓   DATE & TIME", dt_rows, title_color="magenta", accent="magenta")

    # ── Render: Weather ───────────────────────────────────────────────────────
    wx_rows = [
        ("🏙  City",        weather["city"]),
        ("📍  Coordinates", weather["lat_lon"]),
        ("---",            ""),
        ("🌤  Condition",   weather["condition"]),
        ("🌡  Temperature", weather["temperature"]),
        ("🤔  Feels Like",  weather["feels_like"]),
        ("💧  Humidity",    weather["humidity"]),
        ("💨  Wind Speed",  weather["wind_speed"]),
        ("🔵  Pressure",    weather["pressure"]),
    ]
    render_table("🌍   CURRENT WEATHER", wx_rows, title_color="cyan", accent="cyan")

    # ── Render: System ────────────────────────────────────────────────────────
    sys_rows = [
        ("🖥  OS",          sysinfo["os"]),
        ("🏠  Hostname",    sysinfo["hostname"]),
        ("🐍  Python",      sysinfo["python"]),
        ("---",            ""),
        ("⚡  CPU Usage",   sysinfo["cpu_usage"]),
        ("🔢  CPU Cores",   sysinfo["cpu_cores"]),
        ("📶  CPU Freq",    sysinfo["cpu_frequency"]),
        ("---",            ""),
        ("🧠  RAM Usage",   sysinfo["ram_usage"]),
        ("💾  RAM Used",    sysinfo["ram_used"]),
        ("🔄  Swap",        sysinfo["swap"]),
        ("---",            ""),
        ("💿  Disk (root)", sysinfo["disk_root"]),
    ]
    render_table("💻   SYSTEM INFORMATION", sys_rows, title_color="green", accent="green")

    print()
    print(c("  ✅  Dashboard ready. Data refreshed at " +
            datetime.datetime.now().strftime("%H:%M:%S"), fg="gray", dim=True))
    print()


if __name__ == "__main__":
    main()