# zpdf

<p align="center">
  <img src="doc/images/zpdf-banner.svg" alt="zpdf: PDF reader for Zephyr RTOS" width="720">
</p>

A PDF reader for [Zephyr RTOS](https://zephyrproject.org),
with the PDF parser and renderer packaged as a Zephyr module.

<p align="center">
  <img src="doc/images/zpdf-ui.png" alt="The reader on native_sim: the document list, a page fitted to the view, a page at 200 % and the text view" width="840">
</p>

- Renders PDF pages to RGB bitmaps and extracts their text as UTF-8.
- LVGL touch reader for documents on an SD card: page navigation, zoom,
  panning and a text view.
- Runs on the ESP32-S3 (VIEWE UEDX32480035E-WB-A) and on `native_sim`.
- The library (`lib/zpdf`) is portable C with no dependency beyond libc.

## Quick start

```sh
west init -m https://github.com/walidbadar/zpdf --mr main zpdf-workspace
cd zpdf-workspace && west update
west build -b uedx32480035e_wb_a/esp32s3/procpu zpdf/app
west flash
```

## Documentation

Full documentation: <https://walidbadar.github.io/zpdf/>

- [Getting started](https://walidbadar.github.io/zpdf/getting_started.html): requirements, workspace setup, repository layout
- [The reader](https://walidbadar.github.io/zpdf/reader.html): controls, `native_sim`, application options
- [Hardware](https://walidbadar.github.io/zpdf/hardware.html): the supported board, SD card notes, other boards
- [The library](https://walidbadar.github.io/zpdf/library.html): API, supported features, memory, tests and host tools
- [Architecture](https://walidbadar.github.io/zpdf/architecture.html) and [Troubleshooting](https://walidbadar.github.io/zpdf/troubleshooting.html)

The sources are in [`doc/`](doc/); build them locally with
`pip install -r doc/requirements.txt && sphinx-build -b html doc doc/_build/html`.

## Licence

GPL-3.0-only; the bundled fonts and the Zephyr patch carry their own
licences, see [Licence](https://walidbadar.github.io/zpdf/licence.html).
Zephyr is a trademark of The Linux Foundation; this project is not
affiliated with or endorsed by the Zephyr Project.
