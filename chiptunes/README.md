# Optional TF-card chiptunes

The clock firmware already contains four original melodies (CLASSIC CHIME, PIXEL SUNRISE, 8-BIT RUSH, NIGHT CIRCUIT). They play from flash with **no TF card**.

For additional songs, copy any `.rtttl` files from this directory to the **root** of a FAT32-formatted TF card, insert it, open **FILES**, and play or assign a file as an alarm/timer sound. This is normal RTTTL text syntax: `name:d=8,o=5,b=150:c,e,g,4p`.

The three optional examples here are also original, independently composed for this firmware. They do not copy copyrighted melodies.

The same file browser accepts `.wav` (RIFF PCM, 8/16-bit mono/stereo, 8–48kHz) and `.mp3` (ordinary MPEG Layer III). Playback is deliberately low-fi: 16kHz mono through the ESP32's 8-bit DAC and small onboard amplifier.
