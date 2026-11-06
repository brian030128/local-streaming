# Low-latency screen streaming over a direct Ethernet cable

Streams one Windows PC's screen to another with hardware H.264 over UDP. It has video only: no audio and no input forwarding.

```
Sender:   Desktop Duplication -> GPU NV12 conversion -> hardware H.264 -> UDP
Receiver: UDP -> H.264 decode -> immediate present (no frame queue)
```

## Requirements

- Windows 10 or 11 on both PCs.
- Sender: a GPU with a hardware H.264 encoder (Intel Quick Sync, NVIDIA NVENC or AMD VCN).
- Receiver: any GPU. Hardware decoding is used when available, with a software fallback.

## Build

Run `build.bat` from a Visual Studio Developer Command Prompt, or with llvm-mingw on `PATH`. Copy the resulting `.exe` files to the other PC.

## Network setup (once, as Administrator)

A direct cable has no router, so give each PC a fixed address. Find the adapter name with `Get-NetAdapter`, then replace `Ethernet` below.

Sender:

```powershell
New-NetIPAddress -InterfaceAlias "Ethernet" -IPAddress 192.168.50.1 -PrefixLength 24
```

Receiver:

```powershell
New-NetIPAddress -InterfaceAlias "Ethernet" -IPAddress 192.168.50.2 -PrefixLength 24
New-NetFirewallRule -DisplayName "Local stream" -Direction Inbound -Protocol UDP -LocalPort 5000 -Action Allow
```

Check that the link is 1 Gbps with `Get-NetAdapter`, then test it with `ping 192.168.50.2` from the sender. The firewall on the receiver may block ping by default even when the link is fine.

## Run

```
receiver.exe                   (on the PC showing the stream; Esc quits)
sender.exe 192.168.50.2        (on the PC being captured; Ctrl+C quits)
```

Start order doesn't matter. Each program prints its measurements once per second.

| Sender option | Purpose |
| --- | --- |
| `--bitrate 60` | H.264 bitrate in Mbps |
| `--fps 60` | Cap the stream's frame rate |
| `--crop 400x400` | Stream only this region at the screen's center |
| `--size 1280x720` | Scale the stream down |
| `--output 1` | Capture another display |
| `--encoder mf` | Use Media Foundation instead of oneVPL, the default on Intel GPUs |
| `--intra-refresh` | oneVPL only: heal the picture gradually instead of with keyframes (no latency benefit in testing) |

| Receiver option | Purpose |
| --- | --- |
| `--window 1280x720` | Use a window instead of fullscreen |
| `--vsync` | Remove tearing, adding up to one refresh of latency |
| `--actual-size` | Show the video pixel for pixel instead of stretching it |

## Playing games while streaming

1. **Leave the GPU some headroom. This matters most.** Cap the game's frame rate so the GPU isn't at 100%. A saturated GPU delays capture and hurts the game too.
2. **Use borderless or windowed mode.** Exclusive fullscreen can interrupt capture, which makes the sender restart.
3. **If the game still suffers,** try `--fps 30` or `--size 1280x720`. Neither has been measured with a game running. Without a game, 720p showed no consistent latency difference from native 1080p.

## Reducing latency further

- Turn off **Interrupt Moderation** on both Ethernet adapters (Device Manager > adapter > Advanced). This trades some CPU use for faster packet delivery.
- Use a high-refresh display on both PCs. The capture and display refresh rates set a floor that no code can remove.
- Disable Wi-Fi power saving and USB selective suspend if you use a USB Ethernet adapter.

## Measuring real latency

The built-in numbers can't see the monitors, so use a camera for the full picture:

1. Run `latency_clock.exe` on the sender, with the stream running.
2. Take a phone photo showing both monitors.
3. Subtract the clock on the receiver's screen from the clock on the sender's screen.

Take at least 10 photos and average them, because each reading varies by up to a frame or more.

## Reading the numbers

The receiver's `capture-to-present` is the time from a frame being captured to its `Present` call returning, using a clock estimated over the cable. It doesn't include the game's rendering or waiting for capture before that, or the monitor's scan-out and pixel response after it.

`lost` should stay at 0. If it doesn't, lower `--bitrate` and check the cable and adapter. After a loss the picture freezes briefly until a new keyframe arrives.

## Encoders

On Intel GPUs the sender loads oneVPL's runtime, `libmfx64-gen.dll`, directly from the graphics driver. It doesn't use the `libvpl.dll` dispatcher because that needs the Visual C++ runtime. If the runtime is missing or fails, the sender falls back to Media Foundation, which also covers NVIDIA and AMD GPUs.

`third_party/vpl` holds Intel's oneVPL API headers, under the license in that folder.

## Known limitations

- 4:2:0 colour blurs fine coloured text slightly. H.264 through Media Foundation doesn't offer 4:4:4.
- Rotated displays aren't handled.
- Cursors that invert the screen colours are drawn in black.
- Captures can't include the UAC or lock screens; the stream resumes when they close.
