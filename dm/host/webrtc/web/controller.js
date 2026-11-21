// WebRTC Streamer & Remote Gamepad Controller Client

const FLAT_ICONS = {
  stream: `<svg class="ui-icon" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 21h8M12 17v4"/></svg>`,
  controller: `<svg class="ui-icon" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 11h4M8 9v4M15 11h.01M18 11h.01M17.3 5H6.7a4 4 0 0 0-4 3.6L2 15a4 4 0 0 0 4 4c1.2 0 2.3-.5 3-1.4L10.5 16h3l1.5 1.6A4 4 0 0 0 18 19a4 4 0 0 0 4-4l-.7-6.4A4 4 0 0 0 17.3 5z"/></svg>`,
  gear: `<svg class="ui-icon" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>`,
  wheel: `<svg class="ui-icon" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3"/><path d="M12 3v6M3.5 16.5l6-3M20.5 16.5l-6-3"/></svg>`,
  crosshair: `<svg class="ui-icon" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="2" fill="currentColor"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/></svg>`,
  soundOn: `<svg class="ui-icon" viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"/></svg>`,
  soundMuted: `<svg class="ui-icon" viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><line x1="23" y1="9" x2="17" y2="15"/><line x1="17" y1="9" x2="23" y2="15"/></svg>`,
  fullscreen: `<svg class="ui-icon" viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><path d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"/></svg>`,
  exitFullscreen: `<svg class="ui-icon" viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><path d="M8 3v3a2 2 0 0 1-2 2H3m18 0h-3a2 2 0 0 1-2-2V3m0 18v-3a2 2 0 0 1 2-2h3M3 16h3a2 2 0 0 1 2 2v3"/></svg>`
};

class WebRTCController {
  constructor() {
    this.playerId = 0;
    this.operatingMode = 'full-stream'; // Default: Full Stream (Video + Pad)
    this.streamResolution = '480'; // Default: 480p (480 or 720)
    this.peerConnection = null;
    this.dataChannel = null;
    this.isConnected = false;

    // Input States
    this.buttons = 0; // bitmask
    this.lx = 0;
    this.ly = 0;
    this.rx = 0;
    this.ry = 0;

    // Gyro / Motion
    this.gyroEnabled = false;
    this.hasMotion = false;
    this.accX = 0;
    this.accY = 0;
    this.accZ = 0;
    this.rotX = 0;
    this.rotY = 0;
    this.rotZ = 0;
    this.gyroZeroRoll = 0;

    // UI Elements
    this.appContainer = document.getElementById('app-container');
    this.videoEl = document.getElementById('game-video');
    this.audioEl = document.getElementById('game-audio');
    this.videoPlaceholder = document.getElementById('video-placeholder');
    this.statusEl = document.getElementById('conn-status');
    this.playerBadge = document.getElementById('player-badge');
    this.setupPanel = document.getElementById('setup-panel');
    this.modeToggleBtn = document.getElementById('mode-toggle-btn');
    this.resToggleBtn = document.getElementById('res-toggle-btn');
    this.statRtt = document.getElementById('stat-rtt');
    this.statFps = document.getElementById('stat-fps');
    this.statGyro = document.getElementById('stat-gyro');

    // FPS Counter
    this.videoFrames = 0;
    this.lastFpsUpdate = performance.now();

    // Desktop vs Mobile Detection
    this.isDesktop = this.checkIsDesktop();
    this.keyboardButtons = 0;
    this.keyboardLx = 0;
    this.keyboardLy = 0;

    if (this.isDesktop) {
      document.body.classList.add('device-desktop');
      if (this.appContainer) this.appContainer.classList.add('device-desktop');
    }

    this.initUI();
    if (!this.isDesktop) {
      this.initTouchControls();
    }
    this.initKeyboardControls();
    this.initGamepad();
    this.startInputLoop();
  }

  checkIsDesktop() {
    const urlParams = new URLSearchParams(window.location.search);
    if (urlParams.get('device') === 'desktop') return true;
    if (urlParams.get('device') === 'mobile') return false;

    const isMobileUA = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent);
    if (isMobileUA) return false;

    const hasFinePointer = window.matchMedia && window.matchMedia('(hover: hover) and (pointer: fine)').matches;
    const hasTouch = ('ontouchstart' in window) || (navigator.maxTouchPoints > 0);

