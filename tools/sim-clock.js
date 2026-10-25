// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Sets the simulator's station clock from the browser. tools/build_linux.py
// appends this to the update page; the SDK-pinned sdk/Simulator.js gives the
// simulator no time source, but it forwards 'midi' messages to the audio
// thread, where the station's own SysEx (app/transfer.c) sets the clock.
// The simulator renders 44100 frames per second, so the rate is set to that.
// The clock is set when sound starts and every five minutes after, at :20 of
// a minute, clear of the announcements.
(() => {
  const Native = globalThis.AudioWorkletNode;
  if (!Native) return;
  const groups = (value, count) => {
    const out = [];
    for (let i = 0; i < count; i++) { out.push(value & 127); value = Math.floor(value / 128); }
    return out;
  };
  const sysex = (command, data) => ({ type: 'midi', data: new Uint8Array([0xf0, 0x7d, 0x54, 0x53, 0x01, command, ...data, 0xf7]) });
  const set = port => {
    port.postMessage(sysex(0x03, groups(44100000, 4)));            // rate, millihertz
    port.postMessage(sysex(0x01, groups(Date.now() % 86400000, 4))); // UTC, ms of day
  };
  globalThis.AudioWorkletNode = class extends Native {
    constructor(context, name, options) {
      super(context, name, options);
      if (name !== 'fm1-engine') return;
      let timer;
      this.port.addEventListener('message', event => {
        if (event.data?.type !== 'ready') return;
        set(this.port);
        clearInterval(timer);
        timer = setInterval(() => { if (new Date().getUTCMinutes() % 5 === 0 && new Date().getUTCSeconds() === 20) set(this.port); }, 1000);
      });
    }
  };
})();
