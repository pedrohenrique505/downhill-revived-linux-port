"""Play a full native Linux race with public pad input and read-only process observations.

The parent reads the retail finish flag/time through /proc/PID/mem; it never
writes game memory or stops the game threads.
Resize/close use the application's SDL window interface. DHD_ISO is required.
Pictures, logs, settings and saves remain under build/. --ticks is a bounded
smoke test and explicitly does not establish full-race completion.
"""
import argparse
import json
import os
import re
from pathlib import Path
import signal
import shutil
import struct
import subprocess
import time

from bench_window_linux import RACE_NAV, ROOT


def request_close(window_id):
    """Send WM_DELETE_WINDOW directly: some Xwayland WMs ignore _NET_CLOSE_WINDOW."""
    import ctypes as c
    from ctypes.util import find_library
    class Data(c.Union):
        _fields_ = [('l', c.c_long * 5)]
    class ClientMessage(c.Structure):
        _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
                    ('display', c.c_void_p), ('window', c.c_ulong), ('message_type', c.c_ulong),
                    ('format', c.c_int), ('data', Data)]
    class Event(c.Union):
        _fields_ = [('client', ClientMessage), ('pad', c.c_long * 24)]
    x = c.CDLL(find_library('X11'))
    x.XOpenDisplay.argtypes = [c.c_char_p]
    x.XOpenDisplay.restype = c.c_void_p
    x.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
    x.XInternAtom.restype = c.c_ulong
    x.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.POINTER(Event)]
    x.XFlush.argtypes = [c.c_void_p]
    x.XCloseDisplay.argtypes = [c.c_void_p]
    display = x.XOpenDisplay(None)
    if not display:
        raise RuntimeError('cannot open test X display')
    event = Event()
    event.client = ClientMessage(type=33, display=display, window=int(window_id),
                                 message_type=x.XInternAtom(display, b'WM_PROTOCOLS', 0), format=32)
    event.client.data.l[0] = x.XInternAtom(display, b'WM_DELETE_WINDOW', 0)
    try:
        if not x.XSendEvent(display, int(window_id), 0, 0, c.byref(event)):
            raise RuntimeError('cannot send close request to test window')
        x.XFlush(display)
    finally:
        x.XCloseDisplay(display)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build/port/link/dhd')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--hz', type=int, choices=(60, 120, 144), default=60)
    ap.add_argument('--ticks', type=int, help='bounded smoke instead of a full race')
    ap.add_argument('--validation', action='store_true')
    ap.add_argument('--timeout', type=float, default=1200)
    args = ap.parse_args()
    if not os.environ.get('DHD_ISO'):
        ap.error('DHD_ISO must name the local game image')
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents:
        ap.error('output must stay below build/')
    out.mkdir(parents=True, exist_ok=True)
    # Copy the executable so that rebuilding never changes an active run.
    exe = out / 'dhd'
    shutil.copy2(args.exe, exe)
    # The parent can read its child's mapped RAM without stopping any thread.
    # Use the linked symbol rather than assuming a host address for the field counter.
    symbols = subprocess.check_output(['llvm-nm', '--defined-only', str(exe)], text=True)
    field_address = int(next(line.split()[0] for line in symbols.splitlines()
                             if line.split()[-1] == 'kernel_vsync_count'), 16)
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_ISO=os.environ['DHD_ISO'], DHD_RENDER='native', DHD_SKIP_MOVIES='1',
               DHD_SETTINGS='none', DHD_MEMCARD='0', DHD_SND_OFF='1',
               SDL_VIDEODRIVER='x11', DHD_NAV=RACE_NAV, DHD_RACE_SEED='81257',
               DHD_RACE_SYNC='1', DHD_RACE_PAD='0:900:cross', DHD_SCALE='2',
               DHD_SCREENSHOT=str(out / 'race'), DHD_RACE_CAPTURE='g+600,g+6000,g+18000',
               DHD_RACE_CAPTURE_WINDOW='0', DHD_BENCH='6000-6600', DHD_TIMELINE='1', DHD_DUMP='15')
    if args.hz != 60:
        env.update(DHD_INTERP='1', DHD_INTERP_HZ=str(args.hz))
    if args.validation:
        env.update(DHD_VK_VALIDATE='1', VK_LAYER_VALIDATE_SYNC='1')
    log_path = out / 'run.log'
    result = None
    resized = restored = False
    first_finish = None
    with log_path.open('w') as log:
        process = subprocess.Popen([str(exe)], cwd=out, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        began = time.monotonic()
        try:
            memory = os.open(f'/proc/{process.pid}/mem', os.O_RDONLY)
            def read(address, fmt='I'):
                return struct.unpack('<' + fmt, os.pread(memory, struct.calcsize(fmt), address))[0]
            def window():
                ids = subprocess.run(['xdotool', 'search', '--pid', str(process.pid), '--name',
                                      'Downhill Revived'], capture_output=True, text=True).stdout.split()
                if not ids:
                    raise RuntimeError('test window not found')
                return ids[0]
            previous_field = -600
            while process.poll() is None:
                if time.monotonic() - began > args.timeout:
                    raise TimeoutError('native race timed out')
                time.sleep(0.05)
                try:
                    field = read(field_address, 'Q')
                    phase, clock = read(0x0843F348), read(0x0843EEC8)
                    finished = read(0x0843EF18, 'B')
                except (OSError, struct.error):
                    continue  # the child's RAM mapping has not been installed yet
                if field >= previous_field + 600:
                    log.write('race progress ' + json.dumps(dict(field=field, phase=phase,
                              clock=clock, finish=finished, wall_s=time.monotonic()-began)) + '\n')
                    log.flush()
                    previous_field = field
                if phase in (2, 3) and clock >= 1800 and not resized:
                    subprocess.run(['xdotool', 'windowsize', window(), '1100', '650'], check=True)
                    resized = True
                if resized and clock >= 3600 and not restored:
                    subprocess.run(['xdotool', 'windowsize', window(), '800', '600'], check=True)
                    restored = True
                complete = bool(finished and phase in (2, 3, 4))
                if complete and first_finish is None:
                    first_finish = (field, read(0x0843EEDC))
                # Include the game's normal finish-to-results transition before closing.
                done = first_finish is not None and field >= first_finish[0] + 600
                if result is None and (done or (args.ticks and phase in (2, 3, 4) and clock >= args.ticks)):
                    result = dict(full_race=first_finish is not None,
                                  finish_ticks=first_finish[1] if first_finish else None,
                                  field=field, clock=clock, resized=resized, restored=restored,
                                  wall_s=time.monotonic()-began, hz=args.hz, validation=args.validation)
                    log.write('DHD_NATIVE_RACE ' + json.dumps(result) + '\n')
                    log.flush()
                    request_close(window())
            os.close(memory)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    text = log_path.read_text()
    if process.returncode or result is None:
        raise SystemExit(f'FAIL: incomplete run or unclean close; see {log_path}')
    bench = re.search(r'bench: fields=([^ ]+) fields_per_s=([0-9.]+)', text)
    presents = re.search(r'bench interp: displayed_fps=([0-9.]+).*?interpolated=(\d+)', text)
    if bench:
        result['benchmark_fields'] = bench[1]
        result['fields_per_s'] = float(bench[2])
    if presents:
        result['displayed_fps'] = float(presents[1])
        result['interpolated_pictures'] = int(presents[2])
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if (not args.ticks and not result['full_race']) or any(s in text for s in
            ('Validation Error', 'Validation Warning', 'VUID-', 'Vulkan validation:', 'crash:')):
        raise SystemExit(f'FAIL: race/validation; see {log_path}')
    if not list(out.glob('race-*.bmp')) or 'dump: EE at kernel boundary' not in text:
        raise SystemExit('FAIL: screenshot or DHD_DUMP missing')
    if result['clock'] >= 3600 and not (result['resized'] and result['restored']):
        raise SystemExit('FAIL: X11 window resize was not exercised')
    if result['field'] > 6600 and ('bench: fields=' not in text or 'bench threads:' not in text or 'bench timeline:' not in text):
        raise SystemExit('FAIL: DHD_BENCH missing')


if __name__ == '__main__':
    main()