    return !!(hasFinePointer || (!hasTouch && window.innerWidth >= 800));
  }

  initUI() {
    const urlParams = new URLSearchParams(window.location.search);
    if (urlParams.get('mode') === 'controller') {
      this.setMode('controller-only');
    } else {
      this.setMode('full-stream');
    }

    if (urlParams.get('res') === '720' || urlParams.get('res') === '720p') {
      this.setResolution('720');
    } else {
      this.setResolution('480');
    }

    if (urlParams.get('p')) {
      const p = parseInt(urlParams.get('p'), 10);
      if (p >= 1 && p <= 4) {
        this.playerId = p - 1;
        this.playerBadge.textContent = `P${p}`;
        document.querySelectorAll('.player-btn').forEach(b => {
          b.classList.toggle('active', parseInt(b.dataset.player) === this.playerId);
        });
      }
    }

    if (urlParams.get('auto') === '1') {
      setTimeout(() => this.connect(), 400);
    }

    // Header Mode Toggle Button
    if (this.modeToggleBtn) {
      this.modeToggleBtn.addEventListener('click', () => {
        const nextMode = (this.operatingMode === 'full-stream') ? 'controller-only' : 'full-stream';
        this.setMode(nextMode);
      });
    }

    // Header Resolution Toggle Button (480p <-> 720p)
    if (this.resToggleBtn) {
      this.resToggleBtn.addEventListener('click', () => {
        const nextRes = (this.streamResolution === '720') ? '480' : '720';
        this.setResolution(nextRes, true);
      });
    }

    // Modal Mode Select Buttons
    document.querySelectorAll('.mode-btn').forEach(btn => {
      btn.addEventListener('click', (e) => {
        document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
        e.target.classList.add('active');
        this.setMode(e.target.dataset.mode);
      });
    });

    // Modal Resolution Select Buttons
    document.querySelectorAll('.res-btn').forEach(btn => {
      btn.addEventListener('click', (e) => {
        document.querySelectorAll('.res-btn').forEach(b => b.classList.remove('active'));
        e.target.classList.add('active');
        this.setResolution(e.target.dataset.res, true);
      });
    });

    // Modal Player Slot Buttons
    document.querySelectorAll('.player-btn').forEach(btn => {
      btn.addEventListener('click', (e) => {
        document.querySelectorAll('.player-btn').forEach(b => b.classList.remove('active'));
        e.target.classList.add('active');
        this.playerId = parseInt(e.target.dataset.player);
        this.playerBadge.textContent = `P${this.playerId + 1}`;
      });
    });

    // Connect Button
    document.getElementById('connect-btn').addEventListener('click', () => {
      this.connect();
    });

    // Unmute
    const unmuteBtn = document.getElementById('unmute-btn');
    if (unmuteBtn) {
      unmuteBtn.addEventListener('click', () => {
        if (this.audioEl) {
          this.audioEl.muted = !this.audioEl.muted;
          unmuteBtn.innerHTML = this.audioEl.muted ? FLAT_ICONS.soundMuted : FLAT_ICONS.soundOn;
          if (!this.audioEl.muted) {
            this.audioEl.play().catch(() => {});
          }
        }
      });
    }

    // Interaction handler to ensure audio is unblocked on mobile browsers
    document.addEventListener('pointerdown', () => {
      if (this.audioEl && this.audioEl.paused && this.operatingMode !== 'controller-only' && !this.audioEl.muted) {
        this.audioEl.play().catch(() => {});
      }
    });

    // Fullscreen (Native Landscape Fullscreen)
    const fsBtn = document.getElementById('fullscreen-btn');
    if (fsBtn) {
      fsBtn.addEventListener('click', async () => {
        const isFs = document.fullscreenElement || document.webkitFullscreenElement;
        if (!isFs) {
          const docEl = document.documentElement;
          if (docEl.requestFullscreen) {
            await docEl.requestFullscreen().catch(() => {});
          } else if (docEl.webkitRequestFullscreen) {
            await docEl.webkitRequestFullscreen().catch(() => {});
          }

          if (screen.orientation && screen.orientation.lock) {
            screen.orientation.lock('landscape').catch(() => {});
          }
          fsBtn.innerHTML = `${FLAT_ICONS.exitFullscreen}`;
        } else {
          if (document.exitFullscreen) {
            await document.exitFullscreen().catch(() => {});
          } else if (document.webkitExitFullscreen) {
            await document.webkitExitFullscreen().catch(() => {});
          }
          if (screen.orientation && screen.orientation.unlock) {
            screen.orientation.unlock();
          }
          fsBtn.innerHTML = FLAT_ICONS.fullscreen;
        }
      });
    }

    // Gyro Toggle Button
    // Gyro Toggle Buttons (both quick stats and header bar for fullscreen)
    const gyroBtn = document.getElementById('gyro-toggle-btn');
    const gyroHeaderBtn = document.getElementById('gyro-header-btn');
    const calibBtn = document.getElementById('gyro-calib-btn');
    const calibHdrBtn = document.getElementById('gyro-calib-header-btn');

    const toggleGyro = async () => {
      if (!this.gyroEnabled) {
        await this.requestGyroPermission();
      } else {
        this.gyroEnabled = false;
        this.hasMotion = false;
        this.lx = 0;
        this.rotX = 0;
        this.rotY = 0;
        this.rotZ = 0;
        this.updateGyroUI('Gyro: Off', false);
        if (calibBtn) {
          calibBtn.classList.remove('active');
          calibBtn.style.display = 'none';
        }
        if (calibHdrBtn) {
          calibHdrBtn.classList.remove('active');
          calibHdrBtn.style.display = 'none';
        }
      }
    };

    if (gyroBtn) {
      if (!window.isSecureContext) {
        gyroBtn.title = 'Sensor Gyro membutuhkan HTTPS atau flag Chrome';
      }
      gyroBtn.addEventListener('click', toggleGyro);
    }
    if (gyroHeaderBtn) {
      if (!window.isSecureContext) {
        gyroHeaderBtn.title = 'Sensor Gyro membutuhkan HTTPS atau flag Chrome';
      }
      gyroHeaderBtn.addEventListener('click', toggleGyro);
    }

    const triggerRecenter = () => {
      this.recenterGyro();
    };

    if (calibBtn) calibBtn.addEventListener('click', triggerRecenter);
    if (calibHdrBtn) calibHdrBtn.addEventListener('click', triggerRecenter);

    // Keyboard Overlay Toggle (Desktop)
    const kbdToggleBtn = document.getElementById('kbd-toggle-btn');
    const kbdBar = document.getElementById('desktop-keyboard-bar');
    const kbdCloseBtn = document.getElementById('kbd-close-btn');

    if (kbdToggleBtn && kbdBar) {
      kbdToggleBtn.addEventListener('click', () => {
        kbdBar.classList.toggle('hidden');
      });
    }
    if (kbdCloseBtn && kbdBar) {
      kbdCloseBtn.addEventListener('click', () => {
        kbdBar.classList.add('hidden');
      });
    }

    // Video Frame Callback for true video stream FPS
    let fpsRunning = false;
    const startFpsCounter = () => {
      if (fpsRunning) return;
      fpsRunning = true;
      if ('requestVideoFrameCallback' in this.videoEl) {
        const onFrame = () => {
          this.videoFrames++;
          this.videoEl.requestVideoFrameCallback(onFrame);
        };
        this.videoEl.requestVideoFrameCallback(onFrame);
      } else {
        let lastTime = 0;
        this.videoEl.addEventListener('timeupdate', () => {
          if (this.videoEl.currentTime !== lastTime) {
            lastTime = this.videoEl.currentTime;
            this.videoFrames += 2;
          }
        });
      }
    };
    this.videoEl.addEventListener('loadeddata', startFpsCounter);
    this.videoEl.addEventListener('playing', startFpsCounter);

    // Tap on video or placeholder to resume playback if blocked by browser policy
    this.videoEl.addEventListener('click', () => {
      this.videoEl.play().catch(() => {});
    });
    if (this.videoPlaceholder) {
      this.videoPlaceholder.addEventListener('click', () => {
        this.videoEl.play().catch(() => {});
      });
    }
  }

  setMode(mode) {
    this.operatingMode = mode;
    const resRow = document.getElementById('res-form-row');
    if (mode === 'controller-only') {
      this.appContainer.classList.add('mode-controller-only');
      this.appContainer.classList.remove('mode-full-stream');
      if (this.modeToggleBtn) this.modeToggleBtn.innerHTML = `${FLAT_ICONS.controller} <span class="btn-lbl">Pad</span>`;
      if (this.resToggleBtn) this.resToggleBtn.style.display = 'none';
      if (resRow) resRow.style.display = 'none';
      document.querySelectorAll('.mode-btn').forEach(b => {
        b.classList.toggle('active', b.dataset.mode === 'controller-only');
      });
    } else {
      this.appContainer.classList.remove('mode-controller-only');
      this.appContainer.classList.add('mode-full-stream');
      if (this.modeToggleBtn) this.modeToggleBtn.innerHTML = `${FLAT_ICONS.stream} <span class="btn-lbl">Stream</span>`;
      if (this.resToggleBtn) this.resToggleBtn.style.display = '';
      if (resRow) resRow.style.display = '';
      document.querySelectorAll('.mode-btn').forEach(b => {
        b.classList.toggle('active', b.dataset.mode === 'full-stream');
      });
    }
  }

  setResolution(res, notifyHost = false) {
    this.streamResolution = (res === '720' || res === '720p') ? '720' : '480';
    if (this.resToggleBtn) {
      this.resToggleBtn.innerHTML = `${FLAT_ICONS.gear} <span class="btn-lbl">${this.streamResolution}p</span>`;
    }
    document.querySelectorAll('.res-btn').forEach(b => {
      b.classList.toggle('active', b.dataset.res === this.streamResolution);
    });
    if (notifyHost) {
      fetch('/api/resolution', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ resolution: this.streamResolution })
      }).catch(err => console.error('Failed to update resolution:', err));
    }
  }

  async requestGyroPermission() {
    if (!window.isSecureContext) {
      const httpsPort = '8443';
      const httpsUrl = `https://${window.location.hostname}:${httpsPort}/?mode=${(this.operatingMode === 'full-stream') ? 'stream' : 'controller'}&p=${this.playerId + 1}`;
      const userChoice = confirm(
        '[Peringatan] Browser (Android Chrome / Safari) memblokir sensor Gyro pada koneksi HTTP biasa.\n\n' +
        'Pindah ke versi HTTPS agar Gyro aktif?\n' + httpsUrl + '\n\n' +
        'Catatan: Di halaman HTTPS, klik "Advanced" lalu "Proceed to ' + window.location.hostname + ' (unsafe)" untuk mengizinkan sertifikat lokal.'
      );
      if (userChoice) {
        window.location.href = httpsUrl;
        return;
      }
    }

    let granted = false;

    // Check iOS DeviceOrientationEvent
    if (typeof DeviceOrientationEvent !== 'undefined' && typeof DeviceOrientationEvent.requestPermission === 'function') {
      try {
        const res = await DeviceOrientationEvent.requestPermission();
        if (res === 'granted') {
          granted = true;
        } else {
          alert('Izin sensor gerak ditolak di Safari. Izinkan "Motion & Orientation Access" di Pengaturan > Safari.');
          return;
        }
      } catch (e) {
        alert('Gagal meminta izin sensor gerak: ' + e);
        return;
      }
    } else {
      granted = true;
    }

    // Check iOS DeviceMotionEvent
    if (typeof DeviceMotionEvent !== 'undefined' && typeof DeviceMotionEvent.requestPermission === 'function') {
      try {
        await DeviceMotionEvent.requestPermission();
      } catch (e) {}
    }

    if (granted) {
      this.enableGyroListeners();
    }
  }

  updateGyroUI(label, active) {
    const cleanText = (label || '').replace(/[\u{1F300}-\u{1F9FF}\u{2600}-\u{26FF}\u{2700}-\u{27BF}]/gu, '').trim();
    const gyroBtn = document.getElementById('gyro-toggle-btn');
    const gyroHeaderBtn = document.getElementById('gyro-header-btn');
    if (gyroBtn) {
      gyroBtn.innerHTML = `${FLAT_ICONS.wheel} <span class="gyro-stat-lbl">${cleanText || 'Gyro'}</span>`;
      gyroBtn.classList.toggle('active', active);
    }
    if (gyroHeaderBtn) {
      gyroHeaderBtn.innerHTML = `${FLAT_ICONS.wheel} <span class="btn-lbl">${cleanText || 'Gyro'}</span>`;
      gyroHeaderBtn.classList.toggle('active', active);
    }
  }

  recenterGyro() {
    this.gyroZeroRoll = this.currentRoll || 0;
    this.lx = 0;
    this.rotX = 0;
    this.rotY = 0;
    this.rotZ = 0;
    if (navigator.vibrate) navigator.vibrate(25);
    const flashBtn = (id) => {
      const b = document.getElementById(id);
      if (b) {
        const oldHtml = b.innerHTML;
        b.innerHTML = `${FLAT_ICONS.crosshair} <span class="calib-stat-lbl">Zeroed!</span>`;
        setTimeout(() => { b.innerHTML = oldHtml; }, 700);
      }
    };
    flashBtn('gyro-calib-btn');
    flashBtn('gyro-calib-header-btn');
  }

  enableGyroListeners() {
    if (this.isDesktop) return;
    this.gyroEnabled = true;
    this.updateGyroUI('Menunggu Sensor...', true);

    const calibBtn = document.getElementById('gyro-calib-btn');
    if (calibBtn) {
      calibBtn.classList.add('active');
      calibBtn.style.display = 'inline-flex';
    }
    const calibHdrBtn = document.getElementById('gyro-calib-header-btn');
    if (calibHdrBtn) {
      calibHdrBtn.classList.add('active');
    }

    let receivedEvent = false;
    let autoCalibrated = false;

    // Handler for deviceorientation
    const handleOrientation = (e) => {
      if (!this.gyroEnabled) return;
      this.hasMotion = true;
      receivedEvent = true;

      // Robust orientation detection across Safari, Chrome, and PWA
      let angle = 0;
      if (window.screen && window.screen.orientation && typeof window.screen.orientation.angle === 'number') {
        angle = window.screen.orientation.angle;
      } else if (typeof window.orientation === 'number') {
        angle = window.orientation;
      } else if (window.innerWidth > window.innerHeight) {
        angle = 90;
      }

      let rawTilt = 0;
      if (angle === 90) {
        // Landscape (top of phone on the left): tilting wheel left drops top (negative beta)
        rawTilt = (e.beta || 0);
      } else if (angle === 270 || angle === -90) {
        // Landscape (top of phone on the right)
        rawTilt = -(e.beta || 0);
      } else if (angle === 180) {
        // Upside down portrait
        rawTilt = -(e.gamma || 0);
      } else {
        // Portrait (Vertical mode 0 deg): tilting left is negative gamma
        rawTilt = (e.gamma || 0);
      }

      this.currentRoll = rawTilt;

      // Auto-center on first live reading: sets initial resting posture as true 0°
      if (!autoCalibrated) {
        this.gyroZeroRoll = rawTilt;
        autoCalibrated = true;
      }

      let netTilt = rawTilt - (this.gyroZeroRoll || 0);

      // Deadzone 2.0° filters hand resting tremor,
      // while progressive 1.15 curve provides responsive, smooth steering
      const gyroDeadzone = 2.0;
      const gyroMaxAngle = 25.0;
      let normalized = 0;
      const absTilt = Math.abs(netTilt);
      if (absTilt > gyroDeadzone) {
        const span = gyroMaxAngle - gyroDeadzone;
        const linear = Math.min(1.0, (absTilt - gyroDeadzone) / span);
        normalized = Math.sign(netTilt) * Math.pow(linear, 1.15);
      } else {
        normalized = 0;
      }

      const deg = Math.round(absTilt);
      let label = '0°';
      if (absTilt > gyroDeadzone) {
        label = netTilt < 0 ? `◄ ${deg}°` : `${deg}° ►`;
      }
      this.updateGyroUI(label, true);

      // Tilt sets Left Stick X (kart steering) only if not overridden by active touch joystick
      if (!this.touchJoystickActive) {
        this.lx = Math.round(normalized * 32767);
      }
    };

    window.addEventListener('deviceorientation', handleOrientation, true);

    // Also devicemotion for 6-axis accelerations and rotations
    window.addEventListener('devicemotion', (e) => {
      if (!this.gyroEnabled) return;
      receivedEvent = true;
      this.hasMotion = true;
      if (e.accelerationIncludingGravity) {
        this.accX = e.accelerationIncludingGravity.x || 0;
        this.accY = e.accelerationIncludingGravity.y || 0;
        this.accZ = e.accelerationIncludingGravity.z || 0;
      }
      if (e.rotationRate) {
        const deg2rad = Math.PI / 180.0;
        this.rotX = (e.rotationRate.alpha || 0) * deg2rad;
        this.rotY = (e.rotationRate.beta || 0) * deg2rad;
        this.rotZ = (e.rotationRate.gamma || 0) * deg2rad;
      }
      // If within deadzone, zero angular velocity so game never wanders
      if (Math.abs(this.currentRoll - (this.gyroZeroRoll || 0)) <= 2.0) {
        this.rotX = 0;
        this.rotY = 0;
        this.rotZ = 0;
      }
    }, true);

    // Fallback: only if deviceorientation produces no event after 1200ms
    setTimeout(() => {
      if (!receivedEvent && this.gyroEnabled) {
        window.addEventListener('deviceorientationabsolute', handleOrientation, true);
      }
    }, 1200);

    // Timeout check
    setTimeout(() => {
      if (!receivedEvent && this.gyroEnabled) {
        const gyroBtn = document.getElementById('gyro-toggle-btn');
        if (gyroBtn) {
          if (!window.isSecureContext) {
            this.updateGyroUI('Butuh HTTPS', false);
          } else {
            this.updateGyroUI('Sensor: No Event', false);
          }
        }
        console.warn('No deviceorientation event received.');
      }
    }, 2500);
  }

  tuneReceiver(receiver) {
    if (!receiver) return;
    try {
      if ('playoutDelayHint' in receiver) {
        // 0.01s (10ms) provides real-time response while smoothly absorbing Wi-Fi micro-jitter
        receiver.playoutDelayHint = 0.01;
      }
      if ('jitterBufferTarget' in receiver) {
        receiver.jitterBufferTarget = 10;
      }
    } catch (_) {}
  }

  async connect() {
    this.statusEl.textContent = 'Connecting...';
    this.statusEl.className = 'badge badge-disconnected';

    // Prime the video element on user gesture
    if (this.videoEl) {
      this.videoEl.play().catch(() => {});
    }
    if (this.audioEl) {
      this.audioEl.play().catch(() => {});
    }

    if (this.peerConnection) {
      try { this.peerConnection.close(); } catch(e) {}
      this.peerConnection = null;
    }

    const config = { iceServers: [] };
    this.peerConnection = new RTCPeerConnection(config);

    this.peerConnection.onconnectionstatechange = () => {
      console.log('PeerConnection state:', this.peerConnection.connectionState);
      if (this.peerConnection.connectionState === 'connected') {
        this.statusEl.textContent = 'Connected';
        this.statusEl.className = 'badge badge-connected';
      } else if (this.peerConnection.connectionState === 'failed') {
        this.statusEl.textContent = 'Failed';
        this.statusEl.className = 'badge badge-disconnected';
      }
    };

    // Track handler (Independent Video & Audio pipelines for zero lip-sync stall)
    this.peerConnection.ontrack = (event) => {
      console.log('[WebRTC] Received remote track:', event.track.kind);
      const receiver = event.receiver || (this.peerConnection.getReceivers && this.peerConnection.getReceivers().find(r => r.track === event.track));
      this.tuneReceiver(receiver);

      if (event.track.kind === 'video') {
        this.videoEl.srcObject = new MediaStream([event.track]);
        this.videoEl.muted = true;
        if (this.operatingMode !== 'controller-only') {
          this.videoEl.play().catch(e => console.warn('Autoplay video caught:', e));
        }
        if (this.videoPlaceholder) {
          this.videoPlaceholder.style.opacity = '0';
          setTimeout(() => { this.videoPlaceholder.style.display = 'none'; }, 300);
        }
      } else if (event.track.kind === 'audio') {
        if (this.audioEl) {
          this.audioEl.srcObject = new MediaStream([event.track]);
          this.audioEl.muted = (this.operatingMode === 'controller-only');
          if (this.operatingMode !== 'controller-only') {
            this.audioEl.play().catch(e => {
              console.log('Audio autoplay prevented, user interaction will start audio:', e);
            });
          }
        }
      }
    };

    // Continuous 500ms jitter buffer watchdog (keeps jitterBufferTarget at 0ms)
    if (this.watchdogInterval) clearInterval(this.watchdogInterval);
    this.watchdogInterval = setInterval(() => {
      if (this.peerConnection && this.peerConnection.getReceivers) {
        this.peerConnection.getReceivers().forEach(r => this.tuneReceiver(r));
      }
    }, 500);

    // DataChannel for Controller Input
    this.dataChannel = this.peerConnection.createDataChannel('input', {
      ordered: false,
      maxRetransmits: 0
    });
    this.dataChannel.binaryType = 'arraybuffer';

    this.dataChannel.onopen = () => {
      this.isConnected = true;
      this.statusEl.textContent = 'Connected';
      this.statusEl.className = 'badge badge-connected';
      this.setupPanel.style.display = 'none';
      console.log('[WebRTC] Controller input channel open!');
      try { this.dataChannel.send('keyframe'); } catch (_) {}

      if (this.keyframeWatchdog) clearInterval(this.keyframeWatchdog);
      this.keyframeWatchdog = setInterval(() => {
        if (this.isConnected && this.operatingMode === 'full-stream') {
          if (this.videoFrames === 0) {
            if (this.dataChannel && this.dataChannel.readyState === 'open') {
              try { this.dataChannel.send('keyframe'); } catch (_) {}
            }
            if (this.videoEl && this.videoEl.paused) {
              this.videoEl.play().catch(() => {});
            }
          }
        }
      }, 1000);
    };

    this.dataChannel.onclose = () => {
      this.isConnected = false;
      this.statusEl.textContent = 'Disconnected';
      this.statusEl.className = 'badge badge-disconnected';
      this.setupPanel.style.display = 'flex';
    };

    // Add Video & Audio transceivers for media reception
    const wantMedia = (this.operatingMode === 'full-stream');
    if (wantMedia) {
      try {
        this.peerConnection.addTransceiver('video', { direction: 'recvonly' });
        this.peerConnection.addTransceiver('audio', { direction: 'recvonly' });
      } catch (e) {
        console.warn('addTransceiver error:', e);
      }
    }

    const offer = await this.peerConnection.createOffer();
    await this.peerConnection.setLocalDescription(offer);

    // Vanilla ICE wait
    if (this.peerConnection.iceGatheringState !== 'complete') {
      await new Promise((resolve) => {
        let timeout = setTimeout(resolve, 800);
        const checkState = () => {
          if (this.peerConnection && this.peerConnection.iceGatheringState === 'complete') {
            this.peerConnection.removeEventListener('icegatheringstatechange', checkState);
            clearTimeout(timeout);
            resolve();
          }
        };
        this.peerConnection.addEventListener('icegatheringstatechange', checkState);
      });
    }

    // Send SDP Offer to Host
    try {
      const resp = await fetch('/api/offer', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          sdp: this.peerConnection.localDescription.sdp,
          type: this.peerConnection.localDescription.type,
          player: this.playerId,
          resolution: this.streamResolution
        })
      });

      if (!resp.ok) {
        const errJson = await resp.json().catch(() => ({}));
        throw new Error(`Host error ${resp.status}: ${errJson.error || ''}`);
      }

      const answer = await resp.json();
      if (answer.resolution) {
        this.setResolution(answer.resolution, false);
      }
      await this.peerConnection.setRemoteDescription(new RTCSessionDescription({
        sdp: answer.sdp,
        type: answer.type
      }));

      if (typeof answer.player_id === 'number') {
        this.playerId = answer.player_id;
        this.playerBadge.textContent = `P${this.playerId + 1}`;
      }
    } catch (e) {
      console.error('Connection failed:', e);
      this.statusEl.textContent = 'Err: ' + (e.message || 'Failed');
      alert('Connection failed: ' + (e.message || e));
    }
  }

  initTouchControls() {
    this.touchJoystickActive = false;
    const zone = document.getElementById('touch-joystick-zone');
    const knob = document.getElementById('touch-joystick-knob');
    let touchId = null;
    let zoneRect = null;

    const resetKnob = () => {
      knob.style.transform = `translate(0px, 0px)`;
      zone.classList.remove('active');
      this.lx = 0;
      this.ly = 0;
      this.touchJoystickActive = false;
      touchId = null;
    };

    const deadzoneRadius = 20; // Radius deadzone di tengah diperbesar (sebelumnya 12px) agar jari tidak gampang bergeser/belok

    const updateJoystick = (clientX, clientY) => {
      if (!zoneRect) zoneRect = zone.getBoundingClientRect();
      const centerX = zoneRect.left + zoneRect.width / 2;
      const centerY = zoneRect.top + zoneRect.height / 2;
      let dx = clientX - centerX;
      let dy = clientY - centerY;
      const maxRadius = zoneRect.width / 2 - 25; // ~45px
      const dist = Math.hypot(dx, dy);

      // Batasi gerakan knob fisik visual
      let knobX = dx;
      let knobY = dy;
      if (dist > maxRadius) {
        knobX = (dx / dist) * maxRadius;
        knobY = (dy / dist) * maxRadius;
      }

      knob.style.transform = `translate(${knobX}px, ${knobY}px)`;

      // Area Navigasi: Jika belum keluar dari deadzone tengah -> belum ternavigasi
      if (dist <= deadzoneRadius) {
        this.lx = 0;
        this.ly = 0;
      } else {
        // Jika sudah masuk ke area di sekitarnya -> baru tertrigger navigasi
        const activeSpan = maxRadius - deadzoneRadius;
        const normalized = Math.min(1.0, (dist - deadzoneRadius) / activeSpan);
        this.lx = Math.round((dx / dist) * normalized * 32767);
        this.ly = Math.round((-dy / dist) * normalized * 32767);
      }
    };

    // Deteksi sentuhan pada analog maupun area di sekitarnya
    const handleTouchStart = (touch) => {
      if (touchId !== null) return;
      zoneRect = zone.getBoundingClientRect();
      const centerX = zoneRect.left + zoneRect.width / 2;
      const centerY = zoneRect.top + zoneRect.height / 2;
      const distFromCenter = Math.hypot(touch.clientX - centerX, touch.clientY - centerY);

      // Area di sekitar analog (radius hingga lingkaran analog + toleransi)
      if (distFromCenter <= zoneRect.width / 2 + 30) {
        touchId = touch.identifier;
        this.touchJoystickActive = true;
        zone.classList.add('active');
        if (navigator.vibrate) navigator.vibrate(8);
        updateJoystick(touch.clientX, touch.clientY);
      }
    };

    zone.addEventListener('touchstart', (e) => {
      if (touchId !== null) return;
      for (let i = 0; i < e.changedTouches.length; i++) {
        handleTouchStart(e.changedTouches[i]);
        if (touchId !== null) break;
      }
    }, { passive: false });

    // Dukung area di sekitar analog pada kolom kiri agar saat jari masuk area sekitar langsung tertrigger
    const padLeft = document.querySelector('.pad-left');
    if (padLeft) {
      padLeft.addEventListener('touchstart', (e) => {
        if (this.gyroEnabled) return; // Jangan bajak kemudi saat gyro steering aktif!
        if (touchId !== null) return;
        if (e.target.closest('.touch-btn') && !e.target.closest('#touch-joystick-zone')) return;
        for (let i = 0; i < e.changedTouches.length; i++) {
          handleTouchStart(e.changedTouches[i]);
          if (touchId !== null) break;
        }
      }, { passive: true });

      padLeft.addEventListener('touchmove', (e) => {
        if (this.gyroEnabled) return; // Jangan bajak kemudi saat gyro steering aktif!
        if (touchId === null) {
          if (e.target.closest('.touch-btn') && !e.target.closest('#touch-joystick-zone')) return;
          for (let i = 0; i < e.changedTouches.length; i++) {
            handleTouchStart(e.changedTouches[i]);
            if (touchId !== null) break;
          }
        }
      }, { passive: true });
    }

    window.addEventListener('touchmove', (e) => {
      if (touchId === null) return;
      for (let i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === touchId) {
          updateJoystick(e.changedTouches[i].clientX, e.changedTouches[i].clientY);
          break;
        }
      }
    }, { passive: true });

    window.addEventListener('touchend', (e) => {
      if (touchId === null) return;
      for (let i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === touchId) {
          resetKnob();
          break;
        }
      }
    });

    window.addEventListener('touchcancel', (e) => {
      if (touchId === null) return;
      for (let i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === touchId) {
          resetKnob();
          break;
        }
      }
    });

    // Touch Buttons (A, B, X, Y, Triggers, D-pad)
    document.querySelectorAll('.touch-btn').forEach(btn => {
      const bit = parseInt(btn.dataset.btn);
      if (!bit) return;

      btn.addEventListener('touchstart', (e) => {
        e.preventDefault();
        btn.classList.add('active');
        this.buttons |= bit;
        if (navigator.vibrate) navigator.vibrate(10);
      });

      const releaseBtn = (e) => {
        e.preventDefault();
        btn.classList.remove('active');
        this.buttons &= ~bit;
      };

      btn.addEventListener('touchend', releaseBtn);
      btn.addEventListener('touchcancel', releaseBtn);
    });
  }

  initGamepad() {
    window.addEventListener('gamepadconnected', (e) => {
      console.log('Gamepad connected:', e.gamepad.id);
    });
  }

  pollGamepad() {
    const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
    const gp = gamepads[0];
    if (!gp) return;

    let gpButtons = 0;
    const map = [
      2, 1, 8, 4, 64, 128, 256, 512, 2048, 1024, 16, 32, 8192, 32768, 4096, 16384
    ];

    for (let i = 0; i < map.length && i < gp.buttons.length; i++) {
      if (gp.buttons[i].pressed) {
        gpButtons |= map[i];
      }
    }

    this.buttons |= gpButtons;

    if (Math.abs(gp.axes[0]) > 0.1 || Math.abs(gp.axes[1]) > 0.1) {
      this.lx = Math.round(gp.axes[0] * 32767);
      this.ly = Math.round(-gp.axes[1] * 32767);
    }
    if (gp.axes.length >= 4 && (Math.abs(gp.axes[2]) > 0.1 || Math.abs(gp.axes[3]) > 0.1)) {
      this.rx = Math.round(gp.axes[2] * 32767);
      this.ry = Math.round(-gp.axes[3] * 32767);
    }
  }

  initKeyboardControls() {
    const pressedKeys = new Set();
    const chips = document.querySelectorAll('.kbd-chip');

    const updateKeyboardState = () => {
      let b = 0;
      let lx = 0;
      let ly = 0;

      // Left Stick Steering & D-Pad Left/Right
      if (pressedKeys.has('KeyA') || pressedKeys.has('ArrowLeft')) {
        lx -= 32767;
        b |= 4096; // DM_BTN_LEFT
      }
      if (pressedKeys.has('KeyD') || pressedKeys.has('ArrowRight')) {
        lx += 32767;
        b |= 16384; // DM_BTN_RIGHT
      }

      // Left Stick Up/Down & D-Pad Up/Down (for navigating menus & glider pitch)
      if (pressedKeys.has('KeyW') || pressedKeys.has('ArrowUp')) {
        ly += 32767;
        b |= 8192; // DM_BTN_UP
      }
      if (pressedKeys.has('KeyS') || pressedKeys.has('ArrowDown')) {
        ly -= 32767;
        b |= 32768; // DM_BTN_DOWN
      }

      // Face Buttons
      // A (Gas): KeyJ, Space
      if (pressedKeys.has('KeyJ') || pressedKeys.has('Space')) b |= 1;
      // B (Brake): KeyK, KeyZ
      if (pressedKeys.has('KeyK') || pressedKeys.has('KeyZ')) b |= 2;
      // X (Rear Mirror): KeyU, KeyX
      if (pressedKeys.has('KeyU') || pressedKeys.has('KeyX')) b |= 4;
      // Y (Horn / Hop): KeyI, KeyY
      if (pressedKeys.has('KeyI') || pressedKeys.has('KeyY')) b |= 8;

      // Triggers & Shoulders
      // L: KeyO
      if (pressedKeys.has('KeyO')) b |= 64;
      // R: KeyP
      if (pressedKeys.has('KeyP')) b |= 128;
      // ZL (Item): KeyE, KeyQ
      if (pressedKeys.has('KeyE') || pressedKeys.has('KeyQ')) b |= 256;
      // ZR (Drift): ShiftLeft, ShiftRight, KeyC
      if (pressedKeys.has('ShiftLeft') || pressedKeys.has('ShiftRight') || pressedKeys.has('KeyC')) b |= 512;

      // Plus / Start (+): Enter, Escape
      if (pressedKeys.has('Enter') || pressedKeys.has('Escape')) b |= 1024;
      // Minus / Select (−): Tab, Backspace, KeyM
      if (pressedKeys.has('Tab') || pressedKeys.has('Backspace') || pressedKeys.has('KeyM')) b |= 2048;

      this.keyboardButtons = b;
      this.keyboardLx = lx;
      this.keyboardLy = ly;

      // Visual feedback on keyboard HUD chips
      chips.forEach(chip => {
        const keys = (chip.dataset.k || '').split(' ');
        const isActive = keys.some(k => pressedKeys.has(k));
        chip.classList.toggle('active', isActive);
      });
    };

    window.addEventListener('keydown', (e) => {
      if (['Space', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Tab'].includes(e.code)) {
        e.preventDefault();
      }
      if (!pressedKeys.has(e.code)) {
        pressedKeys.add(e.code);
        updateKeyboardState();
      }
    });

    window.addEventListener('keyup', (e) => {
      if (pressedKeys.has(e.code)) {
        pressedKeys.delete(e.code);
        updateKeyboardState();
      }
    });

    window.addEventListener('blur', () => {
      pressedKeys.clear();
      updateKeyboardState();
    });

    document.addEventListener('visibilitychange', () => {
      if (document.hidden) {
        pressedKeys.clear();
        updateKeyboardState();
      }
    });
  }

  startInputLoop() {
    const packet = new ArrayBuffer(26);
    const view = new DataView(packet);

    const loop = () => {
      this.pollGamepad();

      if (this.isConnected && this.dataChannel && this.dataChannel.readyState === 'open') {
        const sendButtons = this.buttons | (this.keyboardButtons || 0);
        const sendLx = (this.keyboardLx !== 0) ? this.keyboardLx : this.lx;
        const sendLy = (this.keyboardLy !== 0) ? this.keyboardLy : this.ly;

        view.setUint8(0, 0xAA); // magic
        view.setUint8(1, this.playerId);
        view.setUint16(2, sendButtons, true);
        view.setInt16(4, sendLx, true);
        view.setInt16(6, sendLy, true);
        view.setInt16(8, this.rx, true);
        view.setInt16(10, this.ry, true);
        view.setUint8(12, this.hasMotion ? 1 : 0);
        view.setUint8(13, 0); // reserved
        view.setInt16(14, Math.round(this.accX * 1000), true);
        view.setInt16(16, Math.round(this.accY * 1000), true);
        view.setInt16(18, Math.round(this.accZ * 1000), true);
        view.setInt16(20, Math.round(this.rotX * 1000), true);
        view.setInt16(22, Math.round(this.rotY * 1000), true);
        view.setInt16(24, Math.round(this.rotZ * 1000), true);

        try {
          this.dataChannel.send(packet);
        } catch (e) {}
      }

      // FPS Calculation
      const now = performance.now();
      if (now - this.lastFpsUpdate >= 1000) {
        if (this.operatingMode === 'full-stream') {
          this.statFps.textContent = `Stream: ${this.videoFrames} FPS`;
          this.videoFrames = 0;
        } else {
          this.statFps.textContent = `Pad: Active`;
        }
        this.lastFpsUpdate = now;
      }

      requestAnimationFrame(loop);
    };

    requestAnimationFrame(loop);
  }
}

window.addEventListener('DOMContentLoaded', () => {
  window.controller = new WebRTCController();
});