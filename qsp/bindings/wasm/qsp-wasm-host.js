/* Copyright (C) 2001-2025 Val Argunov (byte AT qsp DOT org) */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// Host calls (wasm_callbacks.c): Module.qspHost[name](...), missing methods are ignored.
// A returned promise suspends the engine (Asyncify) until it settles, the running export then
// completes later (call it with ccall(..., {async: true})). Only the outermost host call can
// suspend, a promise from a nested one (the host calling the API) is ignored.

const hostCalls = {
  qspWasmHostDebug: (str, bytes) => qspHostCall('onDebug', () => [UTF16ToString(str, bytes)]),
  qspWasmHostSetTimer: (msecs) => qspHostCall('onSetTimer', () => [msecs]),
  qspWasmHostRefreshInt: (isForced, isNewLoc) =>
    qspHostCall('onRefreshInt', () => [!!isForced, !!isNewLoc]),
  qspWasmHostSetInputStrText: (text, bytes) =>
    qspHostCall('onSetInputStrText', () => [UTF16ToString(text, bytes)]),
  qspWasmHostShowMessage: (text, bytes) =>
    qspHostCall('onShowMessage', () => [UTF16ToString(text, bytes)]),
  qspWasmHostShowPicture: (file, bytes) =>
    qspHostCall('onShowPicture', () => [UTF16ToString(file, bytes)]),
  qspWasmHostShowWindow: (type, toShow) => qspHostCall('onShowWindow', () => [type, !!toShow]),
  qspWasmHostPlayFile: (file, bytes, volume) =>
    qspHostCall('onPlayFile', () => [UTF16ToString(file, bytes), volume]),
  qspWasmHostIsPlayingFile: (file, bytes) =>
    qspHostCall('onIsPlayingFile', () => [UTF16ToString(file, bytes)], (res) => (res ? 1 : 0)),
  qspWasmHostCloseFile: (file, bytes) =>
    qspHostCall('onCloseFile', () => [UTF16ToString(file, bytes)]),
  qspWasmHostSystem: (cmd, bytes) => qspHostCall('onSystem', () => [UTF16ToString(cmd, bytes)]),
  qspWasmHostSleep: (msecs) => qspHostCall('onSleep', () => [msecs]),
  qspWasmHostGetMSCount: () => qspHostCall('onGetMSCount', () => [], (res) => res | 0),
  qspWasmHostOpenGame: (file, bytes, isNewGame) =>
    qspHostCall('onOpenGame', () => [UTF16ToString(file, bytes), !!isNewGame]),
  qspWasmHostInitGame: (isNewGame) => qspHostCall('onInitGame', () => [!!isNewGame]),
  qspWasmHostOpenGameStatus: (file, bytes) =>
    qspHostCall('onOpenGameStatus', () => [UTF16ToString(file, bytes)]),
  qspWasmHostSaveGameStatus: (file, bytes) =>
    qspHostCall('onSaveGameStatus', () => [UTF16ToString(file, bytes)]),
  qspWasmHostInputBox: (text, bytes, buffer, maxLen) =>
    qspHostCall('onInputBox', () => [UTF16ToString(text, bytes)], (res) =>
      qspWriteString(res, buffer, maxLen),
    ),
  qspWasmHostShowMenu: (items, count) =>
    qspHostCall('onShowMenu', () => [items, count], (res) => (Number.isInteger(res) ? res : -1)),
  qspWasmHostVersion: (param, bytes, buffer, maxLen) =>
    qspHostCall('onVersion', () => [UTF16ToString(param, bytes)], (res) =>
      qspWriteString(res, buffer, maxLen),
    ),
};

for (const name of Object.keys(hostCalls)) {
  hostCalls[`${name}__deps`] = ['$qspHostCall', '$qspWriteString', '$UTF16ToString'];
  hostCalls[`${name}__async`] = true;
}

