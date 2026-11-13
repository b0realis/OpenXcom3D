> **VR game design experimentation — not for gaming at the moment.**

# OpenXcom3D on the Meta Quest 3: build and install with adb

An experiment: OpenXcom3D (the `voxel_units` branch) as a standalone Meta Quest 3 app. The whole game runs on a
floating pane like any Android app on the headset; with the setting **VR mode** on, the 3D view becomes an immersive
stereo view with head tracking and the controllers. It works, but it isn't proven playable yet. Drawn by the CPU, the
Quest manages only a coarse picture (460x500 an eye at about 20 pictures a second, 710x770 at about 8). This build can
draw the eyes on the headset's graphics chip instead (setting **Renderer: GPU**, with **VR eyes kept on the card**), at
eye pictures up to 1620x1760: that path has run on an Android emulator only, not yet on a headset, so its speed there
is unknown. Until it is tried, this is for trying out VR game design, not for playing.

There is no download: you build the app yourself and install it over USB with `adb`. You also need the original game's
data (UFO: Enemy Unknown and/or Terror from the Deep), as for the desktop version.

## What's here

| File | What |
|---|---|
| `patches/engine_on_android_1ab7e7c.diff` | OpenXcom3D on OXCE's Android port: apply it to MeridianOXC/OpenXcom at `1ab7e7cc830e91a2e6e79fda64f26639f5a981d7` (the `android` branch, OXCE 8.7.1, SDL2). It holds the whole add-on, the VR mode's OpenXR code (`src/FirstPerson/FirstPersonXr*`) and the Android changes. |
| `patches/wrapper_quest3.diff` | the Android app around it: apply it to MeridianOXC/openxcom-android at `71fb95a624f1bc3f6bfbabb4b5fe9b8385ca18e6` (branch `oxce-plus-proto`). The app id is `org.openxcom3d.quest`; it adds the immersive VR activity and uses Khronos's OpenXR loader (no Meta SDK). |
| `patches/SHA256SUMS` | the two patches' checksums |

## Build (on Linux)

**You need:** git, JDK 17, the Android SDK with `platform-tools`, `build-tools;35.0.0`, `platforms;android-35`,
`ndk;30.0.14904198` and `cmake;3.22.1`, and about 5 GB of disk. Gradle fetches itself.

```sh
# the engine: OXCE's Android port with OpenXcom3D on it
git clone https://github.com/MeridianOXC/OpenXcom.git oxce-android
git -C oxce-android checkout -b quest3 1ab7e7cc830e91a2e6e79fda64f26639f5a981d7
git -C oxce-android apply --binary /path/to/OpenXcom3D/quest3/patches/engine_on_android_1ab7e7c.diff   # this repository's

# the Android app around it
git clone --branch oxce-plus-proto https://github.com/MeridianOXC/openxcom-android.git
cd openxcom-android
git checkout -b quest3 71fb95a624f1bc3f6bfbabb4b5fe9b8385ca18e6
git submodule update --init app/src/main/jni/SDL app/src/main/jni/SDL2_image app/src/main/jni/SDL2_mixer app/src/main/jni/SDL_gfx
git apply --binary /path/to/OpenXcom3D/quest3/patches/wrapper_quest3.diff
```

(The wrapper's `OpenXcom` submodule isn't needed: `oxce.dir` below points at the engine.)

**A signing key of your own.** Android installs only signed apps, and later builds install over earlier ones only when
they carry the same key, so make one and keep it:

```sh
keytool -genkeypair -v -storetype PKCS12 -keystore quest.p12 -alias quest -keyalg RSA -keysize 4096 -validity 10000
```

and a properties file for it, e.g. `quest-signing.properties`:

```
storeFile=/path/to/quest.p12
storeType=PKCS12
storePassword=...
keyAlias=quest
keyPassword=...
```

**Local settings**, in `openxcom-android/local.properties` (never committed):

```
sdk.dir=/path/to/android-sdk
oxce.dir=/path/to/oxce-android
signing.properties=/path/to/quest-signing.properties
native.jobs=8
```

`native.jobs` caps the compilers running at once. Then:

```sh
export JAVA_HOME=/path/to/jdk17
./gradlew --no-daemon assembleRelease
```

The app is `app/build/outputs/apk/release/OpenXcom3D-quest-<version>-<code>-release.apk` (arm64 only), about 11 MB.
A clean build takes a few minutes. Don't edit the engine's sources while a build runs.

## Install on the headset

1. **Developer mode (once).** At https://developers.meta.com/horizon/ create an organization (free; Meta wants the
   account verified). In the Meta Horizon phone app: Menu → Devices → your headset → Headset settings → Developer mode
   → on, then restart the headset.
