'use strict';

// The two recordings must have matching resolution, camera movement and timing.
// The player keeps them at the same playback position and stops at the shorter end.
(() => {
  const on = document.getElementById('videoOn');
  const off = document.getElementById('videoOff');
  const compare = document.getElementById('compare');
  const split = document.getElementById('compareSlider');
  const playButton = document.getElementById('playButton');
  const playText = document.getElementById('playText');
  const playIcon = document.getElementById('playIcon');
  const restartButton = document.getElementById('restartButton');
  const timeline = document.getElementById('timeline');
  const timeDisplay = document.getElementById('timeDisplay');
  const zoomSelect = document.getElementById('zoomSelect');
  const status = document.getElementById('playbackStatus');

  let ready = false;
  let playing = false;
  let seeking = false;
  let duration = 0;
  let tickId = null;

  function formatTime(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) return '0:00';
    const value = Math.floor(seconds);
    return `${Math.floor(value / 60)}:${String(value % 60).padStart(2, '0')}`;
  }
  function syncTo(time) {
    if (!ready) return;
    const clipped = Math.min(Math.max(time, 0), Math.max(0, duration - 0.03));
    // Assigning currentTime only when necessary avoids needless seek churn.
    for (const video of [on, off]) {
      if (Math.abs(video.currentTime - clipped) > 0.04) video.currentTime = clipped;
    }
    updateTime(clipped);
  }
  function updateTime(time = on.currentTime) {
    const seconds = Math.min(Math.max(time || 0, 0), duration);
    if (!seeking) timeline.value = String(Math.round(seconds / duration * 1000));
    timeline.style.setProperty('--progress', `${seconds / duration * 100}%`);
    timeDisplay.textContent = `${formatTime(seconds)} / ${formatTime(duration)}`;
  }
  function setPlayUI() {
    playButton.setAttribute('aria-label', playing ? 'Pause both videos' : 'Play both videos');
    playText.textContent = playing ? 'Pause' : 'Play';
    playIcon.innerHTML = playing
      ? '<rect x="6" y="5" width="4" height="14"/><rect x="14" y="5" width="4" height="14"/>'
      : '<path d="M8 5v14l11-7z"/>';
  }
  function stopTick() {
    if (tickId !== null) { cancelAnimationFrame(tickId); tickId = null; }
  }
  function pause() {
    playing = false;
    on.pause(); off.pause(); stopTick(); setPlayUI();
  }
  function tick() {
    if (!playing) return;
    if (on.currentTime >= duration - 0.05 || off.currentTime >= duration - 0.05) {
      syncTo(0);
      // Keep the two recordings looping together (native independent loops would drift).
    }
    if (!on.seeking && !off.seeking && Math.abs(on.currentTime - off.currentTime) > 0.10) {
      off.currentTime = on.currentTime;
    }
    updateTime();
    tickId = requestAnimationFrame(tick);
  }
  async function play() {
    if (!ready || playing) return;
    if (on.currentTime >= duration - 0.06) syncTo(0);
    try {
      await Promise.all([on.play(), off.play()]);
      playing = true;
      status.textContent = 'Playing synchronized captures. Drag the vertical divider to compare.';
      setPlayUI();
      stopTick(); tickId = requestAnimationFrame(tick);
    } catch (_) {
      pause();
      status.textContent = 'Playback was blocked or a video could not be decoded. Try pressing Play again or using H.264 MP4 files.';
    }
  }
  function markReadyIfPossible() {
    if (ready || !on.loadedMetadataReady || !off.loadedMetadataReady) return;
    if (!Number.isFinite(on.duration) || !Number.isFinite(off.duration) || on.duration <= 0 || off.duration <= 0) return;
    duration = Math.min(on.duration, off.duration);
    ready = true;
    compare.dataset.ready = 'true';
    compare.setAttribute('aria-busy', 'false');
    playButton.disabled = false; restartButton.disabled = false; timeline.disabled = false;
    if (Math.abs(on.duration - off.duration) > 0.3) {
      status.textContent = 'Loaded: recordings differ in duration. Only the overlapping portion will be shown; align clip start times for a fair comparison.';
    } else {
      status.textContent = 'Ready. Play both recordings, drag the divider, or pause and zoom.';
    }
    updateTime(0);
  }
  [on, off].forEach((video) => {
    video.addEventListener('loadedmetadata', () => {
      video.loadedMetadataReady = true;
      markReadyIfPossible();
    });
    video.addEventListener('error', () => {
      if (!ready) status.textContent = 'Video files not loaded. Add docs/media/dlaa-off.mp4 and docs/media/dlaa-on.mp4, then refresh the page.';
      else { pause(); status.textContent = 'A recording failed during playback. Check the file format and encoding.'; }
    });
  });

  split.addEventListener('input', () => compare.style.setProperty('--split', `${split.value}%`));
  zoomSelect.addEventListener('change', () => compare.style.setProperty('--zoom', zoomSelect.value));
  playButton.addEventListener('click', () => playing ? pause() : play());
  restartButton.addEventListener('click', () => {
    if (!ready) return;
    pause(); syncTo(0);
    status.textContent = 'Rewound to the start of both recordings.';
  });
  timeline.addEventListener('pointerdown', () => { seeking = true; });
  timeline.addEventListener('input', () => {
    if (!ready) return;
    const value = Number(timeline.value) / 1000 * duration;
    timeDisplay.textContent = `${formatTime(value)} / ${formatTime(duration)}`;
    timeline.style.setProperty('--progress', `${Number(timeline.value) / 10}%`);
    syncTo(value);
  });
  function finishSeek() {
    if (!ready) return;
    seeking = false;
    syncTo(Number(timeline.value) / 1000 * duration);
  }
  timeline.addEventListener('pointerup', finishSeek);
  timeline.addEventListener('change', finishSeek); // keyboard and touch fallback
  document.addEventListener('visibilitychange', () => { if (document.hidden) pause(); });
  window.addEventListener('pagehide', pause);
  setPlayUI();
})();
