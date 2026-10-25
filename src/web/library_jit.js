// carma2web: instantiate JIT-generated wasm modules and register them in the function table
mergeInto(LibraryManager.library, {
  $jitAddFn__deps: ['$wasmTable'],
  $jitAddFn: function (fn) {
    if (!Module.__jitNext || Module.__jitNext >= Module.__jitEnd) {
      var base = wasmTable.grow(1024);
      Module.__jitNext = base;
      Module.__jitEnd = base + 1024;
    }
    var idx = Module.__jitNext++;
    wasmTable.set(idx, fn);
    return idx;
  },
  jit_js_compile__deps: ['$jitAddFn', '$wasmTable'],
  jit_js_compile: function (ptr, len, region) {
    var bytes = HEAPU8.slice(ptr, ptr + len);
    if (!Module.__jitImports) {
      Module.__jitImports = { env: {
        memory: wasmMemory,
        table: wasmTable,
        interp: wasmExports['jit_h_interp'],
        mat: wasmExports['jit_h_mat'],
        fist: wasmExports['jit_h_fist'],
        fild64: wasmExports['jit_h_fild64'],
      } };
      Module.__jitPending = 0;
    }
    if (ENVIRONMENT_IS_NODE || len < 4000) {
      try {
        var inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), Module.__jitImports);
        wasmExports['jit_installed'](region, jitAddFn(inst.exports.f));
      } catch (e) {
        err('jit compile failed for region ' + region + ': ' + e);
        if (!Module.__jitBad) Module.__jitBad = bytes;
        wasmExports['jit_installed'](region, 0);
      }
      return;
    }
    Module.__jitPending++;
    WebAssembly.instantiate(bytes, Module.__jitImports).then(function (res) {
      Module.__jitPending--;
      var idx = jitAddFn(res.instance.exports.f);
      wasmExports['jit_installed'](region, idx);
    }, function (e) {
      Module.__jitPending--;
      err('jit compile failed for region ' + region + ': ' + e);
      if (!Module.__jitDumped) { Module.__jitDumped = 1; Module.__jitBad = bytes; }
      wasmExports['jit_installed'](region, 0);
    });
  },
});
