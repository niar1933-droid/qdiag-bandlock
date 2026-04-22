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

static void probe(void) {
    const char *deps[] = {
        "/vendor/lib64/libcutils.so",
        "/vendor/lib64/libdiag.so",
        "/vendor/lib64/libqmi_cci.so",
        "/vendor/lib64/libqmi_common_so.so",
        "/vendor/lib64/libqmi_encdec.so",
        "/vendor/lib64/libqmi_csi.so",
        "/vendor/lib64/libqmi_client_helper.so",
        NULL,
    };
    for (int i = 0; deps[i]; i++) {
        void *h = dlopen(deps[i], RTLD_NOW | RTLD_GLOBAL);
        const char *err = h ? "ok" : dlerror();
        fprintf(stdout, "DEP %s %s\n", h ? "OK" : "FAIL", deps[i]);
        if (!h) fprintf(stdout, "DEP_ERR %s: %s\n", deps[i], err ? err : "(null)");
        fflush(stdout);
    }

    void *q = dlopen("/vendor/lib64/libqmi_client_qmux.so", RTLD_NOW | RTLD_GLOBAL);
    if (!q) {
        const char *err = dlerror();
        fprintf(stdout, "MAIN FAIL libqmi_client_qmux.so\n");
        fprintf(stdout, "MAIN_ERR %s\n", err ? err : "(null)");
        fflush(stdout);
        return;
    }
    fprintf(stdout, "MAIN OK libqmi_client_qmux.so -> %p\n", q);

    const char *syms[] = {
        "qmi_client_init_instance",
        "qmi_client_init",
        "qmi_client_send_msg_sync",
        "qmi_client_send_msg_async",
        "qmi_client_release",
        "qmi_linux_get_internal_use_port",
        "qmi_linux_get_conn_id_by_name",
        "qmi_idl_get_service_object_v01",
        "qmuxd_get_service_object",
        NULL,
    };
    int mask = 0, found = 0;
    for (int i = 0; syms[i]; i++) {
        void *p = dlsym(q, syms[i]);
        if (p) {
            mask |= (1 << i);
            found++;
            fprintf(stdout, "SYM OK %s -> %p\n", syms[i], p);
        } else {
            fprintf(stdout, "SYM MISS %s\n", syms[i]);
        }
    }
    fprintf(stdout, "MASK 0x%04X %d/%d\n", mask, found,
            (int)(sizeof(syms)/sizeof(*syms))-1);
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
