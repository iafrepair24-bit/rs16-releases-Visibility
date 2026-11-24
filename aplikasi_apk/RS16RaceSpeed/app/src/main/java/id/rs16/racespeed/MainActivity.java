package id.rs16.racespeed;

import android.app.Activity;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.provider.MediaStore;
import android.util.Base64;
import android.util.Log;
import android.view.WindowManager;
import android.webkit.ConsoleMessage;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * RS16 Race Speed - pembungkus aplikasi web RS16 (index.html) jadi aplikasi Android.
 *
 * - Halaman dimuat dari alamat lokal http://app.rs16/ (tidak ke internet): isinya index.html bawaan APK
 *   atau versi lebih baru yang sudah diunduh dari server update.
 * - Saat HP tersambung ke WiFi alat (tanpa internet), semua koneksi aplikasi diarahkan ke WiFi itu
 *   supaya ws://192.168.4.1:81 bisa dicapai walaupun data seluler menyala.
 * - Jembatan "RS16Native" untuk JavaScript: simpan file, layar tetap nyala, getar,
 *   update tampilan, dan update firmware alat lewat OTA (POST /update).
 */
public class MainActivity extends Activity {
    private static final String TAG = "RS16";
    private static final String APP_HOST = "app.rs16";
    private static final String START_URL = "http://" + APP_HOST + "/index.html";
    private static final int REQ_FILE = 11, REQ_FW = 12;

    private WebView web;
    private SharedPreferences prefs;
    private ValueCallback<Uri[]> fileCb;
    private ConnectivityManager cm;
    private ConnectivityManager.NetworkCallback wifiCb;
    private volatile Network bound;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("rs16", MODE_PRIVATE);
        getWindow().setStatusBarColor(Color.parseColor("#06102A"));
        getWindow().setNavigationBarColor(Color.parseColor("#06102A"));

        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#06102A"));
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setDatabaseEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        s.setCacheMode(WebSettings.LOAD_DEFAULT);
        s.setTextZoom(100);

