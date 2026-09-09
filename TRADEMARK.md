# SLYNT

**SLYNT Music Player** is a Linux music player focused on high-quality local audio playback and USB DACs.

> **Status: Beta**
>
> SLYNT is currently usable for everyday music listening, but the project is still under active development. Some areas, particularly hardware detection, DAC reconnection and audio-device handling, may be improved in future releases.

## Features

* Local music library
* USB DAC playback
* Bit-perfect playback with supported DACs
* DSD / DoP playback
* Multiple output modes
* Spectrum visualization
* Metadata and library management
* Audio playback powered by GStreamer

## Requirements

SLYNT currently requires a **USB DAC** for its intended high-quality playback mode.

System audio fallback may be available depending on the selected output mode and system configuration.

## Project Status

This is an early public **Beta release**.

The main goal of this release is to make SLYNT available to other users so the project can receive testing, feedback and contributions.

Hardware configurations can behave differently, especially with ALSA, PipeWire and USB DACs. If you encounter a problem, please open an issue with the relevant terminal output and system information.

---

## 📄 License & Trademark

**SLYNT** is open-source software distributed under the **GNU General Public License v3.0 (GPL-3.0)**.

However, the **name "SLYNT"**, the **logo**, and the **visual identity** are trademarks of Sebastián S. Orozco and are **NOT** licensed under GPL.

* ✅ You may fork, modify and distribute the code under the applicable GPL-3.0 terms.
* ❌ You may not use the **"SLYNT"** name or SLYNT branding for your own distribution without permission.

For full details, see [`TRADEMARK.md`](TRADEMARK.md).

---

## 🙏 Credits

* **Strawberry Music Player** – inspiration and GPL-3.0 codebase
* **Slint** – GUI framework
* **GStreamer** – audio playback
* **TagLib** – metadata handling
* **SQLite** – library storage

## Contributing

Bug reports, testing, suggestions and improvements are welcome.

If you create a fork, please read [`TRADEMARK.md`](TRADEMARK.md) regarding the SLYNT name and branding.