addToLibrary({
  ...hostCalls,

  $qspHostDepth: 0, // host calls in progress

  // getArgs decodes the arguments only if the host has the method
  $qspHostCall__deps: ['$Asyncify', '$qspHostDepth'],
  $qspHostCall: (name, getArgs, convert) =>
    Asyncify.handleSleep((wakeUp) => {
      const host = Module['qspHost'];
      const done = (res) => {
        let value = 0;
        --qspHostDepth;
        try {
          if (convert) value = convert(res);
        } catch (e) {
          console.error(e);
        }
        wakeUp(value);
      };
      let res;
      ++qspHostDepth;
      if (host && host[name]) {
        try {
          res = host[name](...getArgs());
        } catch (e) {
          console.error(e);
        }
      }
      if (!(res && typeof res.then === 'function')) return done(res);
      if (qspHostDepth > 1) {
        console.error(`QSP: the promise returned by ${name} is ignored, the engine can't wait`);
        res.then(undefined, (e) => console.error(e));
        return done(undefined);
      }
      res.then(done, (e) => {
        console.error(e);
        done(undefined);
      });
    }),

  // A longer string is cut, no answer (null, undefined) leaves the buffer empty
  $qspWriteString__deps: ['$stringToUTF16'],
  $qspWriteString: (str, buffer, maxLen) => {
    if (str != null) stringToUTF16(String(str), buffer, (maxLen + 1) * 2);
  },

  qspWasmHostCallFunction__deps: [
    '$Asyncify',
    '$qspHostDepth',
    '$UTF16ToString',
    '$qspReadVariants',
    '$qspFromHostValue',
    '$qspWriteVariant',
  ],
  qspWasmHostCallFunction__async: true,
  qspWasmHostCallFunction: (name, bytes, args, count, result) =>
    Asyncify.handleSleep((wakeUp) => {
      const host = Module['qspHost'];
      const method = result ? 'onHostFunction' : 'onHostStatement';
      const done = (isOk, res) => {
        --qspHostDepth;
        if (isOk && result) {
          try {
            qspWriteVariant(result, qspFromHostValue(res));
          } catch (e) {
            console.error(e);
            isOk = false;
          }
        }
        wakeUp(isOk ? 1 : 0);
      };
      const fail = (e) => {
        console.error(e);
        done(false);
      };
      let res;
      ++qspHostDepth;
      if (!(host && host[method])) return fail(new Error(`QSP: qspHost.${method} is missing`));
      try {
        res = host[method](UTF16ToString(name, bytes), qspReadVariants(args, count));
      } catch (e) {
        return fail(e);
      }
      if (!(res && typeof res.then === 'function')) return done(true, res);
      if (qspHostDepth > 1) {
        res.then(undefined, (e) => console.error(e));
        return fail(new Error(`QSP: the promise returned by ${method} fails, the engine can't wait`));
      }
      res.then((value) => done(true, value), fail);
    }),

  // Frees what qspWriteVariant allocated, the engine copies the result before
  qspWasmHostFreeValue__deps: ['free'],
  qspWasmHostFreeValue: (variant) => {
    const release = (ptr) => {
      const type = HEAP8[ptr + 8];
      if (type === 4 /* QSP_TYPE_STR */) {
        _free(HEAPU32[ptr >> 2]);
      } else if (type === 1 /* QSP_TYPE_TUPLE */) {
        const items = HEAPU32[ptr >> 2];
        const count = HEAP32[(ptr >> 2) + 1];
        for (let i = 0; i < count; ++i) release(items + i * 12);
        _free(items);
      }
      HEAPU8.fill(0, ptr, ptr + 12);
    };
    release(variant);
  },

  // QSPVariant (12 bytes): the union { Num | Str, End | Vals, ValsCount }, the type byte at 8,
  // the engine passes the base types only
  $qspReadVariants__deps: ['$UTF16ToString'],
  $qspReadVariants: (ptr, count) => {
    const read = (ptr) => {
      switch (HEAP8[ptr + 8]) {
        case 1: /* QSP_TYPE_TUPLE */ {
          const items = HEAPU32[ptr >> 2];
          return Array.from({ length: HEAP32[(ptr >> 2) + 1] }, (_, i) => read(items + i * 12));
        }
        case 2: /* QSP_TYPE_NUM */
          return HEAP32[ptr >> 2];
        case 4: /* QSP_TYPE_STR */ {
          const str = HEAPU32[ptr >> 2];
          return str ? UTF16ToString(str, HEAPU32[(ptr >> 2) + 1] - str) : '';
        }
        default:
          return undefined;
      }
    };
    return Array.from({ length: count }, (_, i) => read(ptr + i * 12));
  },

  // A result of the host as a value of QSP, throws on unsupported values
  $qspFromHostValue: (value) => {
    if (value == null) return undefined;
    switch (typeof value) {
      case 'number':
        if (Number.isNaN(value)) throw new TypeError('QSP: NaN is not a value of QSP');
        return Math.max(-2147483647, Math.min(2147483647, Math.trunc(value))); // QSP_MAX_BIGINT
      case 'boolean':
        return value ? 1 : 0;
      case 'string':
        return value;
    }
    if (Array.isArray(value)) return Array.from(value, qspFromHostValue);
    throw new TypeError(`QSP: ${Object.prototype.toString.call(value)} is not a value of QSP`);
  },

  // Writes a value of qspFromHostValue to a zeroed variant, the strings & items are allocated
  $qspWriteVariant__deps: ['malloc', '$stringToUTF16'],
  $qspWriteVariant: (ptr, value) => {
    if (value === undefined) return;
    if (typeof value === 'number') {
      HEAP32[ptr >> 2] = value;
      HEAP8[ptr + 8] = 2; /* QSP_TYPE_NUM */
    } else if (typeof value === 'string') {
      const size = value.length * 2;
      const str = _malloc(size + 2);
      stringToUTF16(value, str, size + 2);
      HEAPU32[ptr >> 2] = str;
      HEAPU32[(ptr >> 2) + 1] = str + size;
      HEAP8[ptr + 8] = 4; /* QSP_TYPE_STR */
    } else {
      const items = value.length ? _malloc(value.length * 12) : 0;
      if (items) HEAPU8.fill(0, items, items + value.length * 12);
      HEAPU32[ptr >> 2] = items;
      HEAP32[(ptr >> 2) + 1] = value.length;
      HEAP8[ptr + 8] = 1; /* QSP_TYPE_TUPLE */
      value.forEach((item, i) => qspWriteVariant(items + i * 12, item));
    }
  },
});
