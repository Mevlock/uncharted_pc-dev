#!/usr/bin/env python3
"""Adds a measured run to docs/perf/fps_history.md.

    fps_history.py <run log>... --build 0.3.6.13-rc --commit 50c6f46 --scene "U3 Cartagena square" \
        [--settings "native, DLSS, FG off, no cap"] [--note "..."] [--samples 6] [--dry-run]

Each log is a game log (user/last_run.log) from a run with BB_FRAME_STATS=1: its "Frame stats"
lines are one per second. The row gets the mean, min and max of the last <samples> of each log (the
steady part, after the scene has settled), all logs together: pass one log per round.
"""
import argparse
import datetime
import pathlib
import re
import statistics
import sys

HISTORY = pathlib.Path(__file__).resolve().parent.parent / 'docs' / 'perf' / 'fps_history.md'
TABLE_HEADER = '| Date | Build | Commit | Scene | Settings | Mean FPS | Min–max | Note |'


def frame_rates(log_path):
    text = pathlib.Path(log_path).read_text(encoding='utf-8', errors='replace')
    return [float(m) for m in re.findall(r'^Frame stats: ([0-9.]+) FPS', text, re.MULTILINE)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--build', required=True)
    ap.add_argument('--commit', default='')
    ap.add_argument('--scene', required=True)
    ap.add_argument('--settings', default='native, DLSS, FG off, no cap')
    ap.add_argument('--note', default='')
    ap.add_argument('--samples', type=int, default=6)
    ap.add_argument('--date', default=datetime.date.today().isoformat())
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    steady = []
    for log in args.logs:
        rates = frame_rates(log)
        if len(rates) < args.samples:
            sys.exit(f'{log}: only {len(rates)} Frame stats lines, need {args.samples}')
        steady += rates[-args.samples:]
    row = (f'| {args.date} | {args.build} | {args.commit} | {args.scene} | {args.settings} | '
           f'{statistics.mean(steady):.1f} | {min(steady):.1f}–{max(steady):.1f} | {args.note} |')
    if args.dry_run:
        print(row)
        return
    text = HISTORY.read_text(encoding='utf-8')
    if TABLE_HEADER not in text:
        sys.exit(f'{HISTORY}: table header not found')
    HISTORY.write_text(text.rstrip('\n') + '\n' + row + '\n', encoding='utf-8')
    print(row)


if __name__ == '__main__':
    main()
