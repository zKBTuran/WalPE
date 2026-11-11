#!/usr/bin/env python3
"""optcheck.py - WalPE user-property coverage harness (scenes).

For every scene wallpaper: start a private Xvfb + walpe-daemon with
WALPE_PROP_PROBE set, open the wallpaper, and for every user property change
its value through the daemon's IPC (the path walpe-ctl and the GUI use),
wait for the reload and compare the engine's state digest (propprobe.h)
with the digest taken with the defaults. An option is EFFECTIVE when the
digest changes; otherwise the reason is derived from where scene.json binds
the property and from what the engine reported reading.

usage: optcheck.py --build <build dir> --out <dir> [--ids a,b] [--jobs N]
       optcheck.py --report <dir> [<dir2>]      (coverage table from results)
Run with python3 -I, WALPE_SCRATCH=<dir with wallpapers/items/<id>> (default: the current
directory). Reads wallpapers as data only (no eval of their text).
"""
import argparse, collections, glob, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time

# WALPE_SCRATCH: the directory that holds wallpapers/items/<id> (the wallpapers to check)
# and, optionally, we/install/assets (a Wallpaper Engine install's assets); run dirs go to
# <WALPE_SCRATCH>/run. Default: the current directory.
S = os.environ.get('WALPE_SCRATCH') or os.getcwd()
ITEMS = os.path.join(S, 'wallpapers', 'items')
ASSETS = os.path.join(S, 'we', 'install', 'assets')


def load_json(p):
    with open(p, 'rb') as f:
        return json.loads(f.read().decode('utf-8-sig', 'replace'))


# ---------------------------------------------------------------- pkg (text entries only)
def pkg_text(pkg):
    """{path: text} of the json entries of a scene.pkg"""
    out = {}
    with open(pkg, 'rb') as f:
        mlen = struct.unpack('<I', f.read(4))[0]
        if mlen != 8 or not f.read(8).startswith(b'PKGV'):
            return out
        n = struct.unpack('<I', f.read(4))[0]
        ents = []
        for _ in range(min(n, 100000)):
            pl = struct.unpack('<I', f.read(4))[0]
            if pl > 1024:
                return out
            path = f.read(pl).decode('utf-8', 'replace')
            off, sz = struct.unpack('<II', f.read(8))
            ents.append((path, off, sz))
        base = f.tell()
        for path, off, sz in ents:
            if path.lower().endswith('.json') and sz < 32 << 20:
                f.seek(base + off)
                out[path] = f.read(sz).decode('utf-8-sig', 'replace')
    return out


# ---------------------------------------------------------------- conditions (safe evaluator)
class Cond:
    """WE property conditions: a.value, !, ==, !=, <, >, <=, >=, &&, ||, ()"""
    TOK = re.compile(r'\s*(===|!==|==|!=|<=|>=|&&|\|\||[()!<>?:=]|"[^"]*"|\'[^\']*\'|-?\d+\.?\d*|[A-Za-z_$][\w$]*(?:\s*\.\s*\w+)?)')

    def __init__(self, text):
        self.toks = []
        self.ok = True
        pos = 0
        text = text or ''
        while pos < len(text):
            if text[pos:].strip() == '':
                break
            m = self.TOK.match(text, pos)
            if not m:
                self.ok = False
                break
            self.toks.append(m.group(1))
            pos = m.end()
        self.toks = [re.sub(r'\s+', '', t) if re.match(r'[A-Za-z_$]', t) else t for t in self.toks]
        self.refs = [t.split('.')[0] for t in self.toks
                     if re.match(r'[A-Za-z_$]', t) and t not in ('true', 'false', 'null', 'undefined')]

    def eval(self, vals):
        if not self.ok or not self.toks:
            return True
        self.i = 0
        self.vals = vals
        try:
            v = self._expr()
            if self.i != len(self.toks):
                return True
            return self._truthy(v)
        except Exception:
            return True

    def _peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    @staticmethod
    def _truthy(v):
        if v is None:
            return False
        if isinstance(v, str):
            return v != ''
        return bool(v)

    @staticmethod
    def _num(v):
        if isinstance(v, bool):
            return float(v)
        if isinstance(v, (int, float)):
            return float(v)
        if isinstance(v, str):
            try:
                return float(v)
            except ValueError:
                return None
        return None

    def _expr(self):
        a = self._or()
        if self._peek() != '?':
            return a
        self.i += 1
        x = self._expr()
        if self._peek() != ':':
            raise ValueError
        self.i += 1
        y = self._expr()
        return x if self._truthy(a) else y

    def _or(self):
        a = self._and()
        while self._peek() == '||':
            self.i += 1
            b = self._and()
            a = self._truthy(a) or self._truthy(b)
        return a

    def _and(self):
        a = self._cmp()
        while self._peek() == '&&':
            self.i += 1
            b = self._cmp()
            a = self._truthy(a) and self._truthy(b)
        return a

    def _cmp(self):
        a = self._unary()
        while self._peek() in ('==', '!=', '===', '!==', '<', '>', '<=', '>='):
            op = self.toks[self.i]
            self.i += 1
            b = self._unary()
            if op in ('==', '!=', '===', '!=='):
                if a is None or b is None:
                    r = a is None and b is None
                elif isinstance(a, str) and isinstance(b, str):
                    r = a == b
                else:
                    x, y = self._num(a), self._num(b)
                    r = x is not None and y is not None and x == y
                if op.startswith('!'):
                    r = not r
            else:
                x, y = self._num(a), self._num(b)
                r = False if x is None or y is None else {'<': x < y, '>': x > y, '<=': x <= y, '>=': x >= y}[op]
            a = r
        return a

    def _unary(self):
        if self._peek() == '!':
            self.i += 1
            return not self._truthy(self._unary())
        return self._prim()

    def _prim(self):
        t = self._peek()
        if t is None:
            raise ValueError
        self.i += 1
        if t == '(':
            v = self._expr()
            if self._peek() != ')':
                raise ValueError
            self.i += 1
            return v
        if t[0] in '"\'':
            return t[1:-1]
        if re.match(r'-?\d', t):
            return float(t)
        if t == 'true':
            return True
        if t == 'false':
            return False
        if t in ('null', 'undefined'):
            return None
        name, _, member = t.partition('.')
        v = None if member and member != 'value' else self.vals.get(name)
        if self._peek() == '=':        # x.text = 'label': the right side
            self.i += 1
            v = self._expr()
        return v


