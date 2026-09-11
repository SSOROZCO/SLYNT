# SLYNT

[![Platform](https://img.shields.io/badge/Platform-Astra%20Linux%20SE%20%7C%20Debian%20%7C%20Ubuntu-red)](https://astralinux.ru/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B%20%2F%20Slint-00599C?logo=c%2B%2B)](https://slint.dev/)
[![Sponsor](https://img.shields.io/badge/-Sponsor-green?logo=github)](https://github.com/sponsors/SSOROZCO)

**SLYNT** is an open-source music player focused on high-quality local audio playback and USB DAC output.

It is currently in **Beta**.

![SLYNT Screenshot](https://raw.githubusercontent.com/SSOROZCO/files/refs/heads/main/Screenshot_Beta_v0.1.0.avif)

SLYNT is developed primarily for Linux and is designed around local music libraries, with particular attention to high-resolution audio and direct USB DAC playback.

> ⚠️ **Beta software:** SLYNT is usable for everyday listening, but some features and hardware configurations are still under development.

---

## 🎵 Features

* Local music library
* Audio playback through GStreamer
* USB DAC support
* Direct ALSA output
* Bit-perfect playback path when using a compatible USB DAC
* DSD playback / DoP support
* Spectrum visualization
* Metadata handling
* SQLite-based music library
* Slint-based graphical interface
* Playback controls and seeking
* Multiple audio output modes

---

## 🔊 Audio Output

SLYNT is primarily designed to be used with a **USB DAC**.

The current audio architecture provides different output modes:

* **Exclusive** — direct output to the USB DAC through ALSA
* **Auto** — attempts to use the USB DAC and can fall back to system audio
* **Compatible** — uses the system audio output

For the intended high-quality playback path, a USB DAC is recommended.

> Hardware compatibility may vary depending on the DAC, ALSA configuration and Linux audio environment.

---

## 🎧 Supported Audio

SLYNT uses GStreamer for audio playback, so supported formats depend partly on the installed GStreamer plugins.

The player is being developed with high-resolution audio in mind, including formats such as:

* FLAC
* WAV
* MP3
* OGG
* AAC
* DSD / DSF
* DFF

Actual format support may vary depending on the installed GStreamer plugins.

---

## 🛠️ Technology

SLYNT is built using:

* **C++**
* **Slint** — graphical interface
* **GStreamer** — audio playback
* **TagLib** — metadata handling
* **SQLite** — music library database
* **ALSA** — direct audio output

---

## 🚧 Current Status

SLYNT is currently considered **Beta**.

The application is already usable for everyday music playback, and it is being actively developed.

Some areas are still being improved, including:

* DAC hot-plugging and reconnection
* Audio-device recovery
* Additional hardware compatibility
* Library improvements
* User interface refinement
* Additional audio formats and playback features

If you encounter a problem, please open an issue and include as much information as possible about your system and DAC.

---

## 🤝 Contributing

Contributions, bug reports, testing and forks are welcome.

If you have a different USB DAC or Linux audio configuration, testing SLYNT on your hardware can be especially useful.

Pull requests and improvements are welcome.

---

## 📜 License

SLYNT is open-source software distributed under the **GNU General Public License v3.0 (GPL-3.0)**.

The source code may be used, modified, forked and redistributed according to the terms of the GPL-3.0 license.

See [`LICENSE`](LICENSE) for the complete license text.

---

## ™️ License & Trademark

**SLYNT** is open-source software distributed under the **GNU General Public License v3.0 (GPL-3.0)**.

However, the name **"SLYNT"**, the **logo**, and the **visual identity** are trademarks of **Sebastián S. Orozco** and are **NOT** licensed under the GPL.

* ✅ You may **fork, modify, and distribute** the code under GPL-3.0.
* ❌ You may **NOT** use the **"SLYNT"** name or branding for your own distribution without permission.

For full details, see [`TRADEMARK.md`](TRADEMARK.md).

---

## 🙏 Credits

* **Strawberry Music Player** — inspiration and GPL-3.0 codebase
* **Slint** — graphical user interface framework
* **GStreamer** — audio playback
* **TagLib** — metadata handling
* **SQLite** — music library storage

---

## 📌 Project Status

SLYNT is an independent project under active development.

The current release should be considered an **early Beta**: functional and suitable for everyday use, but not yet feature-complete or guaranteed to work with every hardware configuration.

Feedback, bug reports, testing and contributions are greatly appreciated.
