# nxnative WebRTC Remote Play

Ultra-low-latency local game streaming and virtual remote gamepad system for Nintendo Switch games running natively on Apple Silicon macOS via **nxnative**.

Allows any smartphone (iOS / Android), tablet, or secondary PC/Mac to connect over local Wi-Fi and play Switch games with full audio, video, motion gyroscope steering, and responsive touch or keyboard controls — **with zero app installation required on the client device**.

---

## Architecture & Technology Stack

```
+---------------------------------------------------------------------------------------------------+
|                                       macOS Host (nxnative)                                       |
|                                                                                                   |
|  +--------------------+   Metal Frame   +------------------------+   H.264 NALUs  +------------+  |
|  | Game (Metal / NVN) | --------------> | MetalPerformanceShader | -------------> |  libdata-  |  |
|  |   Renderer Loop    |                 |   + VideoToolbox VTB   |                |  channel   |  |
|  +--------------------+                 +------------------------+                |  (WebRTC)  |  |
|                                                                                   |            |  |
|  +--------------------+   PCM Stereo    +------------------------+   Opus Frames  |  - SRTP    |  |
|  |   Guest Audio      | --------------> |  Opus Audio Encoder    | -------------> |  - SCTP    |  |
|  |   (hle_audio.c)    |                 |   (48 kHz, 10 ms frame)|                |  - ICE     |  |
|  +--------------------+                 +------------------------+                |            |  |
|                                                                                   |            |  |
|  +--------------------+   dm_pad_set    +------------------------+   Binary 60Hz  |            |  |
|  |  Switch HID Driver | <-------------- | WebRTC DataChannel     | <------------- |            |  |
|  |    (hle_hid.c)     |                 | (Buttons, Sticks, Gyro)|                |            |  |
|  +--------------------+                 +------------------------+                +------------+  |
+----------------------------------------------------------------------------------------- | -------+
                                                                                           |
                                        Local Wi-Fi Network                                | RTP (UDP)
                                  (Ultra-Low Latency ~10-20 ms)                            | DataChannel
                                                                                           v
+---------------------------------------------------------------------------------------------------+
|                                 Client Device (Web Browser)                                       |
|                                                                                                   |
|    - HTML5 Video & Audio Elements (<video autoplay playsinline muted>)                            |
|    - Dual-Mode Gamepad (Vertical Portrait / Horizontal Landscape / Desktop Keyboard)             |
|    - DeviceOrientation / RelativeOrientationSensor (6-Axis Gyro Steering)                        |
+---------------------------------------------------------------------------------------------------+
```

### 1. Video Pipeline
- **Zero-Copy GPU Downscaler**: The game renders natively to an Apple Metal swapchain drawable. When the frame commits (`addFeedbackHandler:`), the texture is downscaled to **854x480 (480p 16:9 widescreen)** using hardware-accelerated **Metal Performance Shaders (`MPSImageBilinearScale`)** directly into shared GPU memory (`IOSurfaceRef`). There are zero CPU-to-GPU or GPU-to-CPU copies (`0 ms memory overhead`).
- **Apple VideoToolbox Hardware Encoder (`vtb_encoder.mm`)**:
  - `EnableLowLatencyRateControl = YES`: Engages Apple Silicon's hardware real-time video encoder pipeline.
  - `AllowFrameReordering = NO`: Zero B-frames, eliminating any frame reordering latency.
  - `MaxFrameDelayCount = 0`: Immediate frame output with zero frame queueing.
  - `ProfileLevel = H.264 Baseline AutoLevel`: Ultra-fast universal decoding on all mobile chips (< 2 ms decode time).
  - `DataRateLimits`: Enforces peak burst rate limits (capped at 1.5× average bitrate) to prevent UDP packet bursts that could cause router queue overflow and video stutter on Wi-Fi.

### 2. Audio Pipeline (`opus_encoder_helper.c`)
- Intercepts 16-bit stereo PCM audio directly from the Switch guest audio subsystem (`hle_audio.c`).
- Encodes audio using **Opus** at 48 kHz stereo, 128 kbps, using **10 ms frame sizes (480 samples)** to guarantee zero lip-sync drift with the video stream.

