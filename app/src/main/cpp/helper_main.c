/*
 * qdiag_helper — standalone ARM64 ELF executable. Runs OUTSIDE the app's
 * classloader linker namespace (clns-1), so it can freely dlopen /vendor/
 * libraries which are otherwise blocked.
 *
 * Launched via `su -c` from the root service. Protocol is line-based on
 * stdout for easy parsing from Kotlin/JNI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>

/* Dual-destination printf: stdout (seen by jni_bridge popen) AND a log file
 * the user can cat from the shell if logcat buffer rolls over. */
static FILE *g_log_fp = NULL;

#define HOUT(...) do { \
    fprintf(stdout, __VA_ARGS__); fflush(stdout); \
    if (g_log_fp) { fprintf(g_log_fp, __VA_ARGS__); fflush(g_log_fp); } \
} while (0)

/* Opens each lib with RTLD_NOW|RTLD_GLOBAL. Stores handle for later dlsym(). */
typedef struct { const char *path; void *h; } lib_t;

static void probe(void) {
    g_log_fp = fopen("/data/local/tmp/qdiag_helper.log", "w");
    HOUT("=== qdiag_helper probe start ===\n");
    lib_t libs[] = {
        { "/vendor/lib64/libcutils.so",            NULL },
        { "/vendor/lib64/libdiag.so",              NULL },
        { "/vendor/lib64/libqmi_cci.so",           NULL },
        { "/vendor/lib64/libqmi_common_so.so",     NULL },
        { "/vendor/lib64/libqmi_encdec.so",        NULL },
        { "/vendor/lib64/libqmi_csi.so",           NULL },
        { "/vendor/lib64/libqmi_client_helper.so", NULL },
        { "/vendor/lib64/libqmi_client_qmux.so",   NULL },
        /* extra candidates that may host the QMI client API on newer MSM */
        { "/vendor/lib64/libqmiservices.so",       NULL },
        { "/vendor/lib64/libqmi_legacy.so",        NULL },
        { "/vendor/lib64/libqmi.so",               NULL },
        { NULL, NULL },
    };
    for (int i = 0; libs[i].path; i++) {
        libs[i].h = dlopen(libs[i].path, RTLD_NOW | RTLD_GLOBAL);
        const char *err = libs[i].h ? "ok" : dlerror();
        HOUT("DEP %s %s\n", libs[i].h ? "OK" : "FAIL", libs[i].path);
        if (!libs[i].h) HOUT("DEP_ERR %s: %s\n", libs[i].path, err ? err : "(null)");
    }

    /* Wide symbol search: MSM QMI CCI + qmuxd variants + NAS service objs. */
    const char *syms[] = {
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
        "nas_get_service_object_v01",
        "dms_get_service_object_v01",
        "qmuxd_get_service_object",
        "qmi_linux_get_internal_use_port",
        "qmi_linux_get_conn_id_by_name",
        "QmiCciInit",
        "QmiClientInitInstance",
        "QmiClientSendMsgSync",
        NULL,
    };

    /* For each symbol, scan every loaded lib handle AND RTLD_DEFAULT.
     * Print the FIRST lib that exports it so we know where it lives. */
    int resolved = 0;
    int total = 0;
    for (int s = 0; syms[s]; s++) {
        total++;
        void *found_at = NULL;
        const char *found_path = NULL;
        for (int l = 0; libs[l].path; l++) {
            if (!libs[l].h) continue;
            void *p = dlsym(libs[l].h, syms[s]);
            if (p) { found_at = p; found_path = libs[l].path; break; }
        }
        if (!found_at) {
            void *p = dlsym(RTLD_DEFAULT, syms[s]);
            if (p) { found_at = p; found_path = "RTLD_DEFAULT"; }
        }
        if (found_at) {
            resolved++;
            HOUT("SYM OK %-46s -> %p @ %s\n", syms[s], found_at, found_path);
        } else {
            HOUT("SYM MISS %s\n", syms[s]);
        }
    }

    HOUT("FOUND(initial) %d/%d\n", resolved, total);

    /* Scan /vendor/lib64/ for QMI IDL service-object libs only. Qualcomm's
     * IDL convention is `lib<service>_<iface>_v01.so` or `libnas_api.so`
     * etc. Avoid the noisy RIL libs which self-register message dispatchers
     * in their ctors. */
    DIR *d = opendir("/vendor/lib64");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (n < 7) continue;
            if (strncmp(e->d_name, "lib", 3)) continue;
            if (strcmp(e->d_name + n - 3, ".so")) continue;
            int match = 0;
            /* QMI IDL libs: name ending in _v01.so */
            if (n > 7 && strcmp(e->d_name + n - 7, "_v01.so") == 0) match = 1;
            /* Specific service API libs */
            else if (strncmp(e->d_name, "libnas_",   7) == 0) match = 1;
            else if (strncmp(e->d_name, "libdms_",   7) == 0) match = 1;
            else if (strncmp(e->d_name, "libwds_",   7) == 0) match = 1;
            else if (strncmp(e->d_name, "libmodem_", 9) == 0) match = 1;
            if (!match) continue;
            char p[512];
            snprintf(p, sizeof(p), "/vendor/lib64/%s", e->d_name);
            /* Skip already-loaded libs */
            int already = 0;
            for (int l = 0; libs[l].path; l++)
                if (strcmp(libs[l].path, p) == 0) { already = 1; break; }
            if (already) continue;
            void *hh = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
            if (!hh) {
                const char *err = dlerror();
                HOUT("SCAN FAIL %s: %s\n", p, err ? err : "(null)");
                continue;
            }
            HOUT("SCAN OK %s\n", p);
            static const char *ssyms[] = {
                "nas_get_service_object_v01",
                "dms_get_service_object_v01",
                "wds_get_service_object_v01",
                "qmi_idl_get_service_object_v01",
            };
            for (size_t s = 0; s < sizeof(ssyms)/sizeof(*ssyms); s++) {
                void *sp = dlsym(hh, ssyms[s]);
                if (sp) {
                    HOUT("SERVICE_OBJ %-32s -> %p @ %s\n", ssyms[s], sp, p);
                    if (strcmp(ssyms[s], "nas_get_service_object_v01") == 0) resolved++;
                }
            }
        }
        closedir(d);
    } else {
        HOUT("SCAN_ERR opendir(/vendor/lib64): %s\n", strerror(errno));
    }

    /* Also re-scan RTLD_DEFAULT for the service-object symbols; some libs
     * pull them in transitively even without an explicit dlopen. */
    static const char *ssyms2[] = {
        "nas_get_service_object_v01",
        "dms_get_service_object_v01",
        "wds_get_service_object_v01",
    };
    for (size_t s = 0; s < sizeof(ssyms2)/sizeof(*ssyms2); s++) {
        void *p = dlsym(RTLD_DEFAULT, ssyms2[s]);
        if (p) HOUT("SERVICE_OBJ_RTLD %-32s -> %p\n", ssyms2[s], p);
    }

    HOUT("FOUND %d/%d\n", resolved, total);
    /* Legacy MASK line kept so jni_bridge parser still captures something non-zero
     * when we have at least one canonical client symbol. Bit layout = first 16 syms. */
    int mask = 0;
    for (int s = 0; s < 16 && syms[s]; s++) {
        void *p = dlsym(RTLD_DEFAULT, syms[s]);
        if (p) mask |= (1 << s);
    }
    HOUT("MAIN OK (symbol scan done)\n");
    HOUT("MASK 0x%04X %d/%d\n", mask, resolved, total);
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
