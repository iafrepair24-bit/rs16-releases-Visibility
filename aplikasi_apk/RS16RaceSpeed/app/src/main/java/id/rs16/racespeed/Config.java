package id.rs16.racespeed;

/** Pengaturan aplikasi RS16. Ubah di sini saja. */
public final class Config {
    /** Versi aplikasi (samakan dengan versionName di app/build.gradle). */
    public static final String APP_VERSION = "1.3.1";

    /**
     * ALAMAT SERVER UPDATE, wajib diakhiri "/".
     * Di folder itu taruh: version.json + index.html (update tampilan)
     *                      firmware.json + file .bin (update firmware alat)
     * Contoh: "https://rs16-update.netlify.app/"
     * Biarkan "" kalau belum punya -> cek update otomatis dimatikan.
     */
    public static final String UPDATE_URL = "";

    private Config() { }
}
