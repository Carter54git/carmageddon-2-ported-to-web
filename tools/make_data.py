#!/usr/bin/env python3
"""
make_data.py - pack your own copy of Carmageddon II into the data files the
web build loads (carma2data.js + carma2web.data.000, .001, ...).

    python3 make_data.py <game folder> <output folder> [--music <folder>]

<game folder>   the installed game: the folder that contains CARMA2_HW.EXE,
                3DFX_WIN.BDD and the DATA folder (see docs/GAME_FILES.md)
<output folder> the web site folder (index.html, carma2hy.js, carma2hy.wasm)
--music DIR     optional: copy CD audio tracks (Track02.ogg/.mp3/.wav ...)
                from DIR into <output folder>/music

No third-party packages are needed (Python 3.6+).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys

PART_SIZE = 20 * 1024 * 1024          # 20 MB parts fit most static hosts
PACKAGE = 'carma2web.data'
ROOT = '/carma2'                       # mount point the runtime expects

# The runtime patches a few addresses inside this exact executable
# (widescreen cameras, HUD anchoring), so other builds may not work.
KNOWN_EXE = {
    '698c2b11865578b6ea2a77324fbc1840080841a8ae4041e9ca14ca5ff027bd6c': 'the version this port is tested with (2 674 688 bytes)',
}
REQUIRED = ['carma2_hw.exe', '3dfx_win.bdd', 'data/common/', 'data/cars/', 'data/races/', 'data/intrface/', 'data/sound/']
SKIP_DIRS = {'music', 'savegame', '__macosx'}
SKIP_EXT = {'.mp3', '.ogg', '.wav.bak', '.lnk', '.ico', '.isu'}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def collect(game):
    files = []
    for dp, dns, fns in os.walk(game):
        dns[:] = sorted(d for d in dns if d.lower() not in SKIP_DIRS)
        for fn in sorted(fns):
            full = os.path.join(dp, fn)
            rel = os.path.relpath(full, game).replace(os.sep, '/')
            if os.path.splitext(fn)[1].lower() in SKIP_EXT and '/' not in rel:
                continue
            # the runtime opens exactly "<root>/carma2_HW.exe"; everything else
            # is looked up case-insensitively
            if rel.lower() == 'carma2_hw.exe':
                rel = 'carma2_HW.exe'
            files.append((rel, full))
    return files


LOADER_HEAD = r"""  var Module = typeof Module !== 'undefined' ? Module : {};
  if (!Module.expectedDataFileDownloads) {
    Module.expectedDataFileDownloads = 0;
  }
  Module.expectedDataFileDownloads++;
  (function() {
    if (Module['ENVIRONMENT_IS_PTHREAD'] || Module['$ww']) return;
    var loadPackage = function(metadata) {
      var PACKAGE_NAME = '%(pkg)s';
      var REMOTE_PACKAGE_BASE = '%(pkg)s';
      var REMOTE_PACKAGE_NAME = Module['locateFile'] ? Module['locateFile'](REMOTE_PACKAGE_BASE, '') : REMOTE_PACKAGE_BASE;
      var REMOTE_PACKAGE_SIZE = metadata['remote_package_size'];
      function fetchRemotePackage(packageName, packageSize, callback, errback) {
        var parts = %(parts)s;
        var buf = new Uint8Array(packageSize), loaded = parts.map(function () { return 0; }), done = 0, failed = false;
        function status() {
          var l = 0;
          for (var i = 0; i < loaded.length; i++) l += loaded[i];
          if (Module['setStatus']) Module['setStatus']('Loading game data: ' + Math.floor(l / 1048576) + ' of ' + Math.floor(packageSize / 1048576) + ' MB');
        }
        function get(i, tries) {
          var name = packageName + '.' + ('00' + i).slice(-3);
          var xhr = new XMLHttpRequest();
          xhr.open('GET', name, true);
          xhr.responseType = 'arraybuffer';
          function retry() {
            loaded[i] = 0;
            if (tries < 4) { setTimeout(function () { get(i, tries + 1); }, 1500 * (tries + 1)); return; }
            if (failed) return;
            failed = true;
            if (Module['setStatus']) Module['setStatus']('Could not load ' + name + '. Please reload the page.');
            errback(new Error('NetworkError for: ' + name));
          }
          xhr.onprogress = function (e) { loaded[i] = e.loaded; status(); };
          xhr.onerror = retry;
          xhr.onload = function () {
            if ((xhr.status == 200 || xhr.status == 304 || (xhr.status == 0 && xhr.response)) && xhr.response && xhr.response.byteLength === parts[i][1]) {
              buf.set(new Uint8Array(xhr.response), parts[i][0]);
              loaded[i] = parts[i][1];
              status();
              if (++done === parts.length) callback(buf.buffer);
            } else retry();
          };
          xhr.send(null);
        }
        status();
        for (var i = 0; i < parts.length; i++) get(i, 0);
      }
      function handleError(error) {
        console.error('package error:', error);
      }
      var fetchedCallback = null;
      var fetched = Module['getPreloadedPackage'] ? Module['getPreloadedPackage'](REMOTE_PACKAGE_NAME, REMOTE_PACKAGE_SIZE) : null;
      if (!fetched) fetchRemotePackage(REMOTE_PACKAGE_NAME, REMOTE_PACKAGE_SIZE, function(data) {
        if (fetchedCallback) { fetchedCallback(data); fetchedCallback = null; } else { fetched = data; }
      }, handleError);
      function runWithFS() {
%(paths)s
        var files = metadata['files'];
        for (var i = 0; i < files.length; ++i) Module['addRunDependency']('fp ' + files[i].filename);
        function processPackageData(arrayBuffer) {
          var byteArray = new Uint8Array(arrayBuffer);
          for (var i = 0; i < files.length; ++i) {
            var f = files[i];
            Module['FS_createDataFile'](f.filename, null, byteArray.subarray(f.start, f.end), true, true, true);
            Module['removeRunDependency']('fp ' + f.filename);
          }
          Module['removeRunDependency']('datafile_%(pkg)s');
        }
        Module['addRunDependency']('datafile_%(pkg)s');
        if (!Module.preloadResults) Module.preloadResults = {};
        Module.preloadResults[PACKAGE_NAME] = {fromCache: false};
        if (fetched) { processPackageData(fetched); fetched = null; } else { fetchedCallback = processPackageData; }
      }
      if (Module['calledRun']) {
        runWithFS();
      } else {
        if (!Module['preRun']) Module['preRun'] = [];
        Module['preRun'].push(runWithFS);
      }
    };
    loadPackage(%(meta)s);
  })();