# ---------------------------------------------------------------- daemon
class Daemon:
    def __init__(self, build, size='640x360', extra_env=None, probe=True):
        self.build = build
        os.makedirs(os.path.join(S, 'run'), exist_ok=True)
        self.R = tempfile.mkdtemp(prefix='oc.', dir=os.path.join(S, 'run'))
        for d in ('rt', 'cfg', 'cache', 'data', 'home', 'probe'):
            os.makedirs(os.path.join(self.R, d))
        os.chmod(os.path.join(self.R, 'rt'), 0o700)
        self.probe = os.path.join(self.R, 'probe')
        env = dict(os.environ)
        env.update(HOME=self.R + '/home', XDG_RUNTIME_DIR=self.R + '/rt', XDG_CONFIG_HOME=self.R + '/cfg',
                   XDG_CACHE_HOME=self.R + '/cache', XDG_DATA_HOME=self.R + '/data', LIBGL_ALWAYS_SOFTWARE='1',
                   WALPE_NO_AUDIO='1')
        if probe:
            env['WALPE_PROP_PROBE'] = self.probe
        if os.path.isdir(ASSETS + '/shaders'):
            env['WALPE_WE_ASSETS'] = ASSETS
        env.update(extra_env or {})
        self.env = env
        r, w = os.pipe()
        # -s 0: no X screen saver (it blanks after 10 minutes and the daemon's
        # pause_dpms then stops rendering: no more digests)
        self.xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(w), '-screen', '0', size + 'x24', '-nolisten', 'tcp', '-noreset', '-s', '0'],
                                     pass_fds=[w], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(w)
        disp = b''
        t0 = time.time()
        while not disp.endswith(b'\n') and time.time() - t0 < 10:
            disp += os.read(r, 16)
        os.close(r)
        env['DISPLAY'] = ':' + disp.decode().strip()
        # (this environment is the caller's: inside a Wayland session the daemon, left to
        # choose, would draw on the real desktop instead of the X server started here)
        env['WALPE_BACKEND'] = 'x11'
        for k in ('WAYLAND_DISPLAY', 'WAYLAND_SOCKET'):
            env.pop(k, None)
        self.log = open(os.path.join(self.R, 'daemon.log'), 'wb')
        self.proc = subprocess.Popen([os.path.join(build, 'walpe-daemon')], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = os.path.join(self.R, 'rt', 'walpe.sock')
        t0 = time.time()
        while time.time() - t0 < 15:
            if os.path.exists(self.sock):
                try:
                    if self.req({'cmd': 'status'}).get('ok'):
                        break
                except OSError:
                    pass
            time.sleep(0.1)
        self.seq = 0
        try:
            self.req({'cmd': 'set', 'pause_dpms': False, 'pause_covered': False, 'pause_fullscreen': False})
        except Exception:
            pass

    def req(self, obj, timeout=20):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(self.sock)
        s.sendall((json.dumps(obj) + '\n').encode())
        buf = b''
        while not buf.endswith(b'\n'):
            d = s.recv(65536)
            if not d:
                break
            buf += d
        s.close()
        return json.loads(buf.decode() or '{}')

    def alive(self):
        return self.proc.poll() is None

    def wait_digest(self, timeout=40):
        """the next digest file (the engine numbers them per process)"""
        want = self.seq + 1
        t0 = time.time()
        while time.time() - t0 < timeout:
            fs = glob.glob(os.path.join(self.probe, '*.%d.json' % want))
            if fs:
                self.seq = want
                try:
                    return load_json(fs[0])
                finally:
                    os.unlink(fs[0])
            if not self.alive():
                return None
            time.sleep(0.03)
        return None

    def close(self, keep_log=None):
        try:
            self.req({'cmd': 'quit'}, timeout=3)
        except Exception:
            pass
        for p in (self.proc, self.xvfb):
            try:
                p.wait(timeout=4)
            except Exception:
                p.terminate()
                try:
                    p.wait(timeout=3)
                except Exception:
                    p.kill()
        self.log.close()
        if keep_log:
            shutil.copy(os.path.join(self.R, 'daemon.log'), keep_log)
        shutil.rmtree(self.R, ignore_errors=True)


# ---------------------------------------------------------------- digest diff
def flat(d, pre=''):
    out = {}
    if isinstance(d, dict):
        for k, v in d.items():
            out.update(flat(v, pre + '/' + str(k)))
    elif isinstance(d, list):
        for i, v in enumerate(d):
            out.update(flat(v, pre + '/' + str(i)))
    else:
        out[pre] = d
    return out


def state_of(dig):
    d = dict(dig)
    d.pop('uses', None)
    d.pop('frame', None)
    d.pop('origin', None)
    f = flat(d)
    for k, v in f.items():
        # a video texture's size follows the decoder (half-size decode starts
        # after the first frames): timing, not state
        if k.endswith('/tex') and isinstance(v, str) and v.endswith(' video'):
            f[k] = re.sub(r' \d+x\d+ video$', ' video', v)
    return f


def diff_keys(a, b):
    ks = set(a) | set(b)
    return {k for k in ks if a.get(k, '\0missing') != b.get(k, '\0missing')}


# ---------------------------------------------------------------- static bindings
FEATURES = [
    (re.compile(r'^\.general\.cameraparallax|parallaxDepth'), 'mouse parallax (camera parallax / layer parallaxDepth)'),
    (re.compile(r'^\.general\.bloom'), 'bloom (HDR glow post-process)'),
    (re.compile(r'^\.general\.camerashake'), 'camera shake'),
    (re.compile(r'\(sound\)'), 'scene sound layers (volume / playback)'),
    (re.compile(r'^\.general\.clearcolor'), 'scene clear colour'),
    (re.compile(r'^\.general\.(zoom|fov|nearz|farz|orthogonalprojection)'), 'camera zoom / projection in general'),
    (re.compile(r'^\.general\.(ambientcolor|skylightcolor|hdr|gravity|wind)'), 'lighting / physics general settings'),
    (re.compile(r'instanceoverride\.[a-z]+\.scriptproperties|instanceoverride\.[a-z]+\?script'), 'scripted particle instance overrides'),
    (re.compile(r'^<materials>.*constantshadervalues'), 'custom layer shaders (constants of an image layer\'s own material)'),
]


def binding_sites(scene, texts):
    """prop -> set of generic JSON paths that bind it"""
    refs = collections.defaultdict(set)

    def gen(path):
        p = re.sub(r'\[\d+\]', '[]', path)
        p = re.sub(r'constantshadervalues\.[^.]+', 'constantshadervalues.*', p)
        p = re.sub(r'scriptproperties\.[^.]+', 'scriptproperties.*', p)
        return p

    def walk(o, path, obj_kind):
        if isinstance(o, dict):
            u = o.get('user')
            if isinstance(u, str):
                refs[u].add(gen(path) + obj_kind)
            elif isinstance(u, dict) and isinstance(u.get('name'), str):
                refs[u['name']].add(gen(path) + '?condition' + obj_kind)
            for k, v in o.items():
                kind = obj_kind
                if path == '.objects[]' or re.fullmatch(r'\.objects\[\d+\]', path):
                    kind = ' (sound)' if 'sound' in o else ' (light)' if 'light' in o else ''
                if k == 'usertextures' and isinstance(v, list):
                    for e in v:
                        if isinstance(e, str):
                            refs[e].add(gen(path) + '.usertextures' + kind)
                walk(v, path + '.' + k, kind)
        elif isinstance(o, list):
            for i, v in enumerate(o):
                walk(v, path + '[%d]' % i, obj_kind)

    walk(scene, '', '')
    for path, txt in texts.items():
        if path == 'scene.json' or not path.endswith('.json'):
            continue
        try:
            j = json.loads(txt)
        except Exception:
            continue
        pre = '<' + path.split('/')[0] + '>'

        def walk2(o, p):
            if isinstance(o, dict):
                u = o.get('user')
                if isinstance(u, str):
                    refs[u].add(pre + gen(p))
                for k, v in o.items():
                    if k == 'usertextures' and isinstance(v, list):
                        for e in v:
                            if isinstance(e, str):
                                refs[e].add(pre + gen(p) + '.usertextures')
                    walk2(v, p + '.' + k)
            elif isinstance(o, list):
                for i, v in enumerate(o):
                    walk2(v, p + '[%d]' % i)
        walk2(j, '')
    return refs


def feature_of(sites):
    for s in sorted(sites):
        for rx, name in FEATURES:
            if rx.search(s):
                return name
    return None


# ---------------------------------------------------------------- alternates
def alternates(key, d, testfile):
    t = (d.get('type') or '').lower()
    v = d.get('value')
    if t in ('bool', 'checkbox'):
        return [not bool(v)]
    if t == 'slider':
        try:
            mn, mx, cur = float(d.get('min', 0)), float(d.get('max', 100)), float(v if v is not None else 0)
        except (TypeError, ValueError):
            return []
        if mx <= mn:
            mx = mn + 1
        far = mx if (mx - cur) >= (cur - mn) else mn
        near = mn if far == mx else mx
        mid = (cur + far) / 2
        isint = not d.get('fraction', True)
        outs = []
        for a in (far, mid, near):
            a = int(round(a)) if isint else round(a, 4)
            if a != cur and a not in outs:
                outs.append(a)
        return outs[:3]
    if t == 'combo':
        outs = []
        for o in d.get('options') or []:
            if isinstance(o, dict) and 'value' in o and str(o['value']) != str(v) and o['value'] not in outs:
                outs.append(o['value'])
        return outs[:4]
    if t == 'color':
        s = str(v or '')
        return ['0 1 0', '1 0 0'] if s.strip().startswith('1 0 0') else ['1 0 0', '0 0 1']
    if t in ('textinput', 'text'):
        return [(str(v if v is not None else '') + ' WalPE 12').strip(), '7']
    if t in ('file', 'scenetexture'):
        return [testfile]
    if t == 'directory':
        return [os.path.dirname(testfile)]
    return []


def is_option(d):
    # "group", "text" and typeless entries are headers / notes in WE
    # whatever their "value" says
    t = (d.get('type') or '').lower()
    return t not in ('group', 'text', '')


# ---------------------------------------------------------------- one scene
SLOW_FRAME = 45     # tier 2: digest after 1.5 s of scene time


def check_scene(wid, build, outdir, testfile, prev=None):
    """prev = an earlier result for this wallpaper: only re-check (tier 2,
    late digest) the options it found read but without a state change"""
    path = os.path.join(ITEMS, wid)
    pj = load_json(os.path.join(path, 'project.json'))
    props = (pj.get('general') or {}).get('properties') or {}
    props = {k: v for k, v in props.items() if isinstance(v, dict)}
    preset_of = None
    if 'dependency' in pj and not pj.get('file') and not os.path.exists(os.path.join(path, 'scene.pkg')):
        preset_of = str(pj['dependency'])
        base = os.path.join(ITEMS, preset_of)
        bpj = load_json(os.path.join(base, 'project.json'))
        props = {k: v for k, v in ((bpj.get('general') or {}).get('properties') or {}).items() if isinstance(v, dict)}
        for k, v in (pj.get('preset') or {}).items():
            if k in props and not isinstance(v, (dict, list)) and v is not None:
                props[k] = dict(props[k], value=v)
    pkg = os.path.join(ITEMS, preset_of or wid, 'scene.pkg')
    texts = pkg_text(pkg) if os.path.isfile(pkg) else {}
    scene = {}
    if 'scene.json' in texts:
        scene = json.loads(texts['scene.json'])
    elif os.path.isfile(os.path.join(ITEMS, preset_of or wid, 'scene.json')):
        scene = load_json(os.path.join(ITEMS, preset_of or wid, 'scene.json'))
        texts = {'scene.json': json.dumps(scene)}
    sites = binding_sites(scene, texts)
    script_text = '\n'.join(texts.values())
    conds = {k: Cond(v.get('condition')) for k, v in props.items() if v.get('condition')}
    cond_users = collections.defaultdict(set)
    for k, c in conds.items():
        for r in c.refs:
            if r != k:
                cond_users[r].add(k)
    defaults = {k: v.get('value') for k, v in props.items()}

    res = {'id': wid, 'title': pj.get('title', ''), 'type': 'scene' + (' (preset of %s)' % preset_of if preset_of else ''),
           'props': {}, 'error': None}
    if prev:
        res = prev
        res['error'] = None
    D = Daemon(build)
    wait = 180 if prev else 60          # the late digest needs 45 frames: slow on a loaded box
    try:
        if prev:
            with open(os.path.join(D.probe, 'frame'), 'w') as f:
                f.write(str(SLOW_FRAME))
        r = D.req({'cmd': 'open', 'path': path})
        base = D.wait_digest(wait)
        if not base:
            res['error'] = 'no digest after open (%s)' % json.dumps(r)[:200]
            return res
        uses = base.get('uses', {})
        D.req({'cmd': 'open', 'path': path})
        base2 = D.wait_digest(wait)
        volatile = set()
        b0 = state_of(base)
        if base2:
            volatile |= diff_keys(b0, state_of(base2))
        if not prev:
            res['nodes'] = len(base.get('nodes', []))
            res['volatile'] = len(volatile)
        cur_over = {}                       # overrides in effect (key -> value)

        def apply(over):
            """make exactly `over` the non-default values; returns the digest"""
            send = {}
            for k in list(cur_over):
                if k not in over:
                    send[k] = defaults[k] if defaults[k] is not None else ''
                    del cur_over[k]
            for k, v in over.items():
                if cur_over.get(k, '\0') != v:
                    send[k] = v
                    cur_over[k] = v
            if not send:
                return None
            D.req({'cmd': 'setprops', 'path': path, 'props': send})
            return D.wait_digest(wait)

        n = 0
        ctx_base = {}                       # frozenset(ctx items) -> state
        order = sorted(props, key=lambda k: (props[k].get('order', 1e9), k))
        if prev:
            # tier 2 only: what was read (or named in a script) without effect
            order = [k for k in order if k in res['props'] and not res['props'][k]['effective'] and
                     res['props'][k].get('how') != 'label' and
                     (res['props'][k]['uses'] or res['props'][k]['in_script'])]
        state_tag = 'state@%.1fs' % (SLOW_FRAME / 30.0) if prev else 'state'
        for key in order:
            d = props[key]
            if prev:
                ent = res['props'][key]
            else:
                ent = {'type': d.get('type', ''), 'text': d.get('text', ''), 'sites': sorted(sites.get(key, [])),
                       'uses': uses.get(key, []), 'cond_users': sorted(cond_users.get(key, [])),
                       'in_script': bool(re.search(r'(?<![A-Za-z0-9_])' + re.escape(key) + r'(?![A-Za-z0-9_])', script_text)),
                       'effective': False, 'how': '', 'changed': []}
                res['props'][key] = ent
            if not is_option(d):
                ent['how'] = 'label'
                continue
            alts = alternates(key, d, testfile)
            if not alts:
                ent['how'] = 'no-alternate'
                continue
            n += 1
            if n % 12 == 0:                 # refresh the baseline: catches slow drift
                bx = apply({})
                if bx:
                    volatile |= diff_keys(b0, state_of(bx))
            for a in alts:
                dg = apply({key: a})
                if not dg:
                    if not D.alive():
                        ent['how'] = 'daemon died'
                        res['error'] = 'daemon died at %s=%r' % (key, a)
                        return res
                    continue
                for u, sl in dg.get('uses', {}).items():
                    if u == key:
                        ent['uses'] = sorted(set(ent['uses']) | set(sl))
                ch = diff_keys(b0, state_of(dg)) - volatile
                if ch:
                    ent.update(effective=True, how=state_tag, alt=a, changed=sorted(ch)[:6], nchanged=len(ch))
                    break
            if ent['effective']:
                continue
            # in the context that makes it relevant: its own condition true,
            # and (for a bound one that did nothing) the options it hangs on
            ctxs = []
            c = conds.get(key)
            if c and c.ok and not c.eval(defaults):
                ctx = solve(c, props, defaults)
                if ctx:
                    ctxs.append(ctx)
            for ctx in ctxs:
                fk = frozenset((k, str(v)) for k, v in ctx.items())
                if fk not in ctx_base:
                    dg = apply(dict(ctx))
                    ctx_base[fk] = state_of(dg) if dg else None
                cb = ctx_base[fk]
                if cb is None:
                    continue
                for a in alts:
                    dg = apply(dict(ctx, **{key: a}))
                    if not dg:
                        continue
                    for u, sl in dg.get('uses', {}).items():
                        if u == key:
                            ent['uses'] = sorted(set(ent['uses']) | set(sl))
                    ch = diff_keys(cb, state_of(dg)) - volatile
                    if ch:
                        ent.update(effective=True, how='%s (with %s)' % (state_tag, ', '.join('%s=%s' % kv for kv in ctx.items())),
                                   alt=a, changed=sorted(ch)[:6], nchanged=len(ch))
                        break
                if ent['effective']:
                    break
        # final baseline: anything that drifted meanwhile is volatile
        bx = apply({})
        if bx:
            late = diff_keys(b0, state_of(bx))
            if late - volatile:
                volatile |= late
                for key, ent in res['props'].items():
                    if prev and not ent.get('how', '').startswith(state_tag):
                        continue    # tier-1 findings were checked in their own run
                    if ent['effective'] and set(ent['changed']) <= volatile and ent.get('nchanged', 0) <= len(ent['changed']):
                        ent.update(effective=False, how='only volatile fields changed')
        res['volatile' + ('2' if prev else '')] = len(volatile)
    finally:
        D.close(keep_log=os.path.join(outdir, wid + '.daemon.log'))
    classify(res)
    return res


# ---------------------------------------------------------------- web wallpapers
def web_sources(path):
    """the page's own text (js / html / css), for 'does the page name this option?'"""
    out = []
    for root, _, fs in os.walk(path):
        for f in fs:
            if f.lower().endswith(('.js', '.html', '.htm', '.css', '.vue', '.mjs')):
                fp = os.path.join(root, f)
                try:
                    if os.path.getsize(fp) < 24 << 20:
                        with open(fp, 'rb') as fh:
                            out.append(fh.read().decode('utf-8', 'replace'))
                except OSError:
                    pass
    return '\n'.join(out)


def shape_ok(d, v):
    """is v what a WE page expects for this declared type?"""
    t = (d.get('type') or '').lower()
    if t in ('bool', 'checkbox'):
        return isinstance(v, bool)
    if t == 'slider':
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == 'color':
        return isinstance(v, str) and len(v.split()) == 3 and all(re.fullmatch(r'-?\d*\.?\d+(e-?\d+)?', x) for x in v.split())
    if t == 'combo':
        opts = [o.get('value') for o in d.get('options') or [] if isinstance(o, dict)]
        # JS has one number type (2.0 === 2), but "2" is not 2 for === / switch
        def same(o, x):
            if isinstance(o, bool) or isinstance(x, bool):
                return o is x
            if isinstance(o, (int, float)) and isinstance(x, (int, float)):
                return o == x
            return type(o) is type(x) and o == x
        return any(same(o, v) for o in opts) or (not opts) or same(d.get('value'), v)
    if t in ('textinput',):
        return isinstance(v, str)
    if t in ('file', 'directory'):
        return isinstance(v, str) and not v.startswith('file:')
    return True


def read_weblog(D, wid, since):
    f = os.path.join(D.probe, 'web.%s.log' % wid)
    if not os.path.exists(f):
        return []
    with open(f, 'rb') as fh:
        lines = fh.read().decode('utf-8', 'replace').splitlines()
    out = []
    for ln in lines[since:]:
        try:
            out.append(json.loads(ln, strict=False))
        except Exception:
            out.append({'kind': 'bad', 'data': ln[:200]})
    return out


def check_web(wid, build, outdir, testfile):
    path = os.path.join(ITEMS, wid)
    pj = load_json(os.path.join(path, 'project.json'))
    base = path
    props = (pj.get('general') or {}).get('properties') or {}
    preset_of = None
    if 'dependency' in pj and not pj.get('file'):
        preset_of = str(pj['dependency'])
        base = os.path.join(ITEMS, preset_of)
        props = (load_json(os.path.join(base, 'project.json')).get('general') or {}).get('properties') or {}
    props = {k: v for k, v in props.items() if isinstance(v, dict)}
    if preset_of:       # the preset's values are this wallpaper's defaults
        for k, v in (pj.get('preset') or {}).items():
            if k in props and not isinstance(v, (dict, list)) and v is not None and \
                    (props[k].get('type') or '') not in ('file', 'directory'):
                props[k] = dict(props[k], value=v)
    src = web_sources(base)
    res = {'id': wid, 'title': pj.get('title', ''), 'type': 'web' + (' (preset of %s)' % preset_of if preset_of else ''),
           'props': {}, 'error': None}
    D = Daemon(build, extra_env={'WALPE_WEB_OFFLINE': '1'})
    try:
        D.req({'cmd': 'open', 'path': path})
        t0 = time.time()
        log = []
        while time.time() - t0 < 60:
            log = read_weblog(D, wid, 0)
            if any(e['kind'] == 'load' for e in log) and any(e['kind'] == 'ack' for e in log):
                break
            if not D.alive():
                break
            time.sleep(0.2)
        load = next((e['data'] for e in log if e['kind'] == 'load'), None)
        ack = next((e['data'] for e in log if e['kind'] == 'ack'), None)
        if load is None:
            res['error'] = 'page never got its properties (no load push within 60 s)'
            return res
        res['listener'] = bool(ack and ack.get('listener'))
        res['load_error'] = (ack or {}).get('error', '')
        seen = len(log)
        prev_key = None
        for key in sorted(props, key=lambda k: (props[k].get('order', 1e9), k)):
            d = props[key]
            ent = {'type': d.get('type', ''), 'text': d.get('text', ''), 'named': bool(
                       re.search(r'(?<![A-Za-z0-9_$])' + re.escape(key) + r'(?![A-Za-z0-9_$])', src)),
                   'effective': False, 'on_load': False, 'on_change': False, 'extra_keys': 0, 'how': ''}
            res['props'][key] = ent
            if not is_option(d):
                ent['how'] = 'label'
                continue
            lv = load.get(key)
            ent['on_load'] = isinstance(lv, dict) and 'value' in lv and shape_ok(d, lv['value'])
            if not ent['on_load']:
                ent['load_value'] = json.dumps(lv)[:80]
            alts = alternates(key, d, testfile)
            if not alts:
                ent['how'] = 'no-alternate'
                continue
            a = alts[0]
            send = {key: a}
            if prev_key is not None:      # put the previous option back in the same request
                pv = props[prev_key].get('value')
                send[prev_key] = pv if pv is not None else ''
            D.req({'cmd': 'setprops', 'path': path, 'props': send})
            prev_key = key
            got = None
            t1 = time.time()
            while time.time() - t1 < 8 and got is None:
                new = read_weblog(D, wid, seen)
                for e in new:
                    if e['kind'] in ('change', 'load') and isinstance(e['data'], dict) and key in e['data']:
                        got = e['data']
                if got is None:
                    time.sleep(0.05)
            time.sleep(0.05)
            new = read_weblog(D, wid, seen)
            seen += len(new)
            errs = [e['data'].get('error') for e in new if e['kind'] == 'ack' and isinstance(e['data'], dict) and e['data'].get('error')]
            if got is None:
                ent['how'] = 'not delivered on change'
                continue
            v = got[key].get('value') if isinstance(got[key], dict) else None
            want = a
            if (d.get('type') or '').lower() in ('file', 'directory'):
                want = a
            same = (v == want) or (isinstance(v, (int, float)) and isinstance(want, (int, float)) and abs(v - want) < 1e-9) \
                or (isinstance(want, bool) and v is want) or str(v) == str(want)
            ent['extra_keys'] = len([k for k in got if k not in send])
            ent['on_change'] = same and shape_ok(d, v)
            if (d.get('type') or '').lower() == 'directory':
                # the page must also be handed the folder's files
                # (userDirectoryFilesAddedOrChanged)
                nf = 0
                for e in new:
                    if e['kind'] == 'dirs' and isinstance(e['data'], dict):
                        nf = max(nf, e['data'].get(key, 0))
                ent['dir_files'] = nf
                if nf < 1:
                    ent['on_change'] = False
                    ent['how'] = 'folder value sent, but no file list (userDirectoryFilesAddedOrChanged)'

            if not ent['on_change']:
                ent['change_value'] = json.dumps(v)[:80]
            if errs:
                ent['page_error'] = errs[0][:160]
            ent['effective'] = ent['on_load'] and ent['on_change'] and ent['named']
        D.req({'cmd': 'resetprops', 'path': path})
    finally:
        D.close(keep_log=os.path.join(outdir, wid + '.daemon.log'))
    for key, e in res['props'].items():
        if e['how'] == 'label':
            e['class'] = 'label'
        elif e['effective']:
            e['class'] = 'effective'
        elif not e['named']:
            e['class'] = 'unbound'
            e['reason'] = 'the page never names it (delivered, but nothing reads it)'
        else:
            e['class'] = 'unread'
            e['reason'] = 'delivery problem: load %s (%s), change %s (%s) %s' % (
                e['on_load'], e.get('load_value', ''), e['on_change'], e.get('change_value', ''), e.get('how', ''))
    res['resent'] = sum(1 for e in res['props'].values() if e.get('extra_keys', 0) > 0)
    return res


def solve(cond, props, defaults):
    """a small assignment of the referenced properties that makes cond true"""
    refs = [r for r in dict.fromkeys(cond.refs) if r in props][:7]
    budget = [4000]             # assignments tried at most
    cands = []
    for r in refs:
        d = props[r]
        t = (d.get('type') or '').lower()
        if t in ('bool', 'checkbox'):
            cands.append([True, False])
        elif t == 'combo':
            cands.append([o.get('value') for o in d.get('options') or [] if isinstance(o, dict)][:12])
        elif t == 'slider':
            cands.append([d.get('min', 0), d.get('max', 1), d.get('value')])
        else:
            cands.append([d.get('value')])

    def rec(i, cur):
        if i == len(refs):
            budget[0] -= 1
            vals = dict(defaults)
            vals.update(cur)
            return dict(cur) if cond.eval(vals) else None
        # the default first: the smallest change that satisfies the condition
        order = [defaults.get(refs[i])] + [v for v in cands[i] if str(v) != str(defaults.get(refs[i]))]
        for v in order:
            if budget[0] <= 0:
                return None
            cur[refs[i]] = v
            r = rec(i + 1, cur)
            if r:
                return r
        cur.pop(refs[i], None)
        return None
    got = rec(0, {})
    if not got:
        return None
    return {k: v for k, v in got.items() if str(v) != str(defaults.get(k))}


def classify(res):
    """reason for every option that is not effective"""
    for key, e in res['props'].items():
        e.pop('reason', None)
        if e['how'] == 'label' or (e.get('type') or '').lower() in ('group', 'text', ''):
            e['class'] = 'label'
            continue
        if e['effective']:
            e['class'] = 'effective'
            continue
        feat = feature_of(e['sites'])
        if e['sites'] and not e['uses']:
            if feat:
                e['class'] = 'engine-feature'
                e['reason'] = feat
            else:
                e['class'] = 'unread'
                e['reason'] = 'bound at %s but WalPE never reads the value' % ', '.join(e['sites'][:3])
        elif e['sites'] and e['uses']:
            if feat and len(e['sites']) == 1:
                e['class'] = 'engine-feature'
                e['reason'] = feat
            else:
                e['class'] = 'read-no-change'
                e['reason'] = 'read (%s) at %s but the scene state did not change' % (', '.join(e['uses']), ', '.join(e['sites'][:3]))
        elif e['uses']:
            e['class'] = 'read-no-change'
            e['reason'] = 'read by a script (%s) but no layer/effect state changed' % ', '.join(e['uses'])
        elif e['cond_users']:
            e['class'] = 'ui-only'
            e['reason'] = 'only shows/hides other options (%d conditions)' % len(e['cond_users'])
        elif e['in_script']:
            e['class'] = 'script-unread'
            e['reason'] = 'named in a script that did not read it while loading'
        else:
            e['class'] = 'unbound'
            e['reason'] = 'not bound to anything in the scene (does nothing in Wallpaper Engine either)'


# ---------------------------------------------------------------- report
CLASSES = ['effective', 'ui-only', 'unbound', 'engine-feature', 'unread', 'read-no-change', 'script-unread']


def report(dirs, out):
    runs = []
    for d in dirs:
        rs = {}
        for f in sorted(glob.glob(os.path.join(d, '*.result.json'))):
            r = load_json(f)
            if r.get('type', '').startswith('scene'):
                classify(r)     # with the current rules, whatever the run used
            rs[r['id']] = r
        runs.append(rs)
    lines = []
    for ri, rs in enumerate(runs):
        name = os.path.basename(os.path.normpath(dirs[ri]))
        lines.append('## %s' % name)
        lines.append('')
        lines.append('| ID | title | labels | options | effective | UI-only | unbound in scene | engine feature | bound but unread | read, no state change | script did not read |')
        lines.append('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
        tot = collections.Counter()
        for wid, r in sorted(rs.items()):
            c = collections.Counter(e.get('class', '?') for e in r['props'].values())
            nopt = sum(v for k, v in c.items() if k != 'label')
            tot.update(c)
            tot['options'] += nopt
            lines.append('| %s | %s | %d | %d | %s |%s' % (wid, (r.get('title') or '')[:38].replace('|', '/'), c['label'], nopt,
                                                         ' | '.join(str(c[k]) for k in CLASSES),
                                                         ' ERROR: ' + r['error'] if r.get('error') else ''))
        lines.append('| **total** | | %d | %d | %s |' % (tot['label'], tot['options'], ' | '.join('**%d**' % tot[k] for k in CLASSES)))
        lines.append('')
        feats = collections.Counter()
        for r in rs.values():
            for e in r['props'].values():
                if e.get('class') == 'engine-feature':
                    feats[e['reason']] += 1
        if feats:
            lines.append('Options that need an engine feature WalPE lacks:')
            lines.append('')
            for k, v in feats.most_common():
                lines.append('- %s: %d' % (k, v))
            lines.append('')
        lines.append('Not effective, by wallpaper (class: option = reason):')
        lines.append('')
        for wid, r in sorted(rs.items()):
            bad = [(k, e) for k, e in r['props'].items() if e.get('class') in ('unread', 'read-no-change', 'script-unread', 'engine-feature')]
            if not bad:
                continue
            lines.append('- **%s** %s' % (wid, (r.get('title') or '')[:50]))
            for k, e in bad:
                lines.append('  - %s: `%s` (%s) = %s' % (e['class'], k[:40], e['type'], e.get('reason', '')))
        lines.append('')
    if len(runs) == 2:
        a, b = runs
        lines.append('## change (first run -> second run)')
        lines.append('')
        fixed = collections.Counter()
        broke = []
        for wid in sorted(set(a) & set(b)):
            for k, eb in b[wid]['props'].items():
                ea = a[wid]['props'].get(k)
                if not ea:
                    continue
                if not ea['effective'] and eb['effective']:
                    fixed[(wid, ea.get('class', '?'))] += 1
                if ea['effective'] and not eb['effective']:
                    broke.append((wid, k, eb.get('reason', eb.get('class'))))
        for (wid, cl), nn in sorted(fixed.items()):
            lines.append('- %s: %d option(s) now effective (were: %s)' % (wid, nn, cl))
        for wid, k, why in broke:
            lines.append('- REGRESSION %s `%s`: %s' % (wid, k, why))
        lines.append('')
    txt = '\n'.join(lines)
    if out:
        with open(out, 'w') as f:
            f.write(txt + '\n')
    print(txt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build')
    ap.add_argument('--out')
    ap.add_argument('--ids', default='')
    ap.add_argument('--type', default='scene', choices=['scene', 'web'],
                    help='web: is every option delivered to the page (applyUserProperties) on load and, '
                         'alone and with the right value type, when it changes; does the page name it')
    ap.add_argument('--recheck', action='store_true',
                    help='tier 2: re-check, with a digest after 1.5 s of scene time, the options the '
                         'results in --out found read but without a state change (script clocks, timers)')
    ap.add_argument('--report', nargs='+')
    ap.add_argument('--report-out')
    a = ap.parse_args()
    if a.report:
        report(a.report, a.report_out)
        return
    os.makedirs(a.out, exist_ok=True)
    testfile = os.path.join(a.out, 'testimage', 'walpe-test.png')
    os.makedirs(os.path.dirname(testfile), exist_ok=True)
    if not os.path.exists(testfile):
        subprocess.run(['convert', '-size', '640x360', 'gradient:#ff2020-#2020ff', '-fill', 'white', '-gravity', 'center',
                        '-pointsize', '60', '-annotate', '0', 'USER IMAGE', testfile], check=True)
    ids = [i for i in a.ids.split(',') if i]
    if not ids:
        for d in sorted(os.listdir(ITEMS)):
            try:
                pj = load_json(os.path.join(ITEMS, d, 'project.json'))
            except Exception:
                continue
            t = (pj.get('type') or '').lower()
            if not t and 'dependency' in pj:
                try:
                    t = (load_json(os.path.join(ITEMS, str(pj['dependency']), 'project.json')).get('type') or '').lower()
                except Exception:
                    t = ''
            if t == a.type:
                ids.append(d)
    for wid in ids:
        t0 = time.time()
        prev = None
        if a.recheck:
            rf = os.path.join(a.out, wid + '.result.json')
            if not os.path.exists(rf):
                continue
            prev = load_json(rf)
            if not any(not e['effective'] and e.get('how') != 'label' and (e['uses'] or e['in_script'])
                       for e in prev['props'].values()):
                continue
        try:
            if a.type == 'web':
                r = check_web(wid, a.build, a.out, testfile)
            else:
                r = check_scene(wid, a.build, a.out, testfile, prev)
        except Exception as ex:      # keep going: one wallpaper must not end the run
            if prev:
                print(wid, 'recheck failed: %r' % (ex,), flush=True)
                continue
            r = {'id': wid, 'title': '', 'props': {}, 'error': 'harness: %r' % (ex,)}
        with open(os.path.join(a.out, wid + '.result.json'), 'w') as f:
            json.dump(r, f, indent=1, ensure_ascii=False)
        c = collections.Counter(e.get('class', '?') for e in r['props'].values())
        extra = ' re-sent other options on %d changes' % r['resent'] if r.get('resent') else ''
        print('%s %-12s %5.0fs %s %s%s' % (wid, r.get('type', ''), time.time() - t0, dict(c), r.get('error') or '', extra), flush=True)


if __name__ == '__main__':
    main()
