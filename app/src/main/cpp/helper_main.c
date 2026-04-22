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

/* Opens each lib with RTLD_NOW|RTLD_GLOBAL. Stores handle for later dlsym(). */
typedef struct { const char *path; void *h; } lib_t;

static void probe(void) {
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
        fprintf(stdout, "DEP %s %s\n", libs[i].h ? "OK" : "FAIL", libs[i].path);
        if (!libs[i].h) fprintf(stdout, "DEP_ERR %s: %s\n",
                                libs[i].path, err ? err : "(null)");
        fflush(stdout);
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
            fprintf(stdout, "SYM OK %-46s -> %p @ %s\n", syms[s], found_at, found_path);
        } else {
            fprintf(stdout, "SYM MISS %s\n", syms[s]);
        }
    }

    fprintf(stdout, "FOUND %d/%d\n", resolved, total);
    /* Legacy MASK line kept so jni_bridge parser still captures something non-zero
     * when we have at least one canonical client symbol. Bit layout = first 16 syms. */
    int mask = 0;
    for (int s = 0; s < 16 && syms[s]; s++) {
        void *p = dlsym(RTLD_DEFAULT, syms[s]);
        if (p) mask |= (1 << s);
    }
    fprintf(stdout, "MAIN OK (symbol scan done)\n");
    fprintf(stdout, "MASK 0x%04X %d/%d\n", mask, resolved, total);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        probe();
        return 0;
    }
    fprintf(stderr, "usage: %s probe\n", argv[0]);
    return 2;
}
