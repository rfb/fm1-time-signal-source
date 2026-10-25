// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Sets the simulator's station clock from the browser. tools/build_linux.py
// appends this to the update page; the SDK-pinned sdk/Simulator.js gives the
// simulator no time source, but both of its cores take 'midi' messages, and
// the station's own SysEx (app/transfer.c) sets the clock:
// - the preview core (globalThis.FM1SimulatorCore) runs on the page, stepped
//   by a timer, until sound is turned on;
// - then the 'fm1-engine' audio worklet takes over.
// The simulator renders 44100 frames per second, so the rate is set to that.
// The clock is set at start, at :20 of every minute (clear of the
// announcements; background tabs slow the preview's timer) and whenever the
// page becomes visible again.
(() => {
  const groups = (value, count) => {
    const out = [];
    for (let i = 0; i < count; i++) { out.push(value & 127); value = Math.floor(value / 128); }
    return out;
  };
  const sysex = (command, data) => ({ type: 'midi', data: new Uint8Array([0xf0, 0x7d, 0x54, 0x53, 0x01, command, ...data, 0xf7]) });
  let target;
  const sync = () => {
    if (!target) return;
    try {
      target(sysex(0x03, groups(44100000, 4)));             // rate, millihertz
      target(sysex(0x01, groups(Date.now() % 86400000, 4))); // UTC, ms of day
    } catch (error) { console.warn('Simulator clock:', error); }
  };
  setInterval(() => { if (new Date().getUTCSeconds() === 20) sync(); }, 1000);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) sync(); });

  const Core = globalThis.FM1SimulatorCore;
  if (Core) globalThis.FM1SimulatorCore = class extends Core {
    constructor(...args) {
      super(...args);
      if (!target || target.preview) {
        target = message => this.command(message);
        target.preview = true;
        sync();
      }
    }
  };
  const Node = globalThis.AudioWorkletNode;
  if (Node) globalThis.AudioWorkletNode = class extends Node {
    constructor(context, name, options) {
      super(context, name, options);
      if (name !== 'fm1-engine') return;
      this.port.addEventListener('message', event => {
        if (event.data?.type !== 'ready') return;
        target = message => this.port.postMessage(message);
        sync();
      });
    }
  };
})();
