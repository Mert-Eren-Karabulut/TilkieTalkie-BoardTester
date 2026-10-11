#!/usr/bin/env python3
"""Generate the voice prompt pack for the SD card (Turkish, macOS `say` voice Yelda) and the tones.

Output: sdcard/sounds/tr/<name>.mp3 (speech) and sdcard/sounds/<tone>.mp3 (tones), 44.1 kHz mono
MP3 at 64 kbps, which the firmware's MP3 decoder plays like any story track.

Requires macOS `say` and the `lameenc` package:
    /Users/mert/Herd/portal.tilkietalkie.com/scripts/bench/.venv/bin/python tools/sounds/gen_prompts.py
(or any python with `pip install lameenc`).

The prompt names must match VoicePrompts.cpp (kNames); the texts are documented in
docs/VOICE-PROMPTS.md.
"""
import array
import json
import math
import os
import subprocess
import sys
import tempfile
import wave

try:
    import lameenc
except ImportError:  # pragma: no cover
    sys.exit("lameenc is missing: run with the bench venv python or `pip install lameenc`")

RATE = 44100
VOICE = "Yelda"      # macOS Turkish voice
SPEECH_RATE = 165    # words per minute, a little slower than the default for children

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT_TR = os.path.join(ROOT, "sdcard", "sounds", "tr")
OUT_TONES = os.path.join(ROOT, "sdcard", "sounds")

# name -> spoken text. "Tilki Toki" is how the brand reads in Turkish; "kablosuz ağ" instead of
# Wi-Fi so the voice does not spell letters. Error codes are fruit names (docs/VOICE-PROMPTS.md).
PROMPTS = {
    "setup_welcome":     "Merhaba! Ben Tilki. Beni internete bağlamak için telefonundaki Tilki Toki uygulamasını aç ve yeni cihaz ekle'ye dokun.",
    "setup_connecting":  "Teşekkürler! İnternete bağlanıyorum, birazcık bekle.",
    "setup_success":     "Harika, internete bağlandım! Şimdi ilk figürünü üzerime yerleştir.",
    "setup_failed":      "Bağlanamadım. Kablosuz ağ adını ve şifresini kontrol edip uygulamadan tekrar dene. Hata kodu: Üzüm.",
    "wifi_lost":         "İnternete bağlanamıyorum. Modemin açık olduğundan emin ol. Hata kodu: Portakal.",
    "wifi_reset_hint":   "Kablosuz ağın değiştiyse iki ses tuşuna birlikte beş saniye basılı tut, sonra beni uygulamadan yeniden bağla.",
    "wifi_reset_done":   "Ağ ayarlarını sildim. Şimdi beni uygulamadan yeniden bağlayabilirsin.",
    "idle_hello":        "Merhaba! Bir hikâye dinlemek için figürünü üzerime koy.",
    "nfc_read_trouble":  "Figürü okuyamadım. Lütfen kaldırıp yeniden yerleştir.",
    "figure_no_content": "Bu figürde henüz hikâye yok. Hata kodu: Elma.",
    "figure_offline":    "Hikâyeleri şu an alamıyorum, internete bağlı değilim. Hata kodu: Armut.",
    "download_start":    "Yeni bir figür! Hikâyelerini indiriyorum, hazır olunca sana haber vereceğim.",
    "download_done":     "Hikâyelerin hazır!",
    "download_failed":   "Hikâyeleri indiremedim, birazdan yeniden deneyeceğim. Hata kodu: Kiraz.",
    "playlist_end":      "Hikâyelerin sonuna geldik. Baştan dinlemek için figürü kaldırıp yeniden koy.",
    "battery_low":       "Şarjım azalıyor, beni şarja takar mısın?",
    "battery_critical":  "Şarjım bitti, uyuyorum. Beni şarja tak.",
    "charge_full":       "Şarjım doldu!",
    "update_start":      "Kendimi güncelliyorum, birkaç dakika sürebilir. Bu sırada beni kapatma.",
    "update_done":       "Güncelleme tamamlandı!",
    "update_failed":     "Güncelleme başarısız oldu, daha sonra yeniden deneyeceğim. Hata kodu: Karpuz.",
}

