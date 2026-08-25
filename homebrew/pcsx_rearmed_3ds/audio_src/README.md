# Audio source clips

Raw signed-16-bit little-endian PCM, generated once via `ffmpeg` and
checked in directly -- NDSP wants raw PCM regardless, and neither clip
needs re-encoding at runtime, so there's no reason to ship an MP3/AAC
decoder in the app just to produce the same bytes on every boot.

Regenerate with:

```sh
# Boot chime -- full clip, audio only, downmixed 5.1->stereo, native
# 44.1kHz kept since it's short.
ffmpeg -i "PlayStation Intro 1080p [Remastered].mp4" \
  -vn -ac 2 -ar 44100 -f s16le -acodec pcm_s16le boot_chime.pcm

# Menu music -- first 45s only (it loops; the source track is 2 minutes
# and this is background music, not something worth the extra ~17MB),
# downsampled to 32kHz since fidelity matters less for a loop.
ffmpeg -i "Offical UK PlayStation Magazine Demo Disk Music_ Menu.mp3" \
  -t 45 -ac 2 -ar 32000 -f s16le -acodec pcm_s16le menu_music.pcm
```