"""


def main():
    ap = argparse.ArgumentParser(description='Pack Carmageddon II game files for the web build.')
    ap.add_argument('game', help='installed game folder (contains CARMA2_HW.EXE and DATA)')
    ap.add_argument('out', help='web site folder to write carma2data.js and carma2web.data.NNN into')
    ap.add_argument('--music', help='folder with CD audio tracks TrackNN.ogg/.mp3/.wav to copy into out/music')
    ap.add_argument('--force', action='store_true', help='continue even if CARMA2_HW.EXE is an unknown version')
    a = ap.parse_args()

    game = os.path.abspath(a.game)
    files = collect(game)
    lower = {rel.lower() for rel, _ in files}
    missing = [r for r in REQUIRED if not any(x == r or (r.endswith('/') and x.startswith(r)) for x in lower)]
    if missing:
        sys.exit('error: %s does not look like a Carmageddon II install, missing: %s\n'
                 'Point it at the folder that contains CARMA2_HW.EXE (see docs/GAME_FILES.md).'
                 % (game, ', '.join(missing)))

    exe = dict(files)['carma2_HW.exe']
    digest = sha256(exe)
    if digest in KNOWN_EXE:
        print('CARMA2_HW.EXE: %s' % KNOWN_EXE[digest])
    else:
        print('WARNING: CARMA2_HW.EXE has an unknown checksum (sha256 %s).' % digest)
        print('         The runtime is tested with one specific executable only; other versions may crash.')
        if not a.force:
            sys.exit('Re-run with --force to pack it anyway.')

    os.makedirs(a.out, exist_ok=True)
    for old in os.listdir(a.out):
        if re.match(re.escape(PACKAGE) + r'\.\d{3}$', old):
            os.remove(os.path.join(a.out, old))

    meta, parts, dirs = [], [], set()
    offset, part_no, part_left = 0, 0, PART_SIZE
    out = open(os.path.join(a.out, '%s.%03d' % (PACKAGE, part_no)), 'wb')
    for rel, full in files:
        name = ROOT + '/' + rel
        d = os.path.dirname(name)
        while d not in dirs and d != '/':
            dirs.add(d)
            d = os.path.dirname(d)
        with open(full, 'rb') as f:
            data = f.read()
        meta.append({'filename': name, 'start': offset, 'end': offset + len(data)})
        offset += len(data)
        view = memoryview(data)
        while len(view):
            n = min(part_left, len(view))
            out.write(view[:n])
            view = view[n:]
            part_left -= n
            if part_left == 0:
                out.close()
                parts.append([part_no * PART_SIZE, PART_SIZE])
                part_no += 1
                part_left = PART_SIZE
                out = open(os.path.join(a.out, '%s.%03d' % (PACKAGE, part_no)), 'wb')
    out.close()
    last = PART_SIZE - part_left
    if last:
        parts.append([part_no * PART_SIZE, last])
    else:
        os.remove(os.path.join(a.out, '%s.%03d' % (PACKAGE, part_no)))

    paths = []
    for d in sorted(dirs, key=lambda x: (x.count('/'), x)):
        parent, base = os.path.dirname(d), os.path.basename(d)
        paths.append("        Module['FS_createPath'](%s, %s, true, true);" % (json.dumps(parent), json.dumps(base)))

    js = LOADER_HEAD % {
        'pkg': PACKAGE,
        'parts': json.dumps(parts),
        'paths': '\n'.join(paths),
        'meta': json.dumps({'files': meta, 'remote_package_size': offset}, separators=(',', ':')),
    }
    with open(os.path.join(a.out, 'carma2data.js'), 'w', encoding='utf-8') as f:
        f.write(js)

    copied = 0
    if a.music:
        mdir = os.path.join(a.out, 'music')
        os.makedirs(mdir, exist_ok=True)
        for fn in sorted(os.listdir(a.music)):
            m = re.match(r'(?i)^track0*(\d+)\.(ogg|mp3|wav)$', fn)
            if m:
                shutil.copyfile(os.path.join(a.music, fn),
                                os.path.join(mdir, 'Track%02d.%s' % (int(m.group(1)), m.group(2).lower())))
                copied += 1

    print('packed %d files, %.1f MB in %d part(s) -> %s' % (len(meta), offset / 1048576.0, len(parts), os.path.abspath(a.out)))
    if a.music:
        print('copied %d music track(s)' % copied)
    print('done. Serve the folder over http(s) and open index.html.')


if __name__ == '__main__':
    main()