### 3. Transport & Signaling (`webrtc_streamer.cpp`)
- Built on **`libdatachannel`** (a native C++17 WebRTC implementation featuring `libjuice` for ICE, `libsrtp2` for media encryption, and `usrsctp` for DataChannels).
- **Embedded Web & Signaling Server**: Uses `cpp-httplib` to serve both HTTP (port `8888`) and HTTPS (port `8443`), handling SDP Offer/Answer exchanges and serving the static web client assets.
- **RTP Synchronization**: Strict 90 kHz RTP timestamp clock derived from monotonic system time (`mach_absolute_time`).
- **Playout Delay Extension**: Automatically negotiates `http://www.webrtc.org/experiments/rtp-hdrext/playout-delay` with `min=0, max=0` so the browser renders frames as soon as they arrive without buffering.
- **Instant Keyframe Recovery**: Implements RTCP PLI (Picture Loss Indication) and FIR (Full Intra Request) handlers. If packets are dropped over Wi-Fi, the host generates a new IDR keyframe instantly.

### 4. Universal Client Web App (`web/`)
- Pure vanilla HTML5, CSS3, and ES6 JavaScript — **no npm, Node.js, or build step required**.
- **Adaptive Micro-Jitter Buffer**: Tunes the receiver's `playoutDelayHint` and `jitterBufferTarget` to 10 ms, maintaining instant response (< 15 ms latency) while absorbing minor Wi-Fi jitter to ensure smooth 60 FPS playback.
- **Motion Steering (Gyroscope)**: Reads device roll angle via `deviceorientation` or the W3C `RelativeOrientationSensor` API, converting phone tilt directly into Left Stick X steering. Includes a 6° center deadzone to prevent accidental turning.
- **Smart Responsive Layouts**:
  - **Mobile Portrait (Vertical)**: Moves trigger buttons above ABXY so the left hand can focus entirely on steering without awkward thumb reaching.
  - **Mobile Landscape (Horizontal)**: Reverts to a standard console gamepad layout (L/ZL on top-left, R/ZR on top-right).
  - **Desktop Mode**: Automatically hides touch controls when a fine pointer / physical keyboard is detected, displaying an interactive Keyboard HUD.

---

## How to Use

### 1. Starting the Host
When running any game using `nxnative` (e.g. `Mario Kart World.app` or via terminal `make -C dm/host run`), the WebRTC server automatically starts in the background:
- **HTTP**: `http://<YOUR_MAC_IP>:8888`
- **HTTPS**: `https://<YOUR_MAC_IP>:8443`

