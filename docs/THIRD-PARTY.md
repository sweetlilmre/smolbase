# Third-party notices

Smolbase contains no copied third-party source code, but it ships one bundled
data asset and links against pinned libraries. Verify current terms against
each project's own LICENSE file (the pinned copies land in
`.pio/libdeps/smolbase/` after a build).

## Bundled asset

### `html/zones.json` — IANA-to-POSIX timezone table

Derived from [nayarsystems/posix_tz_db](https://github.com/nayarsystems/posix_tz_db),
MIT License. The upstream LICENSE file retains the MIT template's placeholder
copyright line, reproduced as published:

> MIT License
>
> Copyright (c) [year] [fullname]
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

## Linked libraries

Pinned in `platformio.ini`; licenses verified from the installed packages'
manifests and LICENSE files (versions as of this writing).

| Library | Version | License | Copyright |
| --- | --- | --- | --- |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | 1.2.26 | FreeBSD (BSD-2-Clause); bundles BSD-licensed Adafruit-derived code (manifest: `MIT AND BSD-2-Clause`) | Copyright (c) 2020 lovyan03 |
| [PsychicHttp](https://github.com/hoeken/PsychicHttp) | 3.1.2 | MIT | Copyright (c) 2024 Jeremy Poulter, Zachary Smith, and Mathieu Carbou |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.x | MIT | Copyright © 2014-2026, Benoit BLANCHON |

## Platform and framework

The build targets the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform: [arduino-esp32](https://github.com/espressif/arduino-esp32)
(LGPL-2.1) over [ESP-IDF](https://github.com/espressif/esp-idf) (Apache-2.0).
These are toolchain/framework dependencies fetched at build time, not
distributed in this repository.
