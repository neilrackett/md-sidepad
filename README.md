# Sidepad

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

![Sidepad](desc/sidepad.png)

Sidepad enables your SidecarTridge Multi-device to connect almost any Bluetooth gamepad (Xbox One/Series, DualShock, DualSense, Switch Pro, 8BitDo, and more) to your Atari ST and use it as a joystick.

Once installed, simply pair your controller, then press ESC and you're ready to play - you don't even need to disconnect your existing joystick, Sidepad works alongside it.

To see Sidepad working, you can use [PP's mouse and joystick tester](https://atari.8bitchip.info/astopensw.php).

## Known limitations

Sidepad currently talks to your Atari ST through the system `joyvec`, so games and demos that read the IKBD ACIA interrupt directly, rather than going through `joyvec`, probably won't see your controller yet.

## Installation

1. Download the latest files from the [releases page](https://github.com/neilrackett/md-sidepad/releases).
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. On the Booster screen, press ESC for the app list and select the Sidepad app.
4. To return to Booster, power on and press X when the menu appears.

## What's next?

The main focus is seeing if we can work out how to get games that read the IKBD ACIA interrupt directly to work, but we've had a lots of other ideas too, like:

- Map buttons to keys?
- Use an analogue stick as a mouse?
- Multiple gamepads?
- Support Bluetooth mice?
- Support Bluetooth keyboard?

Think you can help? Got an idea of your own? We'd love to hear from you, so why not let me know on [X](https://x.com/neilrackett) or submit a PR.

## License

The source code of the project is licensed under the GNU General Public License v3.0. The full license is accessible in the [LICENSE](LICENSE) file.