        web.addJavascriptInterface(new Bridge(), "RS16Native");
        web.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView v, WebResourceRequest r) {
                Uri u = r.getUrl();
                if (!APP_HOST.equals(u.getHost())) return null;   // ubin satelit, dll -> jaringan biasa
                String p = u.getPath() == null ? "/" : u.getPath();
                if (p.equals("/") || p.equals("/index.html")) {
                    try {
                        WebResourceResponse res = new WebResourceResponse("text/html", "utf-8", openIndex());
                        java.util.Map<String, String> h = new java.util.HashMap<>();
                        h.put("Cache-Control", "no-store");
                        res.setResponseHeaders(h);
                        return res;
                    } catch (IOException e) { Log.e(TAG, "index", e); }
                }
                return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found", null, new ByteArrayInputStream(new byte[0]));
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                Uri u = r.getUrl();
                if (APP_HOST.equals(u.getHost())) return false;
                try { startActivity(new Intent(Intent.ACTION_VIEW, u)); } catch (Exception ignored) { }
                return true;   // link luar dibuka di browser
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView v, ValueCallback<Uri[]> cb, FileChooserParams p) {
                if (fileCb != null) fileCb.onReceiveValue(null);
                fileCb = cb;
                Intent i = new Intent(Intent.ACTION_GET_CONTENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                try { startActivityForResult(Intent.createChooser(i, "Pilih file"), REQ_FILE); }
                catch (Exception e) { fileCb = null; return false; }
                return true;
            }

            @Override
            public boolean onConsoleMessage(ConsoleMessage m) {
                Log.d(TAG, "JS: " + m.message() + " @" + m.lineNumber());
                return true;
            }
        });

        setupWifiBinding();
        web.loadUrl(START_URL);

        // cek update tampilan & firmware di latar belakang (kalau ada internet)
        new Thread(() -> { checkWebUpdate(false); checkFirmware(false); }).start();
    }

    // ---------------------------------------------------------------- isi halaman
    private File webFile() { return new File(getFilesDir(), "web/index.html"); }

    private String bundledWebVersion() {
        try (InputStream in = getAssets().open("web_version.txt")) { return readAll(in).trim(); }
        catch (IOException e) { return "0"; }
    }

    /** Versi tampilan yang sedang dipakai: unduhan (kalau lebih baru dari bawaan APK) atau bawaan. */
    private String currentWebVersion() {
        String dl = prefs.getString("webVer", "0"), bundled = bundledWebVersion();
        return (webFile().exists() && verCmp(dl, bundled) > 0) ? dl : bundled;
    }

    private InputStream openIndex() throws IOException {
        String dl = prefs.getString("webVer", "0");
        if (webFile().exists() && verCmp(dl, bundledWebVersion()) > 0) return new FileInputStream(webFile());
        return getAssets().open("index.html");
    }

    // ---------------------------------------------------------------- WiFi alat (tanpa internet)
    private void setupWifiBinding() {
        cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        wifiCb = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onCapabilitiesChanged(Network n, NetworkCapabilities nc) {
                boolean noInternet = !nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
                if (noInternet && !n.equals(bound)) {
                    // WiFi tanpa internet = WiFi alat RS16 -> arahkan koneksi aplikasi ke sana
                    if (cm.bindProcessToNetwork(n)) { bound = n; Log.i(TAG, "bind ke WiFi alat"); }
                } else if (!noInternet && n.equals(bound)) {
                    cm.bindProcessToNetwork(null); bound = null;
                }
            }

            @Override
            public void onLost(Network n) {
                if (n.equals(bound)) { cm.bindProcessToNetwork(null); bound = null; Log.i(TAG, "lepas WiFi alat"); }
            }
        };
        try { cm.registerNetworkCallback(req, wifiCb); } catch (Exception e) { Log.e(TAG, "wifi cb", e); }
    }

    /** Koneksi ke internet (server update) memakai jaringan yang punya internet, bukan WiFi alat. */
    private HttpURLConnection openInternet(String url) throws IOException {
        Network net = null;
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities c = cm.getNetworkCapabilities(n);
            if (c != null && c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) { net = n; break; }
        }
        URL u = new URL(url);
        HttpURLConnection h = (HttpURLConnection) (net != null ? net.openConnection(u) : u.openConnection());
        h.setConnectTimeout(8000);
        h.setReadTimeout(20000);
        h.setUseCaches(false);
        return h;
    }

    private static String updateBase() {
        String u = Config.UPDATE_URL == null ? "" : Config.UPDATE_URL.trim();
        if (u.isEmpty()) return null;
        return u.endsWith("/") ? u : u + "/";
    }

    // ---------------------------------------------------------------- update tampilan
    private void checkWebUpdate(boolean manual) {
        String base = updateBase();
        if (base == null) { if (manual) js("window.onWebUpdate&&onWebUpdate('noconfig','')"); return; }
        try {
            JSONObject j = new JSONObject(httpGetString(base + "version.json"));
            String ver = j.getString("version"), file = j.optString("file", "index.html");
            if (verCmp(ver, currentWebVersion()) <= 0) { if (manual) js("window.onWebUpdate&&onWebUpdate('none'," + JSONObject.quote(ver) + ")"); return; }
            File tmp = new File(getFilesDir(), "web/index.tmp");
            tmp.getParentFile().mkdirs();
            download(base + file, tmp, null);
            String head = readHead(tmp, 400);
            if (!head.contains("<html") && !head.contains("<!DOCTYPE")) throw new IOException("bukan html");
            if (!tmp.renameTo(webFile())) { webFile().delete(); if (!tmp.renameTo(webFile())) throw new IOException("rename"); }
            prefs.edit().putString("webVer", ver).apply();
            js("window.onWebUpdate&&onWebUpdate('ready'," + JSONObject.quote(ver) + ")");
        } catch (Exception e) {
            Log.w(TAG, "cek tampilan: " + e);
            if (manual) js("window.onWebUpdate&&onWebUpdate('error','')");
        }
    }

    // ---------------------------------------------------------------- firmware alat
    private File fwFile(String name) { File d = new File(getFilesDir(), "fw"); d.mkdirs(); return new File(d, name); }

    private void checkFirmware(boolean manual) {
        String base = updateBase();
        if (base == null) { if (manual) fwInfo("{\"state\":\"noconfig\"}"); return; }
        try {
            JSONObject j = new JSONObject(httpGetString(base + "firmware.json"));
            String ver = j.getString("version"), file = j.getString("file"), notes = j.optString("notes", "");
            String have = prefs.getString("fwNetVer", "0");
            if (verCmp(ver, have) <= 0 && fwFile("net.bin").exists()) {
                if (manual) { usePending("net"); fwInfo(pendingJson("ready")); }
                return;
            }
            File tmp = fwFile("net.tmp");
            download(base + file, tmp, null);
            if (!isEsp32Bin(tmp)) throw new IOException("bukan firmware ESP32");
            File dst = fwFile("net.bin"); dst.delete();
            if (!tmp.renameTo(dst)) throw new IOException("rename");
            prefs.edit().putString("fwNetVer", ver).putString("fwNetNotes", notes).putString("fwNetName", file).apply();
            if (manual) { usePending("net"); fwInfo(pendingJson("ready")); }
        } catch (Exception e) {
            Log.w(TAG, "cek firmware: " + e);
            if (manual) fwInfo("{\"state\":\"error\"}");
        }
    }

    private void usePending(String src) { prefs.edit().putString("fwSrc", src).apply(); }

    private File pendingFile() { return fwFile("picked".equals(prefs.getString("fwSrc", "net")) ? "picked.bin" : "net.bin"); }

    private String pendingJson(String state) {
        try {
            boolean picked = "picked".equals(prefs.getString("fwSrc", "net"));
            File f = pendingFile();
            if (!f.exists()) return "null";
            JSONObject o = new JSONObject();
            o.put("state", state);
            o.put("src", picked ? "file" : "net");
            o.put("ver", picked ? "" : prefs.getString("fwNetVer", ""));
            o.put("notes", picked ? "" : prefs.getString("fwNetNotes", ""));
            o.put("name", picked ? prefs.getString("fwPickName", "firmware.bin") : prefs.getString("fwNetName", "firmware.bin"));
            o.put("size", f.length());
            return o.toString();
        } catch (Exception e) { return "null"; }
    }

    private void fwInfo(String json) { js("window.onFwInfo&&onFwInfo(" + JSONObject.quote(json) + ")"); }

    private static boolean isEsp32Bin(File f) {
        try (InputStream in = new FileInputStream(f)) { return in.read() == 0xE9 && f.length() > 64 * 1024; }
        catch (IOException e) { return false; }
    }

    /** Kirim firmware ke alat: POST multipart ke http://host/update (Basic auth), sama seperti OTA lewat browser. */
    private void flashFirmware(String host, String user, String pass) {
        File f = pendingFile();
        if (!f.exists()) { js("onFwDone(false,'Belum ada file firmware')"); return; }
        HttpURLConnection h = null;
        long sent = 0, total = f.length();
        try {
            String boundary = "----RS16" + System.currentTimeMillis();
            byte[] pre = ("--" + boundary + "\r\nContent-Disposition: form-data; name=\"update\"; filename=\"firmware.bin\"\r\n"
                    + "Content-Type: application/octet-stream\r\n\r\n").getBytes(StandardCharsets.UTF_8);
            byte[] post = ("\r\n--" + boundary + "--\r\n").getBytes(StandardCharsets.UTF_8);
            URL u = new URL("http://" + host + "/update");
            Network n = bound;   // pakai WiFi alat
            h = (HttpURLConnection) (n != null ? n.openConnection(u) : u.openConnection());
            h.setConnectTimeout(8000);
            h.setReadTimeout(60000);
            h.setDoOutput(true);
            h.setRequestMethod("POST");
            h.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + boundary);
            String auth = Base64.encodeToString((user + ":" + pass).getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
            h.setRequestProperty("Authorization", "Basic " + auth);
            h.setFixedLengthStreamingMode(pre.length + total + post.length);
            int lastPct = -1;
            try (OutputStream out = h.getOutputStream(); InputStream in = new FileInputStream(f)) {
                out.write(pre);
                byte[] buf = new byte[4096];
                int r;
                while ((r = in.read(buf)) > 0) {
                    out.write(buf, 0, r);
                    sent += r;
                    int pct = (int) (sent * 100 / total);
                    if (pct != lastPct) { lastPct = pct; js("window.onFwProgress&&onFwProgress(" + pct + ")"); }
                }
                out.write(post);
                out.flush();
            }
            int code = h.getResponseCode();
            if (code == 200) js("onFwDone(true,'')");
            else if (code == 401) js("onFwDone(false,'Password salah')");
            else js("onFwDone(false," + JSONObject.quote("Alat menolak firmware (" + code + ")") + ")");
        } catch (IOException e) {
            // alat bisa restart sebelum jawaban terbaca: kalau semua data sudah terkirim, anggap berhasil
            if (sent >= total && total > 0) js("onFwDone(true,'')");
            else js("onFwDone(false," + JSONObject.quote("Koneksi ke alat terputus: " + e.getMessage()) + ")");
        } finally {
            if (h != null) h.disconnect();
        }
    }

    // ---------------------------------------------------------------- jembatan JavaScript
    private class Bridge {
        @JavascriptInterface
        public String info() {
            try {
                JSONObject o = new JSONObject();
                o.put("app", Config.APP_VERSION);
                o.put("web", currentWebVersion());
                o.put("updateUrl", Config.UPDATE_URL);
                return o.toString();
            } catch (Exception e) { return "{}"; }
        }

        @JavascriptInterface
        public void saveFile(String name, String text) {
            String safe = name.replaceAll("[^\\w.\\-]+", "_");
            try {
                if (Build.VERSION.SDK_INT >= 29) {
                    ContentValues v = new ContentValues();
                    v.put(MediaStore.Downloads.DISPLAY_NAME, safe);
                    v.put(MediaStore.Downloads.MIME_TYPE, safe.endsWith(".json") ? "application/json" : "text/csv");
                    v.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/RS16");
                    Uri uri = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
                    try (OutputStream o = getContentResolver().openOutputStream(uri)) { o.write(text.getBytes(StandardCharsets.UTF_8)); }
                    toast("Tersimpan di Download/RS16/" + safe);
                } else {
                    File d = getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);
                    File f = new File(d, safe);
                    try (OutputStream o = new FileOutputStream(f)) { o.write(text.getBytes(StandardCharsets.UTF_8)); }
                    toast("Tersimpan: " + f.getAbsolutePath());
                }
            } catch (Exception e) { toast("Gagal menyimpan file"); Log.e(TAG, "save", e); }
        }

        @JavascriptInterface
        public void keepScreen(boolean on) {
            runOnUiThread(() -> {
                if (on) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            });
        }

        @JavascriptInterface
        public void vibrate(int ms) {
            Vibrator v = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            if (v != null) v.vibrate(VibrationEffect.createOneShot(Math.max(20, Math.min(ms, 1000)), VibrationEffect.DEFAULT_AMPLITUDE));
        }

        @JavascriptInterface
        public void reload() { runOnUiThread(() -> web.loadUrl(START_URL)); }

        @JavascriptInterface
        public void checkWebUpdate() { new Thread(() -> MainActivity.this.checkWebUpdate(true)).start(); }

        @JavascriptInterface
        public void checkFirmware() { new Thread(() -> MainActivity.this.checkFirmware(true)).start(); }

        @JavascriptInterface
        public String fwPending() { return prefs.getString("fwNetVer", "").isEmpty() && !fwFile("picked.bin").exists() ? "null" : pendingJson("ready"); }

        @JavascriptInterface
        public void pickFirmware() {
            runOnUiThread(() -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                try { startActivityForResult(i, REQ_FW); } catch (Exception e) { toast("Tidak ada aplikasi pemilih file"); }
            });
        }

        @JavascriptInterface
        public void deleteFirmware() {
            for (String n : new String[]{"picked.bin", "net.bin", "net.tmp"}) fwFile(n).delete();
            prefs.edit().remove("fwNetVer").remove("fwNetNotes").remove("fwNetName").remove("fwPickName").remove("fwSrc").apply();
        }

        @JavascriptInterface
        public void flashFirmware(String host, String user, String pass) {
            new Thread(() -> MainActivity.this.flashFirmware(host, user, pass)).start();
        }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_FILE) {
            Uri[] r = (res == RESULT_OK && data != null && data.getData() != null) ? new Uri[]{data.getData()} : null;
            if (fileCb != null) fileCb.onReceiveValue(r);
            fileCb = null;
        } else if (req == REQ_FW && res == RESULT_OK && data != null && data.getData() != null) {
            Uri u = data.getData();
            new Thread(() -> {
                File dst = fwFile("picked.bin");
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream out = new FileOutputStream(dst)) {
                    byte[] buf = new byte[8192]; int r;
                    while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
                } catch (Exception e) { fwInfo("{\"state\":\"error\"}"); return; }
                if (!isEsp32Bin(dst)) { dst.delete(); fwInfo("{\"state\":\"bad\"}"); return; }
                String name = u.getLastPathSegment() == null ? "firmware.bin" : u.getLastPathSegment();
                int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1);
                prefs.edit().putString("fwPickName", name).apply();
                usePending("picked");
                fwInfo(pendingJson("picked"));
            }).start();
        }
    }

    // ---------------------------------------------------------------- util
    private void js(String code) { runOnUiThread(() -> { if (web != null) web.evaluateJavascript(code, null); }); }

    private void toast(String m) { runOnUiThread(() -> Toast.makeText(this, m, Toast.LENGTH_LONG).show()); }

    private String httpGetString(String url) throws IOException {
        HttpURLConnection h = openInternet(url);
        try {
            if (h.getResponseCode() != 200) throw new IOException("HTTP " + h.getResponseCode());
            try (InputStream in = h.getInputStream()) { return readAll(in); }
        } finally { h.disconnect(); }
    }

    private void download(String url, File dst, Runnable progress) throws IOException {
        HttpURLConnection h = openInternet(url);
        try {
            if (h.getResponseCode() != 200) throw new IOException("HTTP " + h.getResponseCode());
            try (InputStream in = h.getInputStream(); OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[8192]; int r;
                while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
            }
        } finally { h.disconnect(); }
    }

    private static String readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192]; int r;
        while ((r = in.read(buf)) > 0) bo.write(buf, 0, r);
        return bo.toString("UTF-8");
    }

    private static String readHead(File f, int n) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[n]; int r = in.read(b);
            return r > 0 ? new String(b, 0, r, StandardCharsets.UTF_8) : "";
        } catch (IOException e) { return ""; }
    }

    /** Bandingkan versi "1.2.10" vs "1.2.9" per angka. */
    static int verCmp(String a, String b) {
        String[] x = (a == null ? "0" : a).split("\\."), y = (b == null ? "0" : b).split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? parse(x[i]) : 0, q = i < y.length ? parse(y[i]) : 0;
            if (p != q) return p > q ? 1 : -1;
        }
        return 0;
    }

    private static int parse(String s) { try { return Integer.parseInt(s.replaceAll("[^0-9]", "")); } catch (Exception e) { return 0; } }

    @Override
    public void onBackPressed() {
        if (web != null && web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }

    @Override protected void onResume() { super.onResume(); if (web != null) web.onResume(); }
    @Override protected void onPause() { if (web != null) web.onPause(); super.onPause(); }

    @Override
    protected void onDestroy() {
        try { if (cm != null && wifiCb != null) cm.unregisterNetworkCallback(wifiCb); } catch (Exception ignored) { }
        if (web != null) { web.destroy(); web = null; }
        super.onDestroy();
    }
}
