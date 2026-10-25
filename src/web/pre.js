// mount IndexedDB-backed storage for saved games before main() runs
Module.preRun = (Module.preRun || []).concat([function () {
  try {
    FS.mkdir('/persist');
    FS.mount(IDBFS, {}, '/persist');
    addRunDependency('persist');
    FS.syncfs(true, function (e) { if (e) err('restore failed: ' + e); removeRunDependency('persist'); });
  } catch (e) { err('persist setup: ' + e); }
}]);

// probe for CD music provided as files: music/Track02.ogg ... (also .mp3, .wav)
Module.preRun = Module.preRun.concat([function () {
  if (typeof fetch === 'undefined') return;
  addRunDependency('music');
  Module.__musicTracks = {};
  Module.__musicMax = 0;
  var probes = [];
  for (var t = 2; t <= 20; t++) {
    ['ogg', 'mp3', 'wav'].forEach(function (ext) {
      var n = (t < 10 ? '0' : '') + t, url = 'music/Track' + n + '.' + ext, track = t;
      probes.push(fetch(url, { method: 'HEAD' }).then(function (r) {
        if (r.ok && !Module.__musicTracks[track]) { Module.__musicTracks[track] = url; if (track > Module.__musicMax) Module.__musicMax = track; }
      }).catch(function () {}));
    });
  }
  Promise.all(probes).then(function () {
    if (Module.__musicMax) out('music: ' + Object.keys(Module.__musicTracks).length + ' CD tracks found in music/');
    removeRunDependency('music');
  });
}]);
