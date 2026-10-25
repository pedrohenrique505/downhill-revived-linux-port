"""Bounded Linux window benchmark using public pad navigation and read-only GDB counters.

DHD_ISO must name the player's ISO. No game memory is written by the debugger.
Counts virtual fields, display-buffer changes and the menu counter separately;
race clock ticks are not independent rendered-game-frame counts.
Example: DHD_ISO=... python port/tools/bench_window_linux.py --min-fields 15
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MENU_NAV = '0:triangle|cross,15:cross,18@4C7F38=0:start,9/5:cross'
RACE_NAV = '0:triangle|cross,15:cross,3:cross,16:cross,1:cross,11:cross,18@4C7F38=0:start,18@4C7F38=1:cross,9/5:cross'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build/port/run/dhd')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/port/bench-linux/latest')
    ap.add_argument('--first', type=int, default=420)
    ap.add_argument('--last', type=int, default=480)
    ap.add_argument('--race', action='store_true')
    ap.add_argument('--min-fields', type=float)
    ap.add_argument('--timeout', type=float, default=480)
    args = ap.parse_args()
    if not os.environ.get('DHD_ISO'):
        ap.error('DHD_ISO must name the local game image')
    if args.first < 1 or args.last <= args.first:
        ap.error('require 1 <= first < last')
    args.out.mkdir(parents=True, exist_ok=True)
    script = args.out / 'measure.gdb'
    # Absolute RAM addresses are the existing retail menu/race counters mapped
    # at PS2_RAM_BASE; all accesses below are debugger reads.
    script.write_text('''set pagination off
set confirm off
set startup-with-shell off
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGSEGV nostop noprint pass
python
import gdb,time,json
first,last = %d,%d
def read(expr): return int(gdb.parse_and_eval(expr))
def counters():
 return [read('*(unsigned int *)0x0829E10C'),read('*(unsigned int *)&gs_presented_frames'),read('*(unsigned int *)0x0843EEC8')]
class Measure(gdb.Breakpoint):
 def stop(self):
  field=read('*(unsigned long long *)&kernel_vsync_count')
  if field==first: self.start=time.monotonic(); self.initial=counters()
  if field==last:
   elapsed=time.monotonic()-self.start
   delta=[a-b for a,b in zip(counters(),self.initial)]
   result=dict(fields=last-first,wall_s=elapsed,fields_per_s=(last-first)/elapsed,menu_frames=delta[0],buffer_changes=delta[1],race_ticks=delta[2],phase=read('*(unsigned int *)0x0843F348'))
   print('DHD_BENCH_LINUX '+json.dumps(result),flush=True)
   gdb.execute('quit')
  return False
Measure('gs_vsync')
end
run
''' % (args.first, args.last))
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_ISO=os.environ['DHD_ISO'], DHD_RENDER='exact', DHD_SKIP_MOVIES='1',
               DHD_NAV=RACE_NAV if args.race else MENU_NAV, DHD_RACE_SEED='81257')
    log_path = args.out / 'run.log'
    with log_path.open('w') as log:
        process = subprocess.Popen(['gdb', '-q', '-batch', '-x', str(script.resolve()), str(args.exe.resolve())],
                                   cwd=ROOT / 'build/port/run', env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=args.timeout)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    match = re.search(r'^DHD_BENCH_LINUX (.+)$', log_path.read_text(), re.M)
    if process.returncode or not match:
        raise SystemExit(f'FAIL: incomplete benchmark; see {log_path}')
    result = json.loads(match[1])
    (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if args.race and result['phase'] not in (2, 3, 4):
        raise SystemExit('FAIL: benchmark did not reach racing')
    if not args.race and result['menu_frames'] != result['fields']:
        raise SystemExit('FAIL: menu counter did not advance once per field')
    if args.min_fields is not None and result['fields_per_s'] < args.min_fields:
        raise SystemExit(f'FAIL: below {args.min_fields} fields/s')


if __name__ == '__main__':
    main()