2. **Connect.** Plug the headset into the computer with a USB-C cable. Put the headset on: it asks **Allow USB
   debugging?**: tick *Always allow from this computer* and allow. `adb devices` must list it as `device`.
   If it says `unauthorized`, the headset hasn't answered yet: wear it (a sleeping headset shows no prompt), unplug and
   plug in again (or `adb kill-server`, then `adb devices`), look in the headset's notifications; make sure only one
   `adb` runs (Meta Quest Developer Hub and Android Studio bring their own) and never run adb with sudo.
3. **Install the app:**

   ```sh
   adb install -r OpenXcom3D-quest-<version>-<code>-release.apk
   ```

   `-r` replaces an earlier build and keeps its settings and saves.
4. **Copy your game data** (the folders themselves, named exactly `UFO` and/or `TFTD`):

   ```sh
   adb shell mkdir -p /sdcard/openxcom
   adb push /path/to/UFO  /sdcard/openxcom/
   adb push /path/to/TFTD /sdcard/openxcom/
   ```

5. **Let it read that folder (once):**

   ```sh
   adb shell appops set --uid org.openxcom3d.quest MANAGE_EXTERNAL_STORAGE allow
   ```

6. **Start it:** in the headset, Library → Unknown Sources → OpenXcom3D, or

   ```sh
   adb shell am start -n org.openxcom3d.quest/org.openxcom.extended.PreloaderActivity
   ```

   The first start copies the game's common data and the OpenXcom3D mod. The profile (saves, `options.cfg`,
   `openxcom.log`) lives in `/sdcard/openxcom3d/`.

To remove it: `adb uninstall org.openxcom3d.quest` (the data folders stay; delete `/sdcard/openxcom3d` if you like).

## Playing it

**On the pane** the game plays like the desktop game with a mouse: the controller's ray is the pointer, the trigger
clicks, the stick scrolls, B or Y is Back (on the battlescape it cancels the action in hand; in the 3D view it frees the
cursor). That is all Horizon OS passes to an app on a pane. A Bluetooth keyboard and mouse work as on the desktop.

**VR mode:** Options → 3D VIEW → the VR section at the end → **VR mode: On**. Then going into the 3D view (the 3D button
or F8) leaves the pane for an immersive stereo view; B brings the pane back. The aliens' turn always plays on the pane;
your next turn returns to VR by itself. The VR section's other settings: **VR eye picture** (360x390 / 460x500 /
710x770: sharper is slower), **VR weapon** (the weapon in your hand), **VR turning** (Smooth / Snap), **VR laser** (the
aiming laser), **VR smoothing** (the headset's GPU smooths each eye's pixels).

The head only looks around; the right controller is the weapon; only the right stick turns the soldier (paying his time
units, as the game's turning does), and a shot needs the target in his front arc.

| Control | In the 3D view | When another screen is up (inventory, messages, menus) |
|---|---|---|
| Head | looks round (it never turns the soldier) | — |
| Right controller | the weapon: it aims where you point it (the red laser, setting "VR laser", to the crosshair) | — |
| Left grip, held | both hands on the weapon: the barrel from the right hand through the left | — |
| Left stick | walk and strafe (tile steps or free walking, as the setting Movement says) | — |
| Left stick click | sprint | — |
| Right stick left / right | turn the soldier, as far as you push (setting "VR turning": Smooth; Snap turns 45 degrees a push), paying his time units | — |
| Right stick up / down | up and down (flying, lifts) | the mouse wheel |
| Right stick click | recentre | — |
| Right trigger | fire at the crosshair; with the cursor free, click | click |
| Left trigger | mouse look / free cursor (the C key on the PC), once a pull | — |
| Right grip | the right mouse button | right click |
| The right controller's ray | with the cursor free: the cursor, over the panel ahead (its top bar's buttons) | the mouse |
| A | the inventory | the inventory key (it closes the inventory) |
| B | back to the pane (the 3D view's key, F8) | Esc (closes the screen) |
| X | kneel | — |
| Y, held a second | end the turn | — |
| Left menu button (≡) | Esc: the game's menu | Esc |
| Meta button, held | the headset's own recentring (the game follows) | the same |

## Logs

The game's own log is the useful one: it records the VR steps, each new kind of controller input and, after a crash,
where it happened.

```sh
adb pull /sdcard/openxcom3d/openxcom.log
```

The headset's logcat keeps only about the last minute; to keep everything, stream it to a file while you play:

```sh
adb logcat -c
adb logcat -v time -s OpenXcom3D OpenXcom3DInput SDL AndroidRuntime DEBUG libc > quest_logcat.txt
```
