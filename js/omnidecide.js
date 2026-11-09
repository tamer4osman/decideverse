/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * omnidecide.js - JavaScript API over the OmniDecide WebAssembly module.
 *
 * UMD entry point.  The factory is invoked EXACTLY ONCE per host
 * environment and both exported members (`OmniDecide` and
 * `OmniDecideError`) are taken from the SAME returned API object, so
 * `error instanceof OmniDecideError` behaves identically in Node and
 * in the browser (invoking the factory twice would create two distinct
 * error classes and break the identity check - the defect was fixed
 * during pre-release hardening and is covered by regression tests).
 *
 * Node/CommonJS:  const OmniDecide = require('js/omnidecide.js');
 * ESM:            import OmniDecide from 'js/omnidecide.js';
 * Browser:        <script src="dist/omnidecide.js"></script>
 *                 <script src="js/omnidecide.js"></script>
 *                 -> window.OmniDecide / window.OmniDecideError
 */

(function (root, factory) {
    if (typeof module === 'object' && typeof module.exports === 'object') {
        /* Node/CommonJS (also the interop target for ESM imports). */
        module.exports = factory();
    } else if (typeof define === 'function' && define.amd) {
        /* AMD: single call, both members resolved from one API object. */
        define(factory);
    } else {
        /* Browser globals: invoke the factory once. */
        var api = factory();
        root.OmniDecide = api.OmniDecide;
        root.OmniDecideError = api.OmniDecideError;
    }
})(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var INTEGRATION_VERSION = '1.0.0';

    /* odk_status codes from omnidecide.h. */
    var STATUS = {
        OK: 0,
        ARGUMENT: 1,
        INFERENCE: 2,
        MEMORY: 3,
        INTERNAL: 4
    };

    /**
     * Error class thrown by every failing call of this API.
     * `status` is the kernel odk_status; `message` the deterministic
     * diagnostic from the C wrapper.
     */
    function OmniDecideError(status, message) {
        var err = new Error(message);
        err.name = 'OmniDecideError';
        err.status = status;
        Object.setPrototypeOf(err, OmniDecideError.prototype);
        return err;
    }
    OmniDecideError.prototype = Object.create(Error.prototype, {
        constructor: { value: OmniDecideError, enumerable: false },
        name: { value: 'OmniDecideError', enumerable: false }
    });

    function isEmscriptenModule(mod) {
        return !!(mod &&
                  typeof mod._odkw_evaluate === 'function' &&
                  typeof mod._odkw_kernel_create === 'function' &&
                  typeof mod.stringToUTF8 === 'function' &&
                  typeof mod.UTF8ToString === 'function');
    }

    function writeCString(mod, text) {
        var bytes = mod.lengthBytesUTF8(text) + 1;
        var ptr = mod._malloc(bytes);
        if (!ptr) {
            throw new OmniDecideError(STATUS.MEMORY,
                'omnidecide.js: out of memory writing string argument');
        }
        mod.stringToUTF8(text, ptr, bytes);
        return ptr;
    }

    function lastError(mod) {
        return mod.UTF8ToString(mod._odkw_last_error());
    }

    /* Locate the Emscripten module factory for this environment. */
    function loadModuleFactory() {
        /* Node/CommonJS (js/omnidecide.js is CJS, so require exists). */
        if (typeof require === 'function' && typeof module !== 'undefined' &&
            typeof module.exports === 'object') {
            try {
                return require('../dist/omnidecide.js');
            } catch (e) {
                throw new Error(
                    'omnidecide.js: cannot load dist/omnidecide.js (' + e.message +
                    '). Run ./build.sh to produce the WebAssembly artifacts.');
            }
        }
        /* Browser: dist/omnidecide.js must be loaded first.  Resolve the
         * global object explicitly (the UMD wrapper's `root` parameter is
         * not in scope inside this factory). */
        var g = (typeof self !== 'undefined') ? self :
                (typeof globalThis !== 'undefined') ? globalThis : undefined;
        var factoryFn = g ? g.createOmniDecideModule : null;
        if (typeof factoryFn !== 'function') {
            throw new Error(
                'omnidecide.js: Emscripten loader not found. Include ' +
                'dist/omnidecide.js before js/omnidecide.js.');
        }
        return factoryFn;
    }

    /**
     * Kernel handle.  All decision methods return the parsed envelope:
     * the upstream five-key decision payload, plus `meta` accounting
     * when requested.  Failings throw OmniDecideError.
     */
    function Kernel(mod, handle) {
        this._mod = mod;
        this._handle = handle;
        this._closed = false;
    }

    Object.defineProperties(Kernel.prototype, {
        kernelVersion: {
            get: function () {
                this._assertOpen();
                return this._mod.UTF8ToString(this._mod._odkw_version());
            }
        },
        backend: {
            get: function () {
                this._assertOpen();
                return this._mod.UTF8ToString(this._mod._odkw_backend());
            }
        },
        closed: {
            get: function () { return this._closed; }
        }
    });

    Kernel.prototype._assertOpen = function () {
        if (this._closed) {
            throw new OmniDecideError(STATUS.ARGUMENT,
                'omnidecide.js: kernel has been released');
        }
    };

    /** Run one request; returns the parsed envelope object. */
    Kernel.prototype.evaluate = function (request) {
        var raw = this.evaluateRaw(request);
        return JSON.parse(raw);
    };

    /**
     * Run one request; returns the raw envelope STRING.  The envelope
     * is produced by the C wrapper (odkw_evaluate), so the identical
     * request produces byte-identical output on every host.
     */
    Kernel.prototype.evaluateRaw = function (request) {
        this._assertOpen();
        if (!request || typeof request !== 'object') {
            throw new OmniDecideError(STATUS.ARGUMENT,
                'omnidecide.js: request object required');
        }
        var mod = this._mod;
        var primitive = typeof request.primitive === 'string' ?
            request.primitive : '';
        var instruction = typeof request.instruction === 'string' ?
            request.instruction : '';
        var context = typeof request.context === 'string' ? request.context : '';
        var options = Array.isArray(request.options) ? request.options : null;
        var topK = typeof request.topK === 'number' ?
            Math.floor(request.topK) : 1;
        var caps = request.caps || {};
        var flags = request.meta ? 1 : 0;

        var primPtr = 0, instrPtr = 0, ctxPtr = 0, optArrayPtr = 0;
        var optPtrs = [], envPtr = 0, out = null, i;

        try {
            primPtr = writeCString(mod, primitive);
            instrPtr = writeCString(mod, instruction);
            ctxPtr = writeCString(mod, context);
            if (options !== null) {
                for (i = 0; i < options.length; i++) {
                    optPtrs.push(writeCString(mod,
                        typeof options[i] === 'string' ? options[i] : ''));
                }
                var bytesPerPtr = 4;
                optArrayPtr = mod._malloc(bytesPerPtr * optPtrs.length);
                if (!optArrayPtr) {
                    throw new OmniDecideError(STATUS.MEMORY,
                        'omnidecide.js: out of memory writing options array');
                }
                var heap = new Uint32Array(mod.HEAPU32.buffer,
                                           optArrayPtr, optPtrs.length);
                for (i = 0; i < optPtrs.length; i++) {
                    heap[i] = optPtrs[i];
                }
            }

            envPtr = mod._odkw_evaluate(
                this._handle, primPtr, instrPtr,
                optArrayPtr, options !== null ? options.length : 0,
                ctxPtr, topK,
                caps.maxRanked | 0, caps.maxSnippetChars | 0,
                caps.maxTotalChars | 0, flags);

            if (!envPtr) {
                throw new OmniDecideError(STATUS.MEMORY, lastError(mod));
            }
            out = mod.UTF8ToString(envPtr);
        } finally {
            if (envPtr) mod._odkw_free(envPtr);
            if (optArrayPtr) mod._free(optArrayPtr);
            for (i = 0; i < optPtrs.length; i++) mod._free(optPtrs[i]);
            if (ctxPtr) mod._free(ctxPtr);
            if (instrPtr) mod._free(instrPtr);
            if (primPtr) mod._free(primPtr);
        }

        var envelope;
        try {
            envelope = JSON.parse(out);
        } catch (e) {
            throw new OmniDecideError(STATUS.INTERNAL,
                'omnidecide.js: malformed envelope from wrapper');
        }
        if (envelope && envelope.error) {
            var failErr = new OmniDecideError(envelope.error.status,
                                              envelope.error.message);
            /* Attach the raw C-authored envelope for tooling and parity
             * verification (the exact bytes the wrapper produced). */
            failErr.envelope = out;
            throw failErr;
        }
        return out;
    };

    /* Convenience wrappers over the six upstream primitives. */
    Kernel.prototype.evaluateChoice = function (instruction, options,
                                                context, opts) {
        return this.evaluate(assign({ primitive: 'choice', instruction: instruction,
            options: options, context: context }, opts));
    };
    Kernel.prototype.evaluateScore = function (instruction, rubric,
                                               context, opts) {
        return this.evaluate(assign({ primitive: 'score', instruction: instruction,
            options: rubric, context: context }, opts));
    };
    Kernel.prototype.evaluateNoul = function (statement, context, opts) {
        return this.evaluate(assign({ primitive: 'noul', instruction: statement,
            context: context }, opts));
    };
    Kernel.prototype.evaluateRank = function (instruction, options,
                                              context, opts) {
        return this.evaluate(assign({ primitive: 'rank', instruction: instruction,
            options: options, context: context }, opts));
    };
    Kernel.prototype.evaluateCompare = function (instruction, options,
                                                 context, opts) {
        return this.evaluate(assign({ primitive: 'compare', instruction: instruction,
            options: options, context: context }, opts));
    };
    Kernel.prototype.evaluateExtract = function (instruction, context, topK,
                                                 opts) {
        return this.evaluate(assign({ primitive: 'extract', instruction: instruction,
            context: context, topK: topK }, opts));
    };

    function assign(target, extra) {
        if (extra) {
            for (var k in extra) {
                if (Object.prototype.hasOwnProperty.call(extra, k)) {
                    target[k] = extra[k];
                }
            }
        }
        return target;
    }

    /** Release the kernel.  Idempotent. */
    Kernel.prototype.free = function () {
        if (!this._closed) {
            this._mod._odkw_kernel_free(this._handle);
            this._closed = true;
        }
    };

    /** Live kernel handles in the module (leak diagnostics). */
    Kernel.prototype.engineCount = function () {
        return this._mod._odkw_kernel_count();
    };

    /**
     * Benchmark helper: pre-marshals one request, then executes the
     * C evaluation `iterations` times WITHOUT per-call JavaScript
     * string marshaling.  This measures the isolated C decision path
     * (wrapper + kernel), NOT the full JS-to-WASM pipeline - use
     * evaluate() timing for the pipeline number.
     */
    Kernel.prototype.rawLoopMs = function (request, iterations) {
        this._assertOpen();
        var mod = this._mod;
        var primPtr = writeCString(mod, request.primitive || '');
        var instrPtr = writeCString(mod, request.instruction || '');
        var ctxPtr = writeCString(mod, request.context || '');
        var optPtrs = [], optArrayPtr = 0;
        var options = Array.isArray(request.options) ? request.options : [];
        var i;
        try {
            for (i = 0; i < options.length; i++) {
                optPtrs.push(writeCString(mod, options[i]));
            }
            if (optPtrs.length) {
                optArrayPtr = mod._malloc(4 * optPtrs.length);
                var heap = new Uint32Array(mod.HEAPU32.buffer, optArrayPtr,
                                           optPtrs.length);
                for (i = 0; i < optPtrs.length; i++) heap[i] = optPtrs[i];
            }
            var envPtr = 0;
            var t0 = now();
            for (i = 0; i < iterations; i++) {
                envPtr = mod._odkw_evaluate(
                    this._handle, primPtr, instrPtr,
                    optArrayPtr, options.length, ctxPtr, request.topK || 1,
                    0, 0, 0, 0);
                if (!envPtr) {
                    throw new OmniDecideError(STATUS.MEMORY, lastError(mod));
                }
                mod._odkw_free(envPtr);
            }
            return now() - t0;
        } finally {
            if (optArrayPtr) mod._free(optArrayPtr);
            for (i = 0; i < optPtrs.length; i++) mod._free(optPtrs[i]);
            mod._free(ctxPtr);
            mod._free(instrPtr);
            mod._free(primPtr);
        }
    };

    function now() {
        return (typeof performance !== 'undefined' &&
                typeof performance.now === 'function') ?
            performance.now() : Date.now();
    }

    /* Shared Emscripten module instance.  The WASM runtime is loaded
     * exactly once per process/page; every kernel lives in it. */
    var modulePromise = null;

    function getModule(config) {
        if (modulePromise === null) {
            modulePromise = Promise.resolve()
                .then(function () {
                    return loadModuleFactory()(config || {});
                })
                .then(function (mod) {
                    if (!isEmscriptenModule(mod)) {
                        throw new OmniDecideError(STATUS.INTERNAL,
                            'omnidecide.js: Emscripten module lacks the OmniDecide exports');
                    }
                    return mod;
                });
        }
        return modulePromise;
    }

    /**
     * Create a kernel.
     *   create({mode: 'ascii'|'unicode', language: 'en'|'zh'|...})
     * Returns a Promise resolving to a Kernel.  Failures (including
     * unknown mode/language) reject with OmniDecideError.
     */
    function create(opts) {
        opts = opts || {};
        var mode = typeof opts.mode === 'string' ? opts.mode : 'ascii';
        var language = typeof opts.language === 'string' ? opts.language : 'und';

        return getModule(opts.moduleConfig).then(function (mod) {
            var modePtr = writeCString(mod, mode);
            var langPtr = writeCString(mod, language);
            var handle;
            try {
                handle = mod._odkw_kernel_create(modePtr, langPtr);
            } finally {
                mod._free(langPtr);
                mod._free(modePtr);
            }
            if (!handle) {
                throw new OmniDecideError(mod._odkw_last_status(),
                                          lastError(mod));
            }
            return new Kernel(mod, handle);
        });
    }

    var OmniDecide = {
        create: create,
        version: INTEGRATION_VERSION,
        STATUS: STATUS,
        loadModuleFactory: loadModuleFactory
    };

    return { OmniDecide: OmniDecide, OmniDecideError: OmniDecideError };
});
