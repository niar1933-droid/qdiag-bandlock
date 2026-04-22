/*
 * qdiag_helper — standalone ARM64 ELF executable. Runs OUTSIDE the app's
 * classloader linker namespace (clns-1), so it can freely dlopen /vendor/
 * libraries which are otherwise blocked.
 *
 * Launched via `su -c` from the root service. Protocol is line-based on
 * stdout for easy parsing from Kotlin/JNI.
 *
 * This revision focuses on *discovery*: on X70/HyperOS the classic
 * `*_v01.so` IDL libraries are absent from /vendor/lib64, so before we can
 * go any further we need a full inventory of every place Qualcomm might
 * ship QMI service objects on this firmware. We:
 *   - list ALL `*qmi*`, `*nas*`, `*dms*`, `*_v01.so` files under a set of
 *     candidate directories (pure dirent scan, no dlopen yet);
 *   - dlopen every discovered candidate + the fixed dep set;
 *   - probe ~40 known QMI service-object symbols (`<svc>_get_service_object_v01`)
 *     against every open handle AND RTLD_DEFAULT so we catch service
 *     objects inlined into non-obvious libs (e.g. libqmiservices.so);
 *   - record which canonical CCI entrypoints are present so the Kotlin
 *     caller still gets the old MASK 0x???? feedback line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>

/* Dual-destination printf: stdout (seen by jni_bridge popen) AND a log file
 * the user can cat from the shell if logcat buffer rolls over. */
static FILE *g_log_fp = NULL;

#define HOUT(...) do { \
    fprintf(stdout, __VA_ARGS__); fflush(stdout); \
    if (g_log_fp) { fprintf(g_log_fp, __VA_ARGS__); fflush(g_log_fp); } \
} while (0)

/* Opens each lib with RTLD_NOW|RTLD_GLOBAL. Stores handle for later dlsym(). */
typedef struct { char path[256]; void *h; } lib_t;

#define MAX_LIBS 256
static lib_t g_libs[MAX_LIBS];
static int   g_lib_count = 0;

static int lib_already_tracked(const char *path) {
    for (int i = 0; i < g_lib_count; i++) {
        if (strcmp(g_libs[i].path, path) == 0) return 1;
    }
    return 0;
}

static void lib_track(const char *path, void *h) {
    if (g_lib_count >= MAX_LIBS) return;
    if (lib_already_tracked(path)) return;
    snprintf(g_libs[g_lib_count].path, sizeof(g_libs[g_lib_count].path), "%s", path);
    g_libs[g_lib_count].h = h;
    g_lib_count++;
}

/* Candidate directories Qualcomm has been observed to use for QMI runtime
 * libraries across SDX5x..X75 generations. Order doesn't matter — we scan
 * everything for diagnostic completeness. */
static const char *g_scan_dirs[] = {
    "/vendor/lib64",
    "/vendor/lib64/hw",
    "/vendor/lib64/vndk-sp",
    "/vendor/lib64/vndk-sp-29",
    "/vendor/lib64/vndk-sp-30",
    "/vendor/lib64/vndk-sp-31",
    "/vendor/lib64/vndk-sp-32",
    "/vendor/lib64/vndk-sp-33",
    "/vendor/lib64/vndk-sp-34",
    "/odm/lib64",
    "/odm/lib64/hw",
    "/system_ext/lib64",
    "/system/lib64",
    "/system/lib64/vndk-sp-29",
    "/system/lib64/vndk-sp-30",
    "/system/lib64/vndk-sp-31",
    "/system/lib64/vndk-sp-32",
    "/system/lib64/vndk-sp-33",
    "/system/lib64/vndk-sp-34",
    NULL,
};

/* Return non-zero if the filename matches a "we want to look inside this"
 * heuristic: anything that looks like QMI machinery, or any QMI IDL lib. */
