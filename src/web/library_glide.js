// carma2web: WebGL2 backend for the Glide emulation in glide.c
// State block layout (int32 unless noted) must match glide.c (GlState).
mergeInto(LibraryManager.library, {
  glide_gl_init: function (w, h) {
    var canvas = Module['canvas'];
    // render scale / MSAA ("3dfx-like" smooth output): Module.glideScale, Module.glideMsaa
    var scale = Math.max(1, Math.min(4, (Module.glideScale | 0) || 2));
    var gl = canvas.getContext('webgl2', { antialias: false, depth: false, alpha: false, preserveDrawingBuffer: false });
    if (!gl) { err('WebGL2 not available'); return 0; }
    var maxS = gl.getParameter(gl.MAX_SAMPLES) || 0;
    var msaa = Module.glideMsaa === undefined ? 4 : (Module.glideMsaa | 0);
    msaa = Math.min(msaa, maxS);
    var G = Module.__glide = { gl: gl, w: w, h: h, scale: scale, msaa: msaa, tex: [null], freeIds: [], cur: {},
      forceFilter: Module.glideFilter === undefined ? 1 : (Module.glideFilter | 0) };
    G.aniso = gl.getExtension('EXT_texture_filter_anisotropic');
    // widescreen: the GL target is vw x h "Glide pixels", vw following the window shape (>= 4:3)
    G.calcVw = function () {
      if (Module.glideWide === 0) return w;
      var cw = window.innerWidth || w, ch = window.innerHeight || h;
      var vw = Math.round(h * cw / ch / 2) * 2;
      return Math.max(w, Math.min(w * 2, vw));
    };
    G.fit = function () {
      var cw = window.innerWidth, ch = window.innerHeight, a = G.vw / h;
      var dw = Math.min(cw, ch * a), dh = dw / a;
      canvas.style.width = dw + 'px'; canvas.style.height = dh + 'px';
    };
    function mkfbo(fw, fh) {
      var f = gl.createFramebuffer(), t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, fw, fh, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.bindFramebuffer(gl.FRAMEBUFFER, f);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);
      f.tex = t;
      return f;
    }
    G.alloc = function (vw) {
      if (G.fbo) {
        gl.deleteFramebuffer(G.fbo); gl.deleteRenderbuffer(G.color); gl.deleteRenderbuffer(G.depth);
        gl.deleteTexture(G.rfbo.tex); gl.deleteFramebuffer(G.rfbo);
      }
      G.vw = vw;
      var W = G.W = vw * scale, H = G.H = h * scale;
      canvas.width = W; canvas.height = H;
      G.fbo = gl.createFramebuffer();
      gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
      G.color = gl.createRenderbuffer();
      gl.bindRenderbuffer(gl.RENDERBUFFER, G.color);
      if (msaa > 0) gl.renderbufferStorageMultisample(gl.RENDERBUFFER, msaa, gl.RGBA8, W, H);
      else gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, W, H);
      gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, G.color);
      G.depth = gl.createRenderbuffer();
      gl.bindRenderbuffer(gl.RENDERBUFFER, G.depth);
      if (msaa > 0) gl.renderbufferStorageMultisample(gl.RENDERBUFFER, msaa, gl.DEPTH_COMPONENT24, W, H);
      else gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, W, H);
      gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, G.depth);
      G.rfbo = mkfbo(W, H);
      gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
      gl.viewport(0, 0, W, H);
      gl.clearColor(0, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      if (G.prog) { gl.useProgram(G.prog); gl.uniform2f(G.u.uScreen, vw, h); }
      G.cur = {};
      G.fit();
    };
    G.sfbo = mkfbo(w, h);
    G.alloc(G.calcVw());
    window.addEventListener('resize', function () { G.fit(); });
    out('Glide/WebGL: ' + G.W + 'x' + G.H + ' (x' + scale + '), MSAA ' + msaa + 'x, bilinear ' + (G.forceFilter ? 'forced' : 'as requested') + ', width ' + G.vw);
    var vs = '#version 300 es\n' +
      'in vec2 aPos; in float aDepth; in vec4 aColor; in vec3 aTex; in float aW;\n' +
      'uniform vec2 uScreen; uniform float uPointSize;\n' +
      'out vec4 vColor; out vec3 vTex; out float vW;\n' +
      'void main(){ gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, aDepth * 2.0 - 1.0, 1.0);\n' +
      ' vColor = aColor / 255.0; vTex = aTex; vW = aW; gl_PointSize = uPointSize; }\n';
    var fs = '#version 300 es\nprecision highp float; precision highp int;\n' +
      'in vec4 vColor; in vec3 vTex; in float vW;\n' +
      'uniform sampler2D uTex; uniform int uTexOn;\n' +
      'uniform ivec4 uCC; uniform int uCCInv; uniform ivec4 uAC; uniform int uACInv;\n' +
      'uniform vec4 uConst; uniform int uFog; uniform vec3 uFogColor; uniform float uFogTable[64];\n' +
      'uniform int uChroma; uniform ivec3 uChromaKey;\n' +
      'out vec4 oColor;\n' +
      'float fogf(float w){ if (w < 1.0) return uFogTable[0]; float e = floor(log2(w)); float m = 8.0 - 8.0 / (w / exp2(e));\n' +
      ' float fi = clamp(4.0 * e + m, 0.0, 63.0); int i0 = int(floor(fi)); int i1 = min(i0 + 1, 63); return mix(uFogTable[i0], uFogTable[i1], fract(fi)); }\n' +
      'void main(){\n' +
      ' vec4 t = vec4(0.0, 0.0, 0.0, 1.0);\n' +
      ' if (uTexOn != 0) t = texture(uTex, vTex.xy / vTex.z);\n' +
      ' vec3 lc = uCC.z == 1 ? uConst.rgb : vColor.rgb;\n' +
      ' float la = uAC.z == 1 ? uConst.a : vColor.a;\n' +
      ' vec3 oc = uCC.w == 1 ? t.rgb : (uCC.w == 2 ? uConst.rgb : vColor.rgb);\n' +
      ' float oa = uAC.w == 1 ? t.a : (uAC.w == 2 ? uConst.a : vColor.a);\n' +
      ' vec3 f; int cf = uCC.y;\n' +
      ' if (cf == 0) f = vec3(0.0); else if (cf == 1) f = lc; else if (cf == 2) f = vec3(oa); else if (cf == 3) f = vec3(la);\n' +
      ' else if (cf == 4) f = vec3(t.a); else if (cf == 9) f = 1.0 - lc; else if (cf == 10) f = vec3(1.0 - oa);\n' +
      ' else if (cf == 11) f = vec3(1.0 - la); else if (cf == 12) f = vec3(1.0 - t.a); else f = vec3(1.0);\n' +
      ' vec3 c; int fn = uCC.x;\n' +
      ' if (fn == 0) c = vec3(0.0); else if (fn == 1) c = lc; else if (fn == 2) c = vec3(la); else if (fn == 3) c = f * oc;\n' +
      ' else if (fn == 4) c = f * oc + lc; else if (fn == 5) c = f * oc + la; else if (fn == 6) c = f * (oc - lc);\n' +
      ' else if (fn == 7) c = f * (oc - lc) + lc; else if (fn == 8) c = f * (oc - lc) + la; else if (fn == 9) c = lc - f * lc;\n' +
      ' else if (fn == 16) c = la - f * lc; else c = lc;\n' +
      ' c = clamp(c, 0.0, 1.0); if (uCCInv != 0) c = 1.0 - c;\n' +
      ' float fa; int af = uAC.y;\n' +
      ' if (af == 0) fa = 0.0; else if (af == 1 || af == 3) fa = la; else if (af == 2) fa = oa; else if (af == 4) fa = t.a;\n' +
      ' else if (af == 9 || af == 11) fa = 1.0 - la; else if (af == 10) fa = 1.0 - oa; else if (af == 12) fa = 1.0 - t.a; else fa = 1.0;\n' +
      ' float a; int afn = uAC.x;\n' +
      ' if (afn == 0) a = 0.0; else if (afn == 1 || afn == 2) a = la; else if (afn == 3) a = fa * oa; else if (afn == 4 || afn == 5) a = fa * oa + la;\n' +
      ' else if (afn == 6) a = fa * (oa - la); else if (afn == 7 || afn == 8) a = fa * (oa - la) + la; else if (afn == 9 || afn == 16) a = la - fa * la; else a = la;\n' +
      ' a = clamp(a, 0.0, 1.0); if (uACInv != 0) a = 1.0 - a;\n' +
      ' if (uChroma != 0) { ivec3 q = ivec3(c * 255.0) >> ivec3(3, 2, 3); if (q == uChromaKey) discard; }\n' +
      ' if (uFog == 1) c = mix(c, uFogColor, vColor.a); else if (uFog == 2) c = mix(c, uFogColor, fogf(vW));\n' +
      ' oColor = vec4(c, a); }\n';
    function sh(type, src) { var s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) err('glide shader: ' + gl.getShaderInfoLog(s)); return s; }
    var p = gl.createProgram();
    gl.attachShader(p, sh(gl.VERTEX_SHADER, vs)); gl.attachShader(p, sh(gl.FRAGMENT_SHADER, fs));
    gl.bindAttribLocation(p, 0, 'aPos'); gl.bindAttribLocation(p, 1, 'aDepth'); gl.bindAttribLocation(p, 2, 'aColor');
    gl.bindAttribLocation(p, 3, 'aTex'); gl.bindAttribLocation(p, 4, 'aW');
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) err('glide link: ' + gl.getProgramInfoLog(p));
    G.prog = p;
    gl.useProgram(p);
    var U = G.u = {};
    ['uScreen', 'uPointSize', 'uTex', 'uTexOn', 'uCC', 'uCCInv', 'uAC', 'uACInv', 'uConst', 'uFog', 'uFogColor', 'uFogTable', 'uChroma', 'uChromaKey'].forEach(function (n) { U[n] = gl.getUniformLocation(p, n); });
    gl.uniform2f(U.uScreen, G.vw, h);
    gl.uniform1f(U.uPointSize, scale);
    gl.uniform1i(U.uTex, 0);
    G.vao = gl.createVertexArray(); gl.bindVertexArray(G.vao);
    G.vbo = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, G.vbo);
    var S = 44;
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, S, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 1, gl.FLOAT, false, S, 8);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 4, gl.FLOAT, false, S, 12);
    gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 3, gl.FLOAT, false, S, 28);
    gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4, 1, gl.FLOAT, false, S, 40);
    // quad program for LFB composites
    var qvs = '#version 300 es\nin vec2 aPos; out vec2 vUV; uniform vec4 uRect; uniform vec2 uScreen;\n' +
      'void main(){ vec2 px = uRect.xy + aPos * uRect.zw; vUV = aPos; gl_Position = vec4(px.x / uScreen.x * 2.0 - 1.0, 1.0 - px.y / uScreen.y * 2.0, 0.0, 1.0); }\n';
    var qfs = '#version 300 es\nprecision mediump float; in vec2 vUV; uniform sampler2D uTex; out vec4 o;\n' +
      'void main(){ vec4 c = texture(uTex, vUV); if (c.a < 0.5) discard; o = vec4(c.rgb, 1.0); }\n';
    var q = gl.createProgram();
    gl.attachShader(q, sh(gl.VERTEX_SHADER, qvs)); gl.attachShader(q, sh(gl.FRAGMENT_SHADER, qfs));
    gl.bindAttribLocation(q, 0, 'aPos'); gl.linkProgram(q);
    G.qprog = q; G.qRect = gl.getUniformLocation(q, 'uRect'); G.qScreen = gl.getUniformLocation(q, 'uScreen');
    G.qvao = gl.createVertexArray(); gl.bindVertexArray(G.qvao);
    G.qvbo = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, G.qvbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
    G.qtex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, G.qtex);
    var qf = scale > 1 ? gl.LINEAR : gl.NEAREST;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, qf); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, qf);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindVertexArray(G.vao);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    return 1;
  },

  glide_gl_vw: function () { return Module.__glide ? Module.__glide.vw : 640; },

  glide_gl_tex_create: function (w, h, levels, ptrs) {
    var G = Module.__glide, gl = G.gl;
    var t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    var lw = w, lh = h;
    for (var l = 0; l < levels; l++) {
      var p = HEAP32[(ptrs >> 2) + l];
      gl.texImage2D(gl.TEXTURE_2D, l, gl.RGBA8, lw, lh, 0, gl.RGBA, gl.UNSIGNED_BYTE, HEAPU8.subarray(p, p + lw * lh * 4));
      lw = Math.max(1, lw >> 1); lh = Math.max(1, lh >> 1);
    }
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAX_LEVEL, levels - 1);
    var id = G.freeIds.length ? G.freeIds.pop() : G.tex.length;
    G.tex[id] = { t: t, levels: levels, filt: -1, cs: -1, ct: -1 };
    G.cur = {};
    return id;
  },

  glide_gl_tex_delete: function (id) {
    var G = Module.__glide;
    if (!G || !G.tex[id]) return;
    G.gl.deleteTexture(G.tex[id].t);
    G.tex[id] = null;
    G.freeIds.push(id);
    G.cur = {};
  },

  glide_gl_fog: function (colorPtr, tablePtr) {
    var G = Module.__glide, gl = G.gl;
    G.cur = {};
    gl.useProgram(G.prog);
    gl.uniform3f(G.u.uFogColor, HEAPF32[colorPtr >> 2], HEAPF32[(colorPtr >> 2) + 1], HEAPF32[(colorPtr >> 2) + 2]);
    gl.uniform1fv(G.u.uFogTable, HEAPF32.subarray(tablePtr >> 2, (tablePtr >> 2) + 64));
  },

  glide_gl_draw: function (prim, vptr, nverts, sptr) {
    if (Module.noRaster) return;
    var G = Module.__glide, gl = G.gl, s = sptr >> 2, U = G.u, C = G.cur;
    G.draws = (G.draws || 0) + 1; G.verts = (G.verts || 0) + nverts;
    if (!C.bound) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
      gl.useProgram(G.prog);
      gl.bindVertexArray(G.vao);
      gl.bindBuffer(gl.ARRAY_BUFFER, G.vbo);
      C.bound = true;
    }
    var texId = HEAP32[s];
    var T = texId > 0 ? G.tex[texId] : null;
    if (T) {
      if (C.tex !== texId) { gl.bindTexture(gl.TEXTURE_2D, T.t); C.tex = texId; }
      var filt = HEAP32[s + 22] | (G.forceFilter && T.levels > 1 && !HEAP32[s + 25] ? 1 : 0), cs = HEAP32[s + 23], ct = HEAP32[s + 24];
      if (T.filt !== filt) {
        T.filt = filt;
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filt ? gl.LINEAR : gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, T.levels > 1 ? (filt ? gl.LINEAR_MIPMAP_LINEAR : gl.NEAREST_MIPMAP_NEAREST) : (filt ? gl.LINEAR : gl.NEAREST));
        if (G.aniso && filt && T.levels > 1) gl.texParameterf(gl.TEXTURE_2D, G.aniso.TEXTURE_MAX_ANISOTROPY_EXT, 4);
      }
      if (T.cs !== cs) { T.cs = cs; gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, cs ? gl.CLAMP_TO_EDGE : gl.REPEAT); }
      if (T.ct !== ct) { T.ct = ct; gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, ct ? gl.CLAMP_TO_EDGE : gl.REPEAT); }
    }
    var texOn = T ? 1 : 0;
    if (C.texOn !== texOn) { gl.uniform1i(U.uTexOn, texOn); C.texOn = texOn; }
    // combine state: compare the int block 1..10 and const floats as a key
    var key = HEAP32[s + 1] + ',' + HEAP32[s + 2] + ',' + HEAP32[s + 3] + ',' + HEAP32[s + 4] + ',' + HEAP32[s + 5] + ',' +
      HEAP32[s + 6] + ',' + HEAP32[s + 7] + ',' + HEAP32[s + 8] + ',' + HEAP32[s + 9] + ',' + HEAP32[s + 10];
    if (C.comb !== key) {
      gl.uniform4i(U.uCC, HEAP32[s + 1], HEAP32[s + 2], HEAP32[s + 3], HEAP32[s + 4]);
      gl.uniform1i(U.uCCInv, HEAP32[s + 5]);
      gl.uniform4i(U.uAC, HEAP32[s + 6], HEAP32[s + 7], HEAP32[s + 8], HEAP32[s + 9]);
      gl.uniform1i(U.uACInv, HEAP32[s + 10]);
      C.comb = key;
    }
    var c0 = HEAPF32[s + 26], c1 = HEAPF32[s + 27], c2 = HEAPF32[s + 28], c3 = HEAPF32[s + 29];
    if (C.c0 !== c0 || C.c1 !== c1 || C.c2 !== c2 || C.c3 !== c3) {
      gl.uniform4f(U.uConst, c0 / 255, c1 / 255, c2 / 255, c3 / 255);
      C.c0 = c0; C.c1 = c1; C.c2 = c2; C.c3 = c3;
    }
    var fog = HEAP32[s + 19] & 0xff;
    fog = fog === 1 ? 1 : (fog === 2 ? 2 : 0);
    if (C.fog !== fog) { gl.uniform1i(U.uFog, fog); C.fog = fog; }
    var chroma = HEAP32[s + 20], ck = HEAP32[s + 21];
    if (C.chroma !== chroma || C.ck !== ck) {
      gl.uniform1i(U.uChroma, chroma);
      gl.uniform3i(U.uChromaKey, (ck >> 11) & 31, (ck >> 5) & 63, ck & 31);
      C.chroma = chroma; C.ck = ck;
    }
    var bs = HEAP32[s + 11], bd = HEAP32[s + 12];
    var bkey = bs * 64 + bd;
    if (C.blend !== bkey) {
      if (bs === 4 && bd === 0) gl.disable(gl.BLEND);
      else {
        gl.enable(gl.BLEND);
        var src = [gl.ZERO, gl.SRC_ALPHA, gl.DST_COLOR, gl.ONE, gl.ONE, gl.ONE_MINUS_SRC_ALPHA, gl.ONE_MINUS_DST_COLOR, gl.ZERO];
        var dst = [gl.ZERO, gl.SRC_ALPHA, gl.SRC_COLOR, gl.ONE, gl.ONE, gl.ONE_MINUS_SRC_ALPHA, gl.ONE_MINUS_SRC_COLOR, gl.ZERO];
        var sf = bs === 15 ? gl.SRC_ALPHA_SATURATE : (src[bs] !== undefined ? src[bs] : gl.ONE);
        var df = bd === 15 ? gl.ONE : (dst[bd] !== undefined ? dst[bd] : gl.ZERO);
        gl.blendFunc(sf, df);
      }
      C.blend = bkey;
    }
    var dmode = HEAP32[s + 15], dfunc = HEAP32[s + 16], dmask = HEAP32[s + 17];
    var dkey = (dmode !== 0 ? 1 : 0) * 1000 + dfunc * 10 + (dmode !== 0 && dmask !== 0 ? 1 : 0);
    if (C.depth !== dkey) {
      if (dmode === 0) gl.disable(gl.DEPTH_TEST);
      else { gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.NEVER + dfunc); }
      gl.depthMask(dmode !== 0 && dmask !== 0);
      C.depth = dkey;
    }
    var cm = HEAP32[s + 18] !== 0;
    if (C.cm !== cm) { gl.colorMask(cm, cm, cm, cm); C.cm = cm; }
    gl.bufferData(gl.ARRAY_BUFFER, HEAPF32.subarray(vptr >> 2, (vptr >> 2) + nverts * 11), gl.STREAM_DRAW);
    gl.drawArrays(prim === 1 ? gl.LINES : (prim === 0 ? gl.POINTS : gl.TRIANGLES), 0, nverts);
  },

  glide_gl_clear: function (r, g, b, depth, colorMask, depthMask) {
    var G = Module.__glide, gl = G.gl;
    G.cur = {};
    gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
    var bits = 0;
    gl.colorMask(true, true, true, true);
    if (colorMask) { gl.clearColor(r / 255, g / 255, b / 255, 1); bits |= gl.COLOR_BUFFER_BIT; }
    if (depthMask) { gl.depthMask(true); gl.clearDepth(depth); bits |= gl.DEPTH_BUFFER_BIT; }
    if (bits) gl.clear(bits);
  },

  glide_gl_swap: function (pillarbox) {
    var G = Module.__glide, gl = G.gl;
    G.cur = {};
    G.frames = (G.frames || 0) + 1;
    if (pillarbox && G.vw > G.w) {
      /* menus are 4:3 in the centre: keep the side bars black */
      var side = ((G.vw - G.w) / 2) * G.scale;
      gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
      gl.enable(gl.SCISSOR_TEST);
      gl.colorMask(true, true, true, true);
      gl.clearColor(0, 0, 0, 1);
      gl.scissor(0, 0, Math.ceil(side), G.H); gl.clear(gl.COLOR_BUFFER_BIT);
      gl.scissor(G.W - Math.ceil(side), 0, Math.ceil(side), G.H); gl.clear(gl.COLOR_BUFFER_BIT);
      gl.disable(gl.SCISSOR_TEST);
    }
    /* resolve MSAA into an RGBA8 texture first: a multisampled blit cannot convert to the canvas format */
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.fbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, G.rfbo);
    gl.blitFramebuffer(0, 0, G.W, G.H, 0, 0, G.W, G.H, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.rfbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, G.W, G.H, 0, 0, G.W, G.H, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
    var vw = G.calcVw();
    if (vw !== G.vw) G.alloc(vw);
    return G.vw;
  },

  // draw an RGBA image (alpha 0 = keep) over the back buffer at x,y
  glide_gl_blit: function (x, y, dw, dh, w, h, ptr) {
    var G = Module.__glide, gl = G.gl;
    G.cur = {};
    gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
    gl.useProgram(G.qprog);
    gl.bindVertexArray(G.qvao);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, G.qtex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, HEAPU8.subarray(ptr, ptr + w * h * 4));
    gl.uniform4f(G.qRect, x, y, dw, dh);
    gl.uniform2f(G.qScreen, G.vw, G.h);
    gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST); gl.depthMask(false); gl.colorMask(true, true, true, true);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindVertexArray(G.vao);
    gl.useProgram(G.prog);
    G.cur.tex = -1;
  },

  // read back the back buffer as RGBA (top-down rows)
  glide_gl_read: function (x, y, w, h, ptr) {
    var G = Module.__glide, gl = G.gl;
    G.cur = {};
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.fbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, G.rfbo);
    gl.blitFramebuffer(0, 0, G.W, G.H, 0, 0, G.W, G.H, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.rfbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, G.sfbo);
    var ox = Math.round((G.vw - G.w) / 2 * G.scale);
    gl.blitFramebuffer(ox, 0, ox + G.w * G.scale, G.H, 0, 0, G.w, G.h, gl.COLOR_BUFFER_BIT, gl.LINEAR);
    gl.bindFramebuffer(gl.FRAMEBUFFER, G.sfbo);
    gl.readPixels(x, y, w, h, gl.RGBA, gl.UNSIGNED_BYTE, HEAPU8.subarray(ptr, ptr + w * h * 4));
    gl.bindFramebuffer(gl.FRAMEBUFFER, G.fbo);
  },
});
