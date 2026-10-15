#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Diagnosa USB Android / iPhone - untuk Windows.

Fungsi utama:
  - Deteksi perangkat yang dicolok lewat USB (Android / iPhone).
  - Untuk Android: cek ADB, status USB debugging, info baterai, mode EDL/Preloader.
  - Untuk kasus LCD gelap: bisa langsung buka scrcpy supaya layar HP tampil di PC.
  - Untuk iPhone: cek pairing & info dasar (butuh libimobiledevice / iTunes driver).

Alat ini untuk DIAGNOSA & SERVIS yang sah. Tidak melakukan bypass kunci apa pun.

Cara pakai (di Command Prompt / PowerShell):
    python diagnosa.py            -> deteksi + info
    python diagnosa.py --mirror   -> deteksi, lalu buka scrcpy jika Android siap
    python diagnosa.py --watch     -> pantau terus (colok/cabut) tiap 2 detik
"""

import argparse
import shutil
import subprocess
import sys
import time

# ---------- warna di terminal (biar gampang dibaca) ----------
try:
    import colorama
    colorama.init()
    G, R, Y, C, B, X = (
        colorama.Fore.GREEN, colorama.Fore.RED, colorama.Fore.YELLOW,
        colorama.Fore.CYAN, colorama.Style.BRIGHT, colorama.Style.RESET_ALL,
    )
except Exception:  # colorama opsional
    G = R = Y = C = B = X = ""


def _run(cmd, timeout=15):
    """Jalankan perintah, kembalikan (returncode, stdout, stderr) sebagai teks."""
    try:
        p = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
        )
        return p.returncode, (p.stdout or "").strip(), (p.stderr or "").strip()
    except FileNotFoundError:
        return 127, "", "tool tidak ditemukan"
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"


def _has(tool):
    return shutil.which(tool) is not None


def header(text):
    print(f"\n{B}{C}== {text} =={X}")


# ----------------------------------------------------------------------
# ANDROID
# ----------------------------------------------------------------------
def android_adb():
    """Cek perangkat Android via ADB. Kembalikan list dict per perangkat."""
    if not _has("adb"):
        print(f"{Y}adb belum terpasang.{X} Install 'Android Platform Tools', "
              f"lalu taruh folder-nya di PATH.")
        return []

    rc, out, _ = _run(["adb", "devices", "-l"])
    if rc != 0:
        print(f"{R}Gagal menjalankan adb.{X}")
        return []

    devices = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        serial, state = parts[0], parts[1]
        devices.append({"serial": serial, "state": state})
    return devices


def android_battery(serial):
    rc, out, _ = _run(["adb", "-s", serial, "shell", "dumpsys", "battery"])
    if rc != 0:
        return {}
    info = {}
    for line in out.splitlines():
        if ":" in line:
            k, _, v = line.strip().partition(":")
            info[k.strip()] = v.strip()
    result = {}
    if "level" in info:
        result["baterai"] = f"{info['level']}%"
    # 2 = AC, 4 = USB charging di sebagian ROM; tampilkan apa adanya
    if "status" in info:
        result["status_charging"] = info["status"]
    if "temperature" in info:
        try:
            result["suhu"] = f"{int(info['temperature']) / 10:.1f} C"
        except ValueError:
            pass
    return result


def android_model(serial):
    props = {
        "model": "ro.product.model",
        "merek": "ro.product.brand",
        "android": "ro.build.version.release",
    }
    out = {}
    for name, prop in props.items():
        rc, val, _ = _run(["adb", "-s", serial, "shell", "getprop", prop])
        if rc == 0 and val:
            out[name] = val
    return out


def report_android():
    header("ANDROID (ADB)")
    devices = android_adb()
    if not devices:
        print(f"{Y}Tidak ada Android terdeteksi lewat ADB.{X}")
        print("  Kemungkinan: USB Debugging belum aktif, kabel data rusak,")
        print("  atau HP ada di mode EDL/Preloader (lihat bagian bawah).")
        return devices

    for d in devices:
        serial, state = d["serial"], d["state"]
        if state == "device":
            print(f"{G}[OK]{X} {serial} - siap diakses (USB Debugging ON)")
            info = {}
            info.update(android_model(serial))
            info.update(android_battery(serial))
            for k, v in info.items():
                print(f"       {k:<16}: {v}")
            print(f"       {G}LCD gelap? Jalankan dengan --mirror untuk "
                  f"tampilkan layar di PC.{X}")
        elif state == "unauthorized":
            print(f"{Y}[!] {serial} - terdeteksi tapi BELUM diizinkan.{X}")
            print("       Tekan 'Allow / Izinkan' di layar HP (biasanya tombol")
            print("       kanan-bawah). Kalau LCD gelap: colok mouse OTG untuk klik.")
        else:
            print(f"{Y}[?] {serial} - status: {state}{X}")
    return devices


def mirror_screen(serial):
    """Buka scrcpy supaya layar HP LCD-gelap tampil di PC."""
    if not _has("scrcpy"):
        print(f"{R}scrcpy belum terpasang.{X} Unduh dari rilis resmi scrcpy, "
              f"ekstrak, taruh di PATH.")
        return
    print(f"{C}Membuka scrcpy untuk {serial} ... (tutup jendela untuk selesai){X}")
    # --stay-awake biar layar tidak mati saat dicolok
    subprocess.run(["scrcpy", "-s", serial, "--stay-awake"])


# ----------------------------------------------------------------------
# iPHONE
# ----------------------------------------------------------------------
def report_iphone():
    header("iPHONE (libimobiledevice)")
    if not _has("idevice_id"):
        print(f"{Y}libimobiledevice belum terpasang.{X}")
        print("  Install iTunes/Apple driver + tool libimobiledevice untuk Windows.")
        return []

    rc, out, _ = _run(["idevice_id", "-l"])
    udids = [u for u in out.splitlines() if u.strip()] if rc == 0 else []
    if not udids:
        print(f"{Y}Tidak ada iPhone terdeteksi.{X} "
              f"Pastikan sudah 'Trust' di layar iPhone.")
        return []

    for udid in udids:
        print(f"{G}[OK]{X} iPhone {udid}")
        if _has("ideviceinfo"):
            for key in ("DeviceName", "ProductType", "ProductVersion",
                        "BatteryCurrentCapacity"):
                rc, val, _ = _run(["ideviceinfo", "-u", udid, "-k", key])
                if rc == 0 and val:
                    print(f"       {key:<22}: {val}")
    return udids


# ----------------------------------------------------------------------
# MODE KHUSUS (EDL / DFU) - hanya deteksi, info untuk teknisi
# ----------------------------------------------------------------------
def report_special_modes():
    header("MODE KHUSUS (deteksi saja)")
    print("Jika HP tidak muncul sebagai Android/iPhone biasa, cek Device Manager")
    print("Windows (devmgmt.msc). Tanda yang berguna untuk diagnosa:")
    print(f"  {C}Qualcomm HS-USB QDLoader 9008{X} -> Android mode EDL (CPU hidup)")
    print(f"  {C}MediaTek Preloader / DA USB VCOM{X} -> Android MTK (CPU hidup)")
    print(f"  {C}Apple Mobile Device (DFU/Recovery){X} -> iPhone di DFU/Recovery")
    print("  Tidak muncul apa pun -> cek kabel data, port charging, atau tegangan.")


# ----------------------------------------------------------------------
def scan_once(do_mirror=False):
    print(f"{B}Diagnosa USB - Android / iPhone (Windows){X}")
    print("Alat servis & diagnosa. Tidak melakukan bypass kunci.\n")

    tools = {"adb": _has("adb"), "scrcpy": _has("scrcpy"),
             "idevice_id": _has("idevice_id")}
    print("Status tool: " + ", ".join(
        f"{G if ok else R}{name}={'ada' if ok else 'belum'}{X}"
        for name, ok in tools.items()))

    android = report_android()
    report_iphone()
    report_special_modes()

    if do_mirror:
        ready = [d["serial"] for d in android if d["state"] == "device"]
        if ready:
            mirror_screen(ready[0])
        else:
            print(f"\n{Y}--mirror diminta, tapi tidak ada Android siap "
                  f"(status 'device').{X}")


def main():
    ap = argparse.ArgumentParser(description="Diagnosa USB Android/iPhone (Windows)")
    ap.add_argument("--mirror", action="store_true",
                    help="buka scrcpy untuk Android siap (berguna saat LCD gelap)")
    ap.add_argument("--watch", action="store_true",
                    help="pantau terus tiap 2 detik (Ctrl+C untuk berhenti)")
    args = ap.parse_args()

    if args.watch:
        try:
            while True:
                print("\033c", end="")  # bersihkan layar
                scan_once(do_mirror=False)
                print(f"\n{C}(pantau... Ctrl+C untuk berhenti){X}")
                time.sleep(2)
        except KeyboardInterrupt:
            print("\nSelesai.")
            return 0
    else:
        scan_once(do_mirror=args.mirror)
    return 0


if __name__ == "__main__":
    sys.exit(main())