*(You can find your Mac's local IP in macOS System Settings > Wi-Fi / Network, or by using `tools/nxremote/NXRemote.app`)*.

### 2. Connecting from a Phone / Tablet
1. Connect your phone to the **same local Wi-Fi** as your Mac.
2. Open Chrome or Safari and navigate to:
   ```
   http://<YOUR_MAC_IP>:8888
   ```
3. Tap **"Connect & Play"**.
4. *(Optional)* Tap the **Fullscreen (⛶)** button to lock the screen to landscape and hide the browser address bar.

> **Note on Gyroscope Support**: Modern mobile browsers (iOS Safari & Chrome Android) require a **secure context (HTTPS)** to access motion sensors. To use gyro steering:
> 1. Open `https://<YOUR_MAC_IP>:8443` on your phone.
> 2. Tap "Advanced" -> "Proceed" to accept the local self-signed SSL certificate.
> 3. Tap **🔄 Gyro** to enable motion steering!

### 3. Companion App (`tools/nxremote`)
A native macOS companion utility is included in `tools/nxremote/NXRemote.app`. It automatically:
- Detects your Mac's active LAN IP address.
- Generates an on-screen **QR Code** that you can scan with your phone's camera to connect instantly.
- Provides switches for HTTP/HTTPS, Player Slot selection, and game launching.

---

## Multi-Player Support (Up to 4 Players)

nxnative WebRTC supports up to **4 simultaneous connections** without port conflicts:

| Player Slot | Direct URL Parameter |
| :--- | :--- |
| **Player 1** | `http://<YOUR_MAC_IP>:8888/?p=1` |
| **Player 2** | `http://<YOUR_MAC_IP>:8888/?p=2` |
| **Player 3** | `http://<YOUR_MAC_IP>:8888/?p=3` |
| **Player 4** | `http://<YOUR_MAC_IP>:8888/?p=4` |

### Co-op / Mac Controller Integration
- **Physical Controller / Keyboard on Mac**: If you play on the Mac using a physical controller (DualSense, Xbox, Switch Pro) or keyboard, your input occupies **Player 1**.
- **Remote Player on Phone**: Simply connect the phone using **Player 2 (`?p=2`)**. In Mario Kart's 2-Player Split Screen mode, Player 1 will be controlled from the Mac while Player 2 is controlled from the phone.

---

## Control Mappings

### Desktop Keyboard Controls
When opened on a PC or Mac laptop browser, on-screen touch buttons are hidden and the following keyboard controls are active:

| Action | Primary Key | Alternate Key |
| :--- | :--- | :--- |
| **Steer (Left / Right)** | `A` / `D` | `◄` / `►` (Arrow Keys) |
| **Menu / Glider (Up / Down)** | `W` / `S` | `▲` / `▼` (Arrow Keys) |
| **Gas (A Button)** | `Space` | `J` |
| **Brake / Reverse (B Button)** | `K` | `Z` |
| **Drift / Hop (ZR Trigger)** | `Shift` (Left/Right) | `C` |
| **Use Item (ZL Trigger)** | `E` | `Q` |
| **Rear Mirror (X Button)** | `U` | `X` |
| **Horn / Jump (Y Button)** | `I` | `Y` |
| **Pause (+ Button)** | `Enter` | `Escape` |
| **Map (− Button)** | `Tab` | `M` / `Backspace` |

### Mobile Touch Controls
- **Vertical (Portrait) Mode**:
  - **Left Hand**: Analog Steer Joystick + `− SELECT`. (Triggers hidden so left thumb can navigate without obstructions).
  - **Right Hand**: 2×2 Triggers Grid (`L`, `R`, `ZL Item`, `ZR Drift`) directly above the `ABXY` diamond buttons.
- **Horizontal (Landscape) Mode**:
  - Traditional two-handed gamepad: `L` & `ZL` on top-left, `R` & `ZR` on top-right.

---

## Configuration & Environment Variables

You can customize the WebRTC server behavior via environment variables:

| Variable | Default | Description |
| :--- | :--- | :--- |
| `DM_WEBRTC_PORT` | `8888` | HTTP web server & signaling port |
| `DM_WEBRTC_SSL_PORT` | `8443` | HTTPS web server & signaling port |
| `DM_WEBRTC_WEB` | `NULL` (auto-detect) | Path to custom static web directory |

---

## Troubleshooting & Best Practices

1. **Wi-Fi Recommendations**:
   - Use a **5 GHz Wi-Fi** or Wi-Fi 6 router. 2.4 GHz networks are subject to interference from Bluetooth, microwaves, and neighboring routers, which can cause micro-packet drops.
2. **Video Stuttering / Packet Loss**:
   - Make sure your phone is not in **Low Power Mode** (which throttles hardware video decoding).
   - Ensure you are within good signal range of your Wi-Fi router.
3. **Audio is Muted Initially**:
   - Modern browsers block autoplay audio until the user interacts with the page. Tap anywhere on the screen or tap the **🔊** button in the header bar to unmute.
4. **Android Chrome Gyroscope Permission**:
   - If sensor events are blocked on Chrome Android under HTTP, either use the HTTPS URL (`https://<IP>:8443`) or enable `#enable-generic-sensor-extra-classes` in `chrome://flags`.