static int interesting_libname(const char *name, size_t n) {
    if (n < 6) return 0;
    if (strncmp(name, "lib", 3) != 0) return 0;
    if (n < 4 || strcmp(name + n - 3, ".so") != 0) return 0;
    /* IDL libs: end in "_v01.so". Very strong signal. */
    if (n > 7 && strcmp(name + n - 7, "_v01.so") == 0) return 1;
    /* Specific service API libs historically seen on MSM vendor partitions. */
    if (strncmp(name, "libnas",   6) == 0) return 1;
    if (strncmp(name, "libdms",   6) == 0) return 1;
    if (strncmp(name, "libwds",   6) == 0) return 1;
    if (strncmp(name, "libmodem", 8) == 0) return 1;
    if (strncmp(name, "libqmi",   6) == 0) return 1; /* libqmi_cci, libqmiservices, libqmi_*api*, etc. */
    if (strncmp(name, "libril",   6) == 0) return 1; /* some firmware embeds service objects in RIL */
    return 0;
}

/* Fixed dep set — always dlopen even if dirent scan misses them (for
 * devices where /vendor is mounted read-only with restrictive search
 * permissions for `opendir` but direct `open` still works). */
static const char *g_fixed_deps[] = {
    "/vendor/lib64/libcutils.so",
    "/vendor/lib64/libdiag.so",
    "/vendor/lib64/libqmi_cci.so",
    "/vendor/lib64/libqmi_common_so.so",
    "/vendor/lib64/libqmi_encdec.so",
    "/vendor/lib64/libqmi_csi.so",
    "/vendor/lib64/libqmi_client_helper.so",
    "/vendor/lib64/libqmi_client_qmux.so",
    "/vendor/lib64/libqmiservices.so",
    "/vendor/lib64/libqmi_legacy.so",
    "/vendor/lib64/libqmi.so",
    NULL,
};

/* Canonical QMI CCI entrypoints — for MASK feedback to Kotlin. */
static const char *g_cci_syms[] = {
    "qmi_client_init_instance",
    "qmi_client_init_instance_v2",
    "qmi_client_init",
    "qmi_client_send_msg_sync",
    "qmi_client_send_msg_sync_ex",
    "qmi_client_send_msg_async",
    "qmi_client_release",
    "qmi_client_get_service_instance",
    "qmi_client_register_notify_cb",
    "qmi_client_notifier_init",
    "qmi_client_set_error_code_translation_table",
    "qmi_cci_init",
    "qmi_cci_client_init_instance",
    "qmi_idl_get_service_object_v01",
    "qmi_linux_get_internal_use_port",
    "qmi_linux_get_conn_id_by_name",
    NULL,
};

/* Broad QMI service roster. Every one of these, if defined in any loaded
 * library, is exported as `<name>_get_service_object_v01`. Catches NAS
 * even when it's shipped under an unusual lib name. */
static const char *g_service_roots[] = {
    "nas", "dms", "wds", "voice", "pds", "loc", "cat",
    "uim", "pbm", "rms", "cfg", "pds2", "test",
    "qmi_vs", "qmi_vss",
    "ims", "ims_settings", "ims_rtp", "ims_video", "ims_vt", "ims_presence",
    "qcmap_msgr", "qcmap", "qcmap_cm", "qcsi",
    "sns", "sar", "tune",
    "radio_if", "radio", "radio_ng",
    "mdc", "modem_lte", "modem_nas", "modem_sys",
    "qcvdd", "data_services", "sec_tee", "rmtfs",
    "lte_ioe", "lte_nas", "lte_rrc", "nr5g_nas", "nr5g_rrc",
    "ssctl", "ssctlqmi", "swila",
    NULL,
};

static void scan_dir_listing(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        HOUT("DIRLIST %-40s OPEN_FAIL: %s\n", dir, strerror(errno));
        return;
    }
    HOUT("DIRLIST %-40s OPEN_OK\n", dir);
    struct dirent *e;
    int shown = 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (!interesting_libname(e->d_name, n)) continue;
        HOUT("CAND  %s/%s\n", dir, e->d_name);
        shown++;
    }
    HOUT("DIRLIST %-40s COUNT=%d\n", dir, shown);
    closedir(d);
}

static void try_dlopen_and_track(const char *path) {
    if (lib_already_tracked(path)) return;
    struct stat st;
    if (stat(path, &st) != 0) {
        /* Silently skip missing files in the wide scan (we'd drown the log
         * otherwise); fixed-dep dlopen below still reports explicitly. */
        return;
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        const char *err = dlerror();
        HOUT("SCAN FAIL %s: %s\n", path, err ? err : "(null)");
        return;
    }
    HOUT("SCAN OK %s\n", path);
    lib_track(path, h);
}