# name -> list of (frequency Hz, seconds, gain); 0 Hz = silence
TONES = {
    "tone_charge_on":  [(523.25, 0.12, 0.5), (0, 0.02, 0), (783.99, 0.18, 0.5)],
    "tone_charge_off": [(783.99, 0.12, 0.5), (0, 0.02, 0), (523.25, 0.18, 0.5)],
    "tone_sleep":      [(659.25, 0.15, 0.4), (523.25, 0.15, 0.35), (392.00, 0.3, 0.3)],
    "beep":            [(880.0, 0.08, 0.45)],
}


def say_pcm(text):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "s.wav")
        subprocess.run(["say", "-v", VOICE, "-r", str(SPEECH_RATE), "-o", path,
                        "--data-format=LEI16@44100", text], check=True)
        with wave.open(path) as w:
            if w.getnchannels() != 1 or w.getframerate() != RATE or w.getsampwidth() != 2:
                raise SystemExit("unexpected say output format")
            pcm = array.array("h")
            pcm.frombytes(w.readframes(w.getnframes()))
    return pcm


def normalize(pcm, peak=0.89):
    m = max(1, max(abs(v) for v in pcm))
    g = peak * 32767 / m
    return array.array("h", (int(max(-32768, min(32767, v * g))) for v in pcm))


def pad(pcm, before=0.2, after=0.25):
    a = array.array("h", bytes(2 * int(RATE * before)))
    b = array.array("h", bytes(2 * int(RATE * after)))
    return a + pcm + b


def tone(parts):
    out = array.array("h")
    for f, sec, gain in parts:
        n = int(RATE * sec)
        for k in range(n):
            if f == 0:
                out.append(0)
                continue
            t = k / RATE
            env = min(1.0, k / (RATE * 0.01)) * min(1.0, (n - k) / (RATE * 0.03))  # 10 ms in, 30 ms out
            v = gain * env * (math.sin(2 * math.pi * f * t) + 0.25 * math.sin(4 * math.pi * f * t))
            out.append(int(max(-1.0, min(1.0, v)) * 32767))
    return out


def encode(pcm):
    enc = lameenc.Encoder()
    enc.set_bit_rate(64)
    enc.set_in_sample_rate(RATE)
    enc.set_channels(1)
    enc.set_quality(2)
    return bytes(enc.encode(pcm.tobytes())) + bytes(enc.flush())


def main():
    os.makedirs(OUT_TR, exist_ok=True)
    os.makedirs(OUT_TONES, exist_ok=True)
    total = 0
    for name, text in PROMPTS.items():
        pcm = pad(normalize(say_pcm(text)))
        mp3 = encode(pcm)
        with open(os.path.join(OUT_TR, name + ".mp3"), "wb") as f:
            f.write(mp3)
        total += len(mp3)
        print(f"{name:18s} {len(pcm) / RATE:5.1f} s {len(mp3) // 1024:4d} KB  {text}")
    for name, parts in TONES.items():
        pcm = pad(tone(parts), before=0.05, after=0.1)
        mp3 = encode(pcm)
        with open(os.path.join(OUT_TONES, name + ".mp3"), "wb") as f:
            f.write(mp3)
        total += len(mp3)
        print(f"{name:18s} {len(pcm) / RATE:5.1f} s {len(mp3) // 1024:4d} KB  (tone)")
    with open(os.path.join(OUT_TR, "prompts.json"), "w", encoding="utf-8") as f:
        json.dump({"language": "tr", "voice": VOICE, "version": 1, "prompts": PROMPTS}, f, ensure_ascii=False, indent=2)
    print(f"total {total // 1024} KB -> {os.path.relpath(OUT_TONES, ROOT)}/")


if __name__ == "__main__":
    main()