/* Second pass: scan every candidate dir again, this time dlopen()ing
 * every matching lib and recording the handle. */
static void scan_dir_dlopen(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (!interesting_libname(e->d_name, n)) continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        try_dlopen_and_track(p);
    }
    closedir(d);
}

/* For the given symbol, search every tracked lib + RTLD_DEFAULT. Returns
 * 1 if resolved and prints the hit. */
static int probe_sym(const char *sym, int quiet_on_miss) {
    void *found = NULL;
    const char *where = NULL;
    for (int i = 0; i < g_lib_count; i++) {
        if (!g_libs[i].h) continue;
        void *p = dlsym(g_libs[i].h, sym);
        if (p) { found = p; where = g_libs[i].path; break; }
    }
    if (!found) {
        void *p = dlsym(RTLD_DEFAULT, sym);
        if (p) { found = p; where = "RTLD_DEFAULT"; }
    }
    if (found) {
        HOUT("SYM OK %-46s -> %p @ %s\n", sym, found, where);
        return 1;
    }
    if (!quiet_on_miss) HOUT("SYM MISS %s\n", sym);
    return 0;
}

static void probe(void) {
    g_log_fp = fopen("/data/local/tmp/qdiag_helper.log", "w");
    HOUT("=== qdiag_helper probe start ===\n");

    /* -------- PHASE 1: listing -------- */
    HOUT("---- PHASE 1: directory listings ----\n");
    for (int i = 0; g_scan_dirs[i]; i++) scan_dir_listing(g_scan_dirs[i]);

    /* -------- PHASE 2: dlopen fixed deps -------- */
    HOUT("---- PHASE 2: fixed dep dlopen ----\n");
    for (int i = 0; g_fixed_deps[i]; i++) {
        void *h = dlopen(g_fixed_deps[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            HOUT("DEP OK %s\n", g_fixed_deps[i]);
            lib_track(g_fixed_deps[i], h);
        } else {
            const char *err = dlerror();
            HOUT("DEP FAIL %s: %s\n", g_fixed_deps[i], err ? err : "(null)");
        }
    }

    /* -------- PHASE 3: dlopen all interesting candidates -------- */
    HOUT("---- PHASE 3: wide scan dlopen ----\n");
    for (int i = 0; g_scan_dirs[i]; i++) scan_dir_dlopen(g_scan_dirs[i]);
    HOUT("TRACKED %d libs after wide scan\n", g_lib_count);

    /* -------- PHASE 4: CCI entrypoint mask -------- */
    HOUT("---- PHASE 4: CCI entrypoint probe ----\n");
    int mask = 0;
    int cci_hits = 0, cci_total = 0;
    for (int s = 0; g_cci_syms[s]; s++) {
        cci_total++;
        int hit = probe_sym(g_cci_syms[s], 0);
        if (hit) {
            cci_hits++;
            if (s < 16) mask |= (1 << s);
        }
    }
    HOUT("CCI %d/%d\n", cci_hits, cci_total);

    /* -------- PHASE 5: service-object hunt -------- */
    HOUT("---- PHASE 5: service-object hunt ----\n");
    int svc_hits = 0;
    for (int s = 0; g_service_roots[s]; s++) {
        char sym[128];
        snprintf(sym, sizeof(sym), "%s_get_service_object_v01", g_service_roots[s]);
        if (probe_sym(sym, 1)) svc_hits++;
    }
    HOUT("SERVICES %d service objects resolved\n", svc_hits);

    /* -------- Summary (machine-parsed by jni_bridge) -------- */
    HOUT("FOUND %d/%d\n", cci_hits + svc_hits, cci_total + (int)(sizeof(g_service_roots)/sizeof(*g_service_roots)) - 1);
    HOUT("MAIN OK (symbol scan done)\n");
    HOUT("MASK 0x%04X %d/%d\n", mask, cci_hits, cci_total);
    HOUT("=== qdiag_helper probe end ===\n");
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        probe();
        return 0;
    }
    fprintf(stderr, "usage: %s probe\n", argv[0]);
    return 2;
}
