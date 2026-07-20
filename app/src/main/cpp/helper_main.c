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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <elf.h>
#include <signal.h>
#include <setjmp.h>

/* Signal-safe wrapper: some of the IDL-struct walking code dereferences
 * pointers whose offsets we inferred from reverse-engineering. If the
 * offset is wrong we'd SEGV; a longjmp-in-handler keeps the helper alive
 * and lets subsequent phases still run. */
static sigjmp_buf g_fault_jmp;
static volatile int g_fault_armed = 0;

static void fault_handler(int sig) {
    (void)sig;
    if (g_fault_armed) {
        g_fault_armed = 0;
        siglongjmp(g_fault_jmp, 1);
    }
}

static void install_fault_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sa.sa_flags   = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}

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

/* ---------- ELF dynsym walker (Phase 6) ----------
 *
 * X70/HyperOS ships no `*_v01.so` and no `<svc>_get_service_object_v01`, but
 * it DOES ship libqmiservices.so / libqmiextservices.so / librilqmimiscservices.so
 * / libril*.so / libqmi_legacy.so — which historically embed the NAS/DMS
 * handlers under non-standard symbol names (e.g. `nas_send_sys_sel_pref_req`,
 * `dms_set_operating_mode`, `ril_nas_lock_pci`, ...). Walk each file's
 * .dynsym by hand and print every defined symbol whose name contains any of
 * our interest substrings. This is pure read-only mmap of the on-disk ELF;
 * dlopen isn't required.
 */
static const char *g_elf_needle[] = {
    "nas_", "_nas_", "dms_", "_dms_", "wds_", "_wds_",
    "service_object", "ser_obj",
    "band_pref", "bandpref", "band_list",
    "sys_sel", "sys_selection", "selection_pref",
    "pci_lock", "pcilock", "earfcn_lock",
    "cell_lock", "celllock", "lock_cell", "lock_info", "LOCK_",
    "lte_nas", "nr5g_nas", "nr_nas",
    "ril_nas", "ril_dms", "ril_wds",
    NULL,
};

static int name_matches_needle(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; g_elf_needle[i]; i++) {
        if (strstr(name, g_elf_needle[i])) return 1;
    }
    return 0;
}

static void dump_elf_symbols(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        HOUT("ELF OPEN_FAIL %s: %s\n", path, strerror(errno));
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        HOUT("ELF STAT_FAIL %s\n", path);
        close(fd);
        return;
    }
    size_t fsize = (size_t)st.st_size;
    void *map = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        HOUT("ELF MMAP_FAIL %s: %s\n", path, strerror(errno));
        return;
    }

    const unsigned char *base = (const unsigned char *)map;
    /* ELF magic check */
    if (memcmp(base, ELFMAG, SELFMAG) != 0 ||
        base[EI_CLASS] != ELFCLASS64 || base[EI_DATA] != ELFDATA2LSB) {
        HOUT("ELF BAD_MAGIC %s (class=%d data=%d)\n",
             path, base[EI_CLASS], base[EI_DATA]);
        munmap(map, fsize);
        return;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (eh->e_shoff == 0 || eh->e_shnum == 0 ||
        eh->e_shoff + (Elf64_Xword)eh->e_shnum * sizeof(Elf64_Shdr) > fsize) {
        HOUT("ELF BAD_SHT %s\n", path);
        munmap(map, fsize);
        return;
    }

    const Elf64_Shdr *shtab = (const Elf64_Shdr *)(base + eh->e_shoff);

    /* Find .dynsym + linked .dynstr. There's typically exactly one SHT_DYNSYM. */
    const Elf64_Shdr *dynsym_sh = NULL;
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (shtab[i].sh_type == SHT_DYNSYM) { dynsym_sh = &shtab[i]; break; }
    }
    if (!dynsym_sh) {
        HOUT("ELF NO_DYNSYM %s\n", path);
        munmap(map, fsize);
        return;
    }
    if (dynsym_sh->sh_link >= eh->e_shnum) {
        HOUT("ELF BAD_DYNSTR_LINK %s\n", path);
        munmap(map, fsize);
        return;
    }
    const Elf64_Shdr *dynstr_sh = &shtab[dynsym_sh->sh_link];
    if (dynstr_sh->sh_type != SHT_STRTAB ||
        dynstr_sh->sh_offset + dynstr_sh->sh_size > fsize ||
        dynsym_sh->sh_offset + dynsym_sh->sh_size > fsize ||
        dynsym_sh->sh_entsize == 0) {
        HOUT("ELF BAD_TABLES %s\n", path);
        munmap(map, fsize);
        return;
    }

    const Elf64_Sym *syms = (const Elf64_Sym *)(base + dynsym_sh->sh_offset);
    const char      *strs = (const char *)(base + dynstr_sh->sh_offset);
    size_t           nsym = (size_t)(dynsym_sh->sh_size / dynsym_sh->sh_entsize);

    int hits = 0;
    for (size_t i = 0; i < nsym; i++) {
        const Elf64_Sym *s = &syms[i];
        /* Skip undefined (imports), only emit defined exports. */
        if (s->st_shndx == SHN_UNDEF) continue;
        /* Only globals and weaks; skip locals (compiler-internal). */
        unsigned bind = ELF64_ST_BIND(s->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        /* Only funcs + objects; skip file/section symbols. */
        unsigned type = ELF64_ST_TYPE(s->st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
        if (s->st_name >= dynstr_sh->sh_size) continue;
        const char *name = strs + s->st_name;
        if (!name_matches_needle(name)) continue;
        const char *kind = (type == STT_FUNC) ? "FUNC"
                         : (type == STT_OBJECT) ? "OBJ"
                         : "NOT";
        HOUT("ELFSYM %-48s %4s %s\n", name, kind, path);
        hits++;
        if (hits >= 256) { /* paranoid cap per-lib */
            HOUT("ELFSYM %s HIT_CAP reached\n", path);
            break;
        }
    }
    if (hits == 0) HOUT("ELFSYM %s NONE\n", path);
    munmap(map, fsize);
}

/* ---------- Phase 7: live CCI ping ----------
 *
 * We actually call qmi_client_init_instance() with the service-object
 * pointers Phase 6 discovered (nas_qmi_idl_service_object_v01,
 * nas_ext_qmi_idl_service_object_v01). If this succeeds we KNOW the QRTR
 * transport is up end-to-end — ~libqmi_cci returned a real handle that
 * the modem has ack'd. We then release the handle cleanly.
 *
 * CCI ABI we depend on (stable since ~2013 across all MSM vendor stacks):
 *   qmi_client_init_instance(service_obj,
 *                            instance_id,       // uint32_t — 0 = any
 *                            ind_cb, ind_cb_data,
 *                            os_params,         // may be NULL
 *                            timeout_ms,        // uint32_t
 *                            client_handle_out)
 *   qmi_client_release(client_handle)
 *
 * Return codes: 0 = OK, anything else is a QMI_* error (timeout, no service,
 * version mismatch, …). The actual numeric space is in qmi_client_error.h;
 * we just print the int and the caller can look it up.
 */
typedef int (*qmi_client_init_instance_fn)(
    void *service_obj,
    unsigned int instance_id,
    void *ind_cb,
    void *ind_cb_data,
    void *os_params,
    unsigned int timeout_ms,
    void **client_handle);

typedef int (*qmi_client_release_fn)(void *client_handle);

typedef int (*qmi_client_send_msg_sync_fn)(
    void *client_handle,
    unsigned int msg_id,
    void *req_c_struct,
    unsigned int req_c_struct_len,
    void *resp_c_struct,
    unsigned int resp_c_struct_len,
    unsigned int timeout_msecs);

/* qmi_client_send_raw_msg_sync — exported from libqmi_cci.so on X70.
 * Takes raw TLV bytes for req + resp (no c-struct marshalling), which
 * lets us call ANY msg_id without having the IDL headers. Signature
 * stable across all MSM CCI builds. */
typedef int (*qmi_client_send_raw_msg_sync_fn)(
    void *client_handle,
    unsigned int msg_id,
    void *req_buf,
    unsigned int req_buf_len,
    void *resp_buf,
    unsigned int resp_buf_len,
    unsigned int *resp_len_out,
    unsigned int timeout_msecs);

/* Parse a QMI response payload as a stream of TLVs. Format is
 * [type:u8][length:u16 LE][value:length]. Returns number of TLVs found. */
static int parse_qmi_tlvs(const char *tag, const unsigned char *buf, size_t n) {
    size_t p = 0;
    int tlv_count = 0;
    while (p + 3 <= n) {
        uint8_t  t  = buf[p];
        uint16_t l  = (uint16_t)(buf[p + 1] | (buf[p + 2] << 8));
        if (p + 3 + l > n) {
            HOUT("   TLV[%s] @ +%zu type=0x%02X len=%u TRUNCATED\n",
                 tag, p, t, l);
            break;
        }
        /* Log first 32 value bytes as hex. */
        char vhex[3 * 32 + 1];
        int vp = 0;
        size_t vcap = (l > 32) ? 32 : l;
        for (size_t i = 0; i < vcap; i++) {
            vp += snprintf(vhex + vp, sizeof(vhex) - vp, "%02X ", buf[p + 3 + i]);
        }
        vhex[vp] = 0;
        const char *kind = (t == 0x02) ? "RESULT" : "";
        HOUT("   TLV[%s] @ +%zu type=0x%02X len=%3u %s : %s%s\n",
             tag, p, t, l, kind, vhex, (l > 32) ? "..." : "");
        /* Special decode for the mandatory RESULT TLV (0x02, always 4 bytes:
         * result_code:u16 LE, err_code:u16 LE). Present in every response. */
        if (t == 0x02 && l == 4) {
            uint16_t rc = (uint16_t)(buf[p + 3] | (buf[p + 4] << 8));
            uint16_t ec = (uint16_t)(buf[p + 5] | (buf[p + 6] << 8));
            HOUT("     result_code=%u  err_code=%u\n", rc, ec);
        }
        p += 3 + l;
        tlv_count++;
    }
    return tlv_count;
}

static void hex_dump_line(const char *tag, const unsigned char *buf, size_t n) {
    char out[512];
    int pos = 0;
    size_t cap = (n > 96) ? 96 : n;
    for (size_t i = 0; i < cap && pos < (int)(sizeof(out) - 4); i++) {
        pos += snprintf(out + pos, sizeof(out) - pos, "%02X ", buf[i]);
    }
    HOUT("%s (%zu bytes): %s%s\n", tag, n, out, (n > cap) ? "..." : "");
}

static void *lookup_service_obj(const char *accessor_sym, const char *data_sym) {
    /* Prefer calling the *_get_service_object_internal_v01() accessor when
     * present — matches how libqmi_client_qmux uses these IDLs. Fall back
     * to the raw data symbol (stable on X70) if the accessor doesn't exist. */
    void (*acc)(void) = (void (*)(void)) dlsym(RTLD_DEFAULT, accessor_sym);
    if (acc) {
        typedef void *(*acc_fn)(void);
        void *obj = ((acc_fn)(void *)acc)();
        if (obj) {
            HOUT("CCI accessor %s -> %p\n", accessor_sym, obj);
            return obj;
        }
        HOUT("CCI accessor %s returned NULL\n", accessor_sym);
    }
    void *obj = dlsym(RTLD_DEFAULT, data_sym);
    if (obj) {
        HOUT("CCI data      %s -> %p\n", data_sym, obj);
        return obj;
    }
    HOUT("CCI service obj for %s / %s NOT FOUND\n", accessor_sym, data_sym);
    return NULL;
}

static void try_cci_ping(const char *tag,
                         const char *accessor_sym,
                         const char *data_sym) {
    void *service_obj = lookup_service_obj(accessor_sym, data_sym);
    if (!service_obj) return;

    qmi_client_init_instance_fn init_fn =
        (qmi_client_init_instance_fn) dlsym(RTLD_DEFAULT, "qmi_client_init_instance");
    qmi_client_release_fn release_fn =
        (qmi_client_release_fn) dlsym(RTLD_DEFAULT, "qmi_client_release");

    if (!init_fn || !release_fn) {
        HOUT("CCI %-6s SKIP: init=%p release=%p\n", tag, (void *)init_fn, (void *)release_fn);
        return;
    }

    void *handle = NULL;
    /* 5-second timeout — X70 NAS normally answers in <100ms. */
    int rc = init_fn(service_obj,
                     /* instance_id = */ 0,
                     /* ind_cb      */ NULL,
                     /* ind_data    */ NULL,
                     /* os_params   */ NULL,
                     /* timeout_ms  */ 5000,
                     &handle);
    HOUT("CCI %-6s init rc=%d handle=%p\n", tag, rc, handle);

    if (rc == 0 && handle) {
        int rr = release_fn(handle);
        HOUT("CCI %-6s release rc=%d\n", tag, rr);
    }
}

/* ---------- Phase 9: CCI full round-trip probe ----------
 *
 * Now that Phase 7 proved qmi_client_init_instance returns rc=0, we go
 * one layer deeper: open a real client, call qmi_client_send_msg_sync()
 * against a shortlist of candidate msg_ids, and log the return codes +
 * response bytes. The c-struct API in CCI encodes via the service
 * object's IDL table, so for messages that have an *empty* request
 * body (no mandatory TLVs) we can pass (NULL, 0) for the request and a
 * zeroed response buffer for CCI to fill in — even if we can't parse
 * the c-struct layout without headers, we still learn:
 *
 *   rc == 0                    msg exists, modem executed it OK
 *   rc == QMI_IDL_LIB_NO_ERR   (0) same as above
 *   rc == QMI_SERVICE_ERR (-4) CCI couldn't find msg_id in service IDL
 *   rc == -22 / -95            vendor rejected (unsupported / permission)
 *   rc == QMI_TIMEOUT_ERR      no answer in 5s
 *
 * From the delta we can pick the right lock message on X70 without the
 * proprietary headers.
 *
 * Candidates we probe (all have empty/optional request bodies on stock
 * Qualcomm, so passing (NULL,0) won't malform anything):
 *   NAS   0x0020 GET_SIGNAL_STRENGTH_REQ   — known-good, just pings
 *   NAS   0x004D GET_SYS_INFO_REQ          — known-good
 *   NAS   0x0080 RF_BAND_INFO_REQ          — band inventory
 *   NAS   0x004A GET_OPERATOR_NAME_DATA    — safe, PLMN query
 *   NAS   0x0043 GET_SYSTEM_SELECTION_PREFERENCE — read current sys_sel
 *   NASEXT vendor probes 0x55E0..0x55FF on msg_id stride — brute-force
 *          the vendor opcode range that historically carries the MSM
 *          engineering-mode PCI/EARFCN lock commands. Empty body.
 *   DMS   0x0025 GET_DEVICE_SERIAL_NUMBERS — sanity.
 */
typedef struct {
    unsigned int msg_id;
    const char  *name;
} cci_probe_msg_t;

static const cci_probe_msg_t g_nas_probes[] = {
    { 0x0020, "NAS_GET_SIGNAL_STRENGTH_REQ" },
    { 0x004D, "NAS_GET_SYS_INFO_REQ" },
    { 0x0080, "NAS_RF_BAND_INFO_REQ" },
    { 0x004A, "NAS_GET_OPERATOR_NAME_REQ" },
    { 0x0043, "NAS_GET_SYS_SELECT_PREF_REQ" },
    { 0x0099, "NAS_GET_LTE_CPHY_CA_INFO_REQ" },
    { 0x0074, "NAS_GET_RF_BAND_INFO_REQ_alt" },
    { 0,      NULL },
};

static const cci_probe_msg_t g_dms_probes[] = {
    { 0x0025, "DMS_GET_DEVICE_SERIAL_NUMBERS_REQ" },
    { 0x0020, "DMS_GET_DEVICE_MODEL_ID_REQ" },
    { 0,      NULL },
};

/* Vendor NAS_EXT range: iterate a conservative stride that historically
 * held the lock/rf-engineering commands on MSM NAS_EXT IDLs. We DON'T
 * probe 0x00..0x1F (collides with standard NAS) or values we'd consider
 * dangerous. */
static const unsigned int g_nasext_range_starts[] = {
    0x0050, 0x0060, 0x0070, 0x0080, 0x0090, 0x00A0, 0x00B0, 0x00C0,
    0x55E0, 0x55F0, 0x56A0, 0x56B0,
    0
};

static void probe_send(void *handle, const char *service_tag,
                       qmi_client_send_msg_sync_fn send_fn,
                       unsigned int msg_id, const char *name) {
    unsigned char resp[2048];
    memset(resp, 0, sizeof(resp));
    int rc = send_fn(handle, msg_id,
                     /* req */ NULL, 0,
                     /* resp */ resp, sizeof(resp),
                     /* timeout_ms */ 3000);
    /* Try to count how many leading bytes look non-zero (CCI writes the
     * c-struct in-place; even for unknown layouts this gives us a "did
     * we get real payload?" signal). */
    size_t nonzero = 0;
    for (size_t i = 0; i < sizeof(resp); i++) if (resp[i]) nonzero++;
    HOUT("SEND %-6s 0x%04X %-34s rc=%4d nz=%zu\n",
         service_tag, msg_id, name ? name : "(vendor)", rc, nonzero);
    if (rc == 0 && nonzero > 0) {
        hex_dump_line("   RESP", resp, nonzero < 128 ? nonzero : 128);
    }
}

/* Raw-msg send: uses qmi_client_send_raw_msg_sync which accepts raw
 * TLV bytes for both req and resp, bypassing the c-struct IDL layer.
 * This is the primary path on X70 now that Phase 11a confirmed the
 * symbol is exported. */
static void probe_raw_send(void *handle, const char *tag,
                           qmi_client_send_raw_msg_sync_fn raw_fn,
                           unsigned int msg_id,
                           const unsigned char *req, unsigned int req_len,
                           const char *name) {
    unsigned char resp[2048];
    memset(resp, 0, sizeof(resp));
    unsigned int resp_len = 0;
    int rc = raw_fn(handle, msg_id,
                    (void *)req, req_len,
                    resp, sizeof(resp),
                    &resp_len,
                    3000);
    HOUT("RAW %-6s 0x%04X %-36s rc=%4d resp_len=%u\n",
         tag, msg_id, name ? name : "(probe)", rc, resp_len);
    if (rc == 0 && resp_len > 0) {
        hex_dump_line("   BYTES", resp, resp_len);
        parse_qmi_tlvs(tag, resp, resp_len);
    }
}

/* Deep-probe the 7 known NAS_EXT opcodes with raw-send. Logs every
 * TLV returned — gives us the shape of each message so we can tell
 * GET (returns data TLVs) from SET (returns only result TLV) from
 * CLEAR (result only, no data). */
static const unsigned int g_nasext_known_opcodes[] = {
    0x007A, 0x0087, 0x00A1, 0x00A3, 0x00A4, 0x00DD, 0x00E2, 0
};

static void sweep_raw_all_accepted(void *handle, const char *tag,
                                    qmi_client_send_raw_msg_sync_fn raw_fn,
                                    unsigned int lo, unsigned int hi) {
    for (unsigned int id = lo; id <= hi; id++) {
        unsigned char resp[2048];
        memset(resp, 0, sizeof(resp));
        unsigned int resp_len = 0;
        int rc = raw_fn(handle, id, NULL, 0, resp, sizeof(resp),
                        &resp_len, 500);
        if (rc == -43) continue;           /* not in IDL, silent */
        if (rc == 0 && resp_len >= 7) {
            /* Real TLV response — parse it. */
            HOUT("RAW %-6s 0x%04X rc=0 resp_len=%u\n", tag, id, resp_len);
            parse_qmi_tlvs(tag, resp, resp_len);
        } else if (rc == 0) {
            HOUT("RAW %-6s 0x%04X rc=0 resp_len=%u (no TLV body)\n",
                 tag, id, resp_len);
        } else {
            HOUT("RAW %-6s 0x%04X rc=%d resp_len=%u (needs req TLV)\n",
                 tag, id, rc, resp_len);
        }
    }
}

/* Full-range sweep: probe every msg_id in [lo, hi] on the given client.
 *
 * Phase 10 only logged rc==0 results, which hid msgs that DO exist in the
 * IDL but need mandatory TLVs we didn't send (e.g. 0x0024 GET_SERVING_SYSTEM,
 * 0x004D GET_SYS_INFO when called without its mandatory mode TLV). Now we
 * bucket every rc:
 *   rc ==   0   accepted, modem executed it
 *   rc == -43   IDL_LIB_MSG_ID_NOT_FOUND — opcode absent from this service
 *   rc == other implemented, but our empty request is malformed
 *
 * Only "not in IDL" rejects are boring; everything else is a real hit. */
static void sweep_msg_ids(void *handle, const char *tag,
                          qmi_client_send_msg_sync_fn send_fn,
                          unsigned int lo, unsigned int hi) {
    int accepted = 0, needs_tlv = 0, total = 0, nf = 0;
    for (unsigned int id = lo; id <= hi; id++) {
        unsigned char resp[512];
        memset(resp, 0, sizeof(resp));
        int rc = send_fn(handle, id, NULL, 0, resp, sizeof(resp), 600);
        total++;
        if (rc == -43) { nf++; continue; }
        if (rc == 0) {
            accepted++;
            size_t nz = 0;
            for (size_t i = 0; i < sizeof(resp); i++) if (resp[i]) nz++;
            HOUT("SWEEP %-6s 0x%04X rc=0 nz=%zu\n", tag, id, nz);
            if (nz > 2) hex_dump_line("   PAYLOAD", resp, nz < 128 ? nz : 128);
        } else {
            needs_tlv++;
            HOUT("SWEEP %-6s 0x%04X rc=%d (impl, needs TLV)\n", tag, id, rc);
        }
    }
    HOUT("SWEEP %-6s total=%d accepted=%d needs_tlv=%d not_impl=%d\n",
         tag, total, accepted, needs_tlv, nf);
}

/* ---------- Phase 11/13: walk qmi_idl_service_object_s_t struct ----------
 *
 * Phase 11 guessed the layout wrong. Phase 12's 128-byte hex dump of
 * the real NAS / NAS_EXT / DMS service objects on X70/HyperOS pinned
 * the actual layout:
 *
 *   struct qmi_idl_service_object_s_t {          // NAS    NASEXT  DMS
 *       uint32_t library_version;   // +0x00        6       6       6
 *       uint32_t idl_version;       // +0x04        1       1       1
 *       uint32_t service_id;        // +0x08        3       3       2
 *       uint32_t max_msg_len;       // +0x0C    0x8F23   0x0028  0x3017
 *       uint16_t num_req;           // +0x10    0x00E2   0x000D  0x0051
 *       uint16_t num_resp;          // +0x12    0x00E2   0x000D  0x0051
 *       uint32_t num_ind;           // +0x14    0x51     0x0C    0x0C
 *       const qmi_idl_msg_entry_t *req;   // +0x18 (u64 ptr)
 *       const qmi_idl_msg_entry_t *resp;  // +0x20 (u64 ptr)
 *       const qmi_idl_msg_entry_t *ind;   // +0x28 (u64 ptr)
 *       const void *ranges_tbl;           // +0x30 (u64 ptr)
 *       uint64_t   total_msg_len;         // +0x38 (u64)
 *       ...
 *   };
 *
 * Each message entry is 16 bytes on 64-bit:
 *   struct qmi_idl_message_entry {
 *       uint16_t msg_id;
 *       uint16_t flags;       // often 0
 *       uint32_t max_size;    // c-struct size for this msg
 *       const void *tlv_desc; // ptr to TLV descriptor array
 *   };
 *
 * With max_size for every msg_id we can finally send valid requests:
 * pass a zeroed buffer of exactly that many bytes as the c_struct.
 * The encoder reads only optional-field-present flags at fixed offsets
 * (all zero → "not present") and emits a header-only TLV stream.
 *
 * For /vendor/lib64/libqmi_encdec.so the layout is identical, because
 * the same libqmi_encdec defines the accessor for both libqmiservices
 * and libqmiextservices.
 *
 * We're conservative: dump only if the pointers look sane (inside
 * readable memory) and abort the walk on the first suspicious entry.
 */
#define IDL_DUMP_MAX 256

/* Phase 14: proven entry size = 6 bytes (not 16).
 * NAS:    req_ptr=0x..A61C, resp_ptr=0x..AB68; distance 0x54C = 1356 bytes
 *         ÷ 226 entries = 6 bytes each.
 * NASEXT: distance 0x4E = 78 bytes ÷ 13 entries = 6 bytes each.
 *
 * Likely layout: { msg_id:u16, max_size:u16, desc_idx:u16 } — the TLV
 * descriptor isn't a raw pointer (doesn't fit in 6 bytes) but an index
 * into a separate descriptor blob referenced at service_object +0x30. */
typedef struct __attribute__((packed)) {
    uint16_t msg_id;
    uint16_t max_size;
    uint16_t desc_idx;
} qmi_idl_msg_entry_t;

static int is_probably_readable(const void *p) {
    if (!p) return 0;
    /* Read one byte through /proc/self/pagemap would be ideal; here we
     * just check alignment and a sensible upper bound. Fault reads are
     * caught by signal handlers we don't install — so only call this on
     * pointers we at least BELIEVE are inside a mapped .rodata. */
    uintptr_t u = (uintptr_t)p;
    if (u < 0x1000) return 0;              /* NULL / low pages */
    if (u & 0x1) return 0;                 /* must be aligned */
    if (u > 0x7FFFFFFFFFFFULL) return 0;   /* userspace cap */
    return 1;
}

static void dump_service_object_table(const char *tag, const void *sobj) {
    if (!is_probably_readable(sobj)) {
        HOUT("IDL %-6s service_object not readable %p\n", tag, sobj);
        return;
    }
    const uint8_t *p = (const uint8_t *)sobj;
    uint32_t library_version = *(const uint32_t *)(p + 0x00);
    uint32_t idl_version     = *(const uint32_t *)(p + 0x04);
    uint32_t service_id      = *(const uint32_t *)(p + 0x08);
    uint32_t max_msg_len     = *(const uint32_t *)(p + 0x0C);
    uint16_t num_req         = *(const uint16_t *)(p + 0x10);
    uint16_t num_resp        = *(const uint16_t *)(p + 0x12);
    uint32_t num_ind         = *(const uint32_t *)(p + 0x14);
    const qmi_idl_msg_entry_t *req_tbl  =
        *(const qmi_idl_msg_entry_t **)(p + 0x18);
    const qmi_idl_msg_entry_t *resp_tbl =
        *(const qmi_idl_msg_entry_t **)(p + 0x20);
    const qmi_idl_msg_entry_t *ind_tbl  =
        *(const qmi_idl_msg_entry_t **)(p + 0x28);

    HOUT("IDL %-6s libver=0x%08x idlver=0x%08x service_id=0x%x max_msg_len=%u\n",
         tag, library_version, idl_version, service_id, max_msg_len);
    HOUT("IDL %-6s num_req=%u num_resp=%u num_ind=%u\n",
         tag, num_req, num_resp, num_ind);
    HOUT("IDL %-6s tables req=%p resp=%p ind=%p\n",
         tag, (const void *)req_tbl, (const void *)resp_tbl, (const void *)ind_tbl);

    /* Sanity-check ranges. Legit IDL tables for NAS have ~200 entries. */
    if (num_req > 2048 || num_resp > 2048 || num_ind > 2048) {
        HOUT("IDL %-6s sanity FAIL — struct offsets might differ on this stack\n", tag);
        return;
    }

    if (is_probably_readable(req_tbl) && num_req > 0) {
        uint32_t n = num_req > IDL_DUMP_MAX ? IDL_DUMP_MAX : num_req;
        HOUT("IDL %-6s REQ table (%u entries, 6B each):\n", tag, n);
        for (uint32_t i = 0; i < n; i++) {
            HOUT("  REQ  0x%04X max_size=%u desc_idx=0x%04X\n",
                 req_tbl[i].msg_id, req_tbl[i].max_size, req_tbl[i].desc_idx);
        }
    }
    if (is_probably_readable(resp_tbl) && num_resp > 0) {
        uint32_t n = num_resp > IDL_DUMP_MAX ? IDL_DUMP_MAX : num_resp;
        HOUT("IDL %-6s RESP table (%u entries, 6B each):\n", tag, n);
        for (uint32_t i = 0; i < n; i++) {
            HOUT("  RESP 0x%04X max_size=%u desc_idx=0x%04X\n",
                 resp_tbl[i].msg_id, resp_tbl[i].max_size, resp_tbl[i].desc_idx);
        }
    }
    if (is_probably_readable(ind_tbl) && num_ind > 0) {
        uint32_t n = num_ind > IDL_DUMP_MAX ? IDL_DUMP_MAX : num_ind;
        HOUT("IDL %-6s IND  table (%u entries, 6B each):\n", tag, n);
        for (uint32_t i = 0; i < n; i++) {
            HOUT("  IND  0x%04X max_size=%u desc_idx=0x%04X\n",
                 ind_tbl[i].msg_id, ind_tbl[i].max_size, ind_tbl[i].desc_idx);
        }
    }
}

/* Extra CCI/IDL symbols we probe for — lets us encode+decode arbitrary
 * TLV-formatted requests/responses without having the IDL headers. */
static const char *g_raw_cci_syms[] = {
    "qmi_client_send_raw_msg_async",
    "qmi_client_send_raw_msg_sync",
    "qmi_idl_message_encode",
    "qmi_idl_message_decode",
    "qmi_idl_get_max_service_len",
    "qmi_idl_service_message_db_set_ranges",
    "qmi_client_message_db_get_decode_ranges",
    "qmi_client_message_db_get_encode_ranges",
    "qmi_idl_tlv_encode",
    "qmi_idl_tlv_decode",
    NULL,
};

static void try_cci_full_rt(const char *tag,
                            const char *accessor_sym,
                            const char *data_sym,
                            const cci_probe_msg_t *probes,
                            int scan_vendor_range) {
    void *service_obj = lookup_service_obj(accessor_sym, data_sym);
    if (!service_obj) return;

    qmi_client_init_instance_fn init_fn =
        (qmi_client_init_instance_fn) dlsym(RTLD_DEFAULT, "qmi_client_init_instance");
    qmi_client_release_fn release_fn =
        (qmi_client_release_fn) dlsym(RTLD_DEFAULT, "qmi_client_release");
    qmi_client_send_msg_sync_fn send_fn =
        (qmi_client_send_msg_sync_fn) dlsym(RTLD_DEFAULT, "qmi_client_send_msg_sync");

    if (!init_fn || !release_fn || !send_fn) {
        HOUT("SEND %-6s SKIP: init=%p release=%p send=%p\n",
             tag, (void *)init_fn, (void *)release_fn, (void *)send_fn);
        return;
    }

    void *handle = NULL;
    int rc = init_fn(service_obj, 0, NULL, NULL, NULL, 5000, &handle);
    if (rc != 0 || !handle) {
        HOUT("SEND %-6s init FAIL rc=%d\n", tag, rc);
        return;
    }
    HOUT("SEND %-6s init OK handle=%p — probing messages...\n", tag, handle);

    if (probes) {
        for (int i = 0; probes[i].name; i++) {
            probe_send(handle, tag, send_fn, probes[i].msg_id, probes[i].name);
        }
    }
    if (scan_vendor_range) {
        /* Full sweep over the vendor msg_id space — covers everything
         * the IDL table knows about. Local -43 rejects return instantly,
         * only true accepts take a modem round-trip, so this stays
         * fast (usually <2s for 512 ids).  */
        sweep_msg_ids(handle, tag, send_fn, 0x0001, 0x01FF);
    }
    release_fn(handle);
    HOUT("SEND %-6s done, released\n", tag);
}

/* Raw hex dump of the first [n] bytes at [sobj] — lets the analyst
 * manually find the real qmi_idl_service_object_s_t offsets for this
 * stack (Phase 11 revealed our inferred layout is off). */
static void dump_service_object_bytes(const char *tag, const void *sobj, size_t n) {
    if (!is_probably_readable(sobj)) {
        HOUT("SOBJ %-6s %p not readable\n", tag, sobj);
        return;
    }
    if (sigsetjmp(g_fault_jmp, 1) == 0) {
        g_fault_armed = 1;
        const unsigned char *p = (const unsigned char *)sobj;
        /* 16-byte rows for readability. */
        for (size_t off = 0; off < n; off += 16) {
            char row[128];
            int rp = 0;
            rp += snprintf(row + rp, sizeof(row) - rp, "SOBJ %-6s +0x%02zx:",
                           tag, off);
            for (size_t i = 0; i < 16 && off + i < n; i++) {
                rp += snprintf(row + rp, sizeof(row) - rp, " %02X", p[off + i]);
            }
            HOUT("%s\n", row);
        }
        g_fault_armed = 0;
    } else {
        HOUT("SOBJ %-6s fault during dump\n", tag);
    }
}

/* Also try calling qmi_idl_get_max_service_len(service_obj) directly —
 * exported from libqmi_cci.so, returns the max msg len for the service.
 * A plausible value (~2K..64K) confirms the service object pointer is
 * valid; this + the hex dump together pins down the struct layout. */
typedef unsigned int (*qmi_idl_get_max_service_len_fn)(const void *sobj);
static void call_max_service_len(const char *tag, const void *sobj) {
    qmi_idl_get_max_service_len_fn fn =
        (qmi_idl_get_max_service_len_fn) dlsym(RTLD_DEFAULT,
                                               "qmi_idl_get_max_service_len");
    if (!fn) { HOUT("MAXLEN %-6s NO_SYM\n", tag); return; }
    if (!is_probably_readable(sobj)) {
        HOUT("MAXLEN %-6s sobj not readable\n", tag); return;
    }
    if (sigsetjmp(g_fault_jmp, 1) == 0) {
        g_fault_armed = 1;
        unsigned int v = fn(sobj);
        g_fault_armed = 0;
        HOUT("MAXLEN %-6s = %u (0x%x)\n", tag, v, v);
    } else {
        HOUT("MAXLEN %-6s fault\n", tag);
    }
}

/* Safely dump the service object's message table with a SEGV-handler
 * guard — if our inferred struct offsets are wrong for this build of
 * libqmi_encdec, we abort the dump instead of crashing the helper. */
static void dump_service_object_guarded(const char *tag, const void *sobj) {
    if (sigsetjmp(g_fault_jmp, 1) == 0) {
        g_fault_armed = 1;
        dump_service_object_table(tag, sobj);
        g_fault_armed = 0;
    } else {
        HOUT("IDL %-6s SIGSEGV — struct offsets differ on this stack, aborting dump\n", tag);
    }
}

/* ---------- Phase 8: .rodata string scan ----------
 *
 * Walk the given file's ELF .rodata-ish sections (any PROGBITS with ALLOC
 * and no EXEC) and emit every printable ASCII run (>= MIN_LEN chars) whose
 * contents match a lock/band/cell/rf interest substring. This surfaces
 * debug/trace strings like "NAS_LOCK_CELL_REQ" / "nas_pci_lock_req" that
 * let us pick the right msg_id without having the IDL headers.
 */
#define RODATA_MIN_LEN 6
#define RODATA_MAX_HITS 400
static const char *g_rodata_needle[] = {
    "LOCK", "lock", "Lock",
    "PCI",  "pci",
    "EARFCN", "earfcn",
    "CELL", "cell",
    "BAND", "band",
    "SYS_SEL", "sys_sel", "selection_pref",
    "REJECT", "reject",
    "RF_",  "rf_",
    "BAR",  "bar_",
    "FREQ", "freq",
    NULL,
};

static int rodata_interesting(const char *s, size_t len) {
    if (len < RODATA_MIN_LEN) return 0;
    for (int i = 0; g_rodata_needle[i]; i++) {
        if (strstr(s, g_rodata_needle[i])) return 1;
    }
    return 0;
}

static void dump_rodata_strings(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { HOUT("RODATA OPEN_FAIL %s\n", path); return; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        HOUT("RODATA STAT_FAIL %s\n", path); close(fd); return;
    }
    size_t fsize = (size_t)st.st_size;
    void *map = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { HOUT("RODATA MMAP_FAIL %s\n", path); return; }
    const unsigned char *base = (const unsigned char *)map;
    if (memcmp(base, ELFMAG, SELFMAG) != 0 || base[EI_CLASS] != ELFCLASS64) {
        HOUT("RODATA BAD_MAGIC %s\n", path);
        munmap(map, fsize); return;
    }
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    const Elf64_Shdr *shtab = (const Elf64_Shdr *)(base + eh->e_shoff);

    int hits = 0;
    for (unsigned i = 0; i < eh->e_shnum && hits < RODATA_MAX_HITS; i++) {
        const Elf64_Shdr *sh = &shtab[i];
        if (sh->sh_type != SHT_PROGBITS) continue;
        if (!(sh->sh_flags & SHF_ALLOC)) continue;
        if (sh->sh_flags & SHF_EXECINSTR) continue;
        if (sh->sh_offset + sh->sh_size > fsize) continue;
        const unsigned char *data = base + sh->sh_offset;
        size_t sz = sh->sh_size;
        /* Walk NUL-terminated ASCII runs. */
        size_t k = 0;
        while (k < sz && hits < RODATA_MAX_HITS) {
            /* Find start of printable run. */
            while (k < sz && (data[k] < 0x20 || data[k] > 0x7E)) k++;
            size_t start = k;
            while (k < sz && data[k] >= 0x20 && data[k] <= 0x7E) k++;
            if (k <= start) { k++; continue; }
            size_t len = k - start;
            if (len >= RODATA_MIN_LEN && len < 256) {
                /* copy to NUL-terminated local buf to run strstr */
                char buf[256];
                memcpy(buf, data + start, len);
                buf[len] = 0;
                if (rodata_interesting(buf, len)) {
                    HOUT("RODATA %s: %s\n", path, buf);
                    hits++;
                }
            }
        }
    }
    if (hits == 0) HOUT("RODATA %s NONE\n", path);
    munmap(map, fsize);
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

    /* -------- PHASE 6: ELF dynsym dump -------- */
    HOUT("---- PHASE 6: ELF dynsym dump (nas/dms/wds/lock/band/service_object/sys_sel/pci) ----\n");
    for (int i = 0; i < g_lib_count; i++) {
        dump_elf_symbols(g_libs[i].path);
    }

    /* -------- PHASE 7: live CCI init/release for NAS + NAS_EXT -------- */
    HOUT("---- PHASE 7: live CCI init_instance ----\n");
    try_cci_ping("NAS",
                 "nas_get_service_object_internal_v01",
                 "nas_qmi_idl_service_object_v01");
    try_cci_ping("NASEXT",
                 "nas_ext_get_service_object_internal_v01",
                 "nas_ext_qmi_idl_service_object_v01");
    try_cci_ping("DMS",
                 "dms_get_service_object_internal_v01",
                 "dms_qmi_idl_service_object_v01");

    /* -------- PHASE 11a: extra CCI/IDL raw-send symbol probe -------- */
    HOUT("---- PHASE 11a: raw-send / encoder API probe ----\n");
    for (int s = 0; g_raw_cci_syms[s]; s++) {
        probe_sym(g_raw_cci_syms[s], 0);
    }

    /* -------- PHASE 11b: walk qmi_idl_service_object_s_t struct for NAS + NAS_EXT + DMS -------- */
    HOUT("---- PHASE 11b: IDL service-object table dump ----\n");
    install_fault_handler();
    void *sobj_nas    = lookup_service_obj("nas_get_service_object_internal_v01",
                                           "nas_qmi_idl_service_object_v01");
    void *sobj_nasext = lookup_service_obj("nas_ext_get_service_object_internal_v01",
                                           "nas_ext_qmi_idl_service_object_v01");
    void *sobj_dms    = lookup_service_obj("dms_get_service_object_internal_v01",
                                           "dms_qmi_idl_service_object_v01");
    if (sobj_nas)    dump_service_object_guarded("NAS",    sobj_nas);
    if (sobj_nasext) dump_service_object_guarded("NASEXT", sobj_nasext);
    if (sobj_dms)    dump_service_object_guarded("DMS",    sobj_dms);

    /* -------- PHASE 12a: raw hex dump of service-object first 128 bytes (kept for paranoia) -------- */
    HOUT("---- PHASE 12a: service-object hex dump ----\n");
    if (sobj_nas)    dump_service_object_bytes("NAS",    sobj_nas,    128);
    if (sobj_nasext) dump_service_object_bytes("NASEXT", sobj_nasext, 128);
    if (sobj_dms)    dump_service_object_bytes("DMS",    sobj_dms,    128);

    /* -------- PHASE 12b: raw-msg send via qmi_client_send_raw_msg_sync -------- */
    HOUT("---- PHASE 12b: raw-send (TLV-layer) probe ----\n");
    qmi_client_send_raw_msg_sync_fn raw_fn =
        (qmi_client_send_raw_msg_sync_fn) dlsym(RTLD_DEFAULT,
                                                "qmi_client_send_raw_msg_sync");
    qmi_client_init_instance_fn   init_fn    =
        (qmi_client_init_instance_fn)   dlsym(RTLD_DEFAULT, "qmi_client_init_instance");
    qmi_client_release_fn         release_fn =
        (qmi_client_release_fn)         dlsym(RTLD_DEFAULT, "qmi_client_release");

    if (raw_fn && init_fn && release_fn) {
        /* Open a NAS client, deep-probe every accepted opcode (raw). */
        if (sobj_nas) {
            void *h = NULL;
            int rc = init_fn(sobj_nas, 0, NULL, NULL, NULL, 5000, &h);
            HOUT("RAW NAS    init rc=%d handle=%p\n", rc, h);
            if (rc == 0 && h) {
                sweep_raw_all_accepted(h, "NAS", raw_fn, 0x0001, 0x01FF);
                release_fn(h);
            }
        }
        /* NAS_EXT: the 7 known opcodes plus full sweep for any we missed. */
        if (sobj_nasext) {
            void *h = NULL;
            int rc = init_fn(sobj_nasext, 0, NULL, NULL, NULL, 5000, &h);
            HOUT("RAW NASEXT init rc=%d handle=%p\n", rc, h);
            if (rc == 0 && h) {
                HOUT("--- NASEXT known-opcode deep probe ---\n");
                for (int i = 0; g_nasext_known_opcodes[i]; i++) {
                    probe_raw_send(h, "NASEXT", raw_fn,
                                   g_nasext_known_opcodes[i], NULL, 0, NULL);
                }

                /* Phase 13c: send speculative TLV payloads to the 5 NASEXT
                 * opcodes that rejected empty body with "needs TLV".
                 *
                 * We try common TLV layouts that vendor lock requests use:
                 *   A) TLV 0x01 len=4 value=00*4   — single u32 param
                 *   B) TLV 0x01 len=8 value=00*8   — pair of u32 (EARFCN+PCI)
                 *   C) TLV 0x01 len=1 value=00     — single byte enable/mode
                 *
                 * If any of A/B/C turns INVALID_ARG (err=1) into SUCCESS
                 * (err=0) or into a different error like NO_ENTRY_FOUND
                 * (err=15) — we've found the expected payload shape. */
                static const unsigned int targets[] = { 0x007A, 0x00A1, 0x00A4, 0x00DD, 0x00E2, 0 };
                static const unsigned char tlv_u32_zero[]    = { 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
                static const unsigned char tlv_u32u32_zero[] = { 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
                static const unsigned char tlv_u8_zero[]     = { 0x01, 0x01, 0x00, 0x00 };
                for (int i = 0; targets[i]; i++) {
                    HOUT("  spec-A 0x%04X TLV01 u32=0:\n", targets[i]);
                    probe_raw_send(h, "NASEXT", raw_fn, targets[i],
                                   tlv_u32_zero, sizeof(tlv_u32_zero), "spec_u32");
                    HOUT("  spec-B 0x%04X TLV01 u32+u32=0:\n", targets[i]);
                    probe_raw_send(h, "NASEXT", raw_fn, targets[i],
                                   tlv_u32u32_zero, sizeof(tlv_u32u32_zero), "spec_u32u32");
                    HOUT("  spec-C 0x%04X TLV01 u8=0:\n", targets[i]);
                    probe_raw_send(h, "NASEXT", raw_fn, targets[i],
                                   tlv_u8_zero, sizeof(tlv_u8_zero), "spec_u8");
                }

                HOUT("--- NASEXT full sweep ---\n");
                sweep_raw_all_accepted(h, "NASEXT", raw_fn, 0x0001, 0x01FF);
                release_fn(h);
            }
        }
    } else {
        HOUT("RAW: missing raw_fn=%p init_fn=%p release_fn=%p\n",
             (void *)raw_fn, (void *)init_fn, (void *)release_fn);
    }

    /* -------- PHASE 9: live CCI round-trip (init + send + parse rc) -------- */
    HOUT("---- PHASE 9: CCI full round-trip probe ----\n");
    try_cci_full_rt("NAS",
                    "nas_get_service_object_internal_v01",
                    "nas_qmi_idl_service_object_v01",
                    g_nas_probes,
                    /* scan_vendor_range = */ 1);
    try_cci_full_rt("NASEXT",
                    "nas_ext_get_service_object_internal_v01",
                    "nas_ext_qmi_idl_service_object_v01",
                    /* probes = */ NULL,
                    /* scan_vendor_range = */ 1);
    try_cci_full_rt("DMS",
                    "dms_get_service_object_internal_v01",
                    "dms_qmi_idl_service_object_v01",
                    g_dms_probes,
                    /* scan_vendor_range = */ 0);

    /* -------- PHASE 8: .rodata string scan on lock-adjacent libs -------- */
    HOUT("---- PHASE 8: .rodata string scan (LOCK/PCI/EARFCN/CELL/BAND/...) ----\n");
    static const char *rodata_targets[] = {
        "/vendor/lib64/libqmiservices.so",
        "/vendor/lib64/libqmiextservices.so",
        "/vendor/lib64/librilqmimiscservices.so",
        "/vendor/lib64/libqmi_legacy.so",
        "/vendor/lib64/libril.so",
        "/vendor/lib64/libril-qc-radioconfig.so",
        NULL,
    };
    for (int i = 0; rodata_targets[i]; i++) {
        struct stat st;
        if (stat(rodata_targets[i], &st) == 0) {
            dump_rodata_strings(rodata_targets[i]);
        } else {
            HOUT("RODATA %s MISSING\n", rodata_targets[i]);
        }
    }

    /* -------- Summary (machine-parsed by jni_bridge) -------- */
    HOUT("FOUND %d/%d\n", cci_hits + svc_hits, cci_total + (int)(sizeof(g_service_roots)/sizeof(*g_service_roots)) - 1);
    HOUT("MAIN OK (symbol scan done)\n");
    HOUT("MASK 0x%04X %d/%d\n", mask, cci_hits, cci_total);
    HOUT("=== qdiag_helper probe end ===\n");
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
}

/* ---------- Phase 14: CCI band-lock mode (standalone) ----------
 *
 * Usage: qdiag_helper lock <lte_low_hex> <lte_high_hex> <nr_low_hex> <nr_high_hex>
 *
 * Writes a short line to /data/local/tmp/qdiag_helper.log AND to stdout:
 *   LOCK_RESULT rc=<transport_rc> result_code=<qmi_result> err_code=<qmi_err>
 *
 * Exit code mirrors the parsed result_code (0 = success, 1 = QMI error)
 * so the Kotlin caller can check $?. On transport failure, rc != 0 and
 * exit code = 2.
 *
 * This is the MINIMAL CCI-based SET_SYSTEM_SELECTION_PREFERENCE path:
 *   1. dlopen the three libs we know hold NAS (libqmiservices +
 *      libqmi_cci + libqmi_encdec)
 *   2. lookup the nas_qmi_idl_service_object_v01 pointer
 *   3. qmi_client_init_instance -> get NAS handle
 *   4. craft 0x0033 request body with the same TLV layout qmi_nas.c uses
 *      for the DIAG path (TLV 0x11 legacy + 0x12 mode + 0x1C lte_ext +
 *      0x24 nr_sa + 0x25 nr_nsa)
 *   5. qmi_client_send_raw_msg_sync (proven-working on X70, Phase 12)
 *   6. parse response for result/err TLV, print line, release handle, exit.
 */

/* Hex TLV helper: [type:u8][len:u16 LE][value:len]. */
static size_t lock_put_tlv(uint8_t *buf, size_t off,
                           uint8_t type, const uint8_t *val, size_t len) {
    buf[off++] = type;
    buf[off++] = (uint8_t)(len & 0xFF);
    buf[off++] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(buf + off, val, len);
    return off + len;
}

static void put_u64_le(uint8_t *dst, uint64_t v) {
    for (int i = 0; i < 8; i++) dst[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

static int do_lock_bands(uint64_t lte_low, uint64_t lte_high,
                         uint64_t nr_low,  uint64_t nr_high) {
    g_log_fp = fopen("/data/local/tmp/qdiag_helper.log", "a");
    HOUT("=== qdiag_helper lock start ===\n");
    HOUT("LOCK ARGS lte=0x%016llx%016llx nr=0x%016llx%016llx\n",
         (unsigned long long)lte_high, (unsigned long long)lte_low,
         (unsigned long long)nr_high,  (unsigned long long)nr_low);

    install_fault_handler();

    /* Open enough of vendor lib set for NAS + CCI to resolve. */
    static const char *fixed_deps[] = {
        "/vendor/lib64/libqmi_cci.so",
        "/vendor/lib64/libqmi_encdec.so",
        "/vendor/lib64/libqmiservices.so",
        NULL
    };
    for (int i = 0; fixed_deps[i]; i++) {
        void *h = dlopen(fixed_deps[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            HOUT("LOCK dlopen FAIL %s: %s\n", fixed_deps[i], dlerror());
        } else {
            HOUT("LOCK dep OK %s\n", fixed_deps[i]);
        }
    }

    /* Find NAS service object. */
    void *sobj_nas = lookup_service_obj("nas_get_service_object_internal_v01",
                                        "nas_qmi_idl_service_object_v01");
    if (!sobj_nas) {
        HOUT("LOCK_RESULT rc=-1 result_code=0 err_code=0  (no sobj)\n");
        return 2;
    }

    qmi_client_init_instance_fn init_fn =
        (qmi_client_init_instance_fn) dlsym(RTLD_DEFAULT, "qmi_client_init_instance");
    qmi_client_release_fn release_fn =
        (qmi_client_release_fn) dlsym(RTLD_DEFAULT, "qmi_client_release");
    qmi_client_send_raw_msg_sync_fn raw_fn =
        (qmi_client_send_raw_msg_sync_fn) dlsym(RTLD_DEFAULT,
                                                "qmi_client_send_raw_msg_sync");
    if (!init_fn || !release_fn || !raw_fn) {
        HOUT("LOCK_RESULT rc=-2 result_code=0 err_code=0  (missing sym init=%p rel=%p raw=%p)\n",
             (void *)init_fn, (void *)release_fn, (void *)raw_fn);
        return 2;
    }

    void *h = NULL;
    int rc = init_fn(sobj_nas, 0, NULL, NULL, NULL, 5000, &h);
    if (rc != 0 || !h) {
        HOUT("LOCK_RESULT rc=%d result_code=0 err_code=0  (init failed)\n", rc);
        return 2;
    }
    HOUT("LOCK init OK handle=%p\n", h);

    /* Build TLV body. Same field order as qmi_nas.c. */
    uint8_t tlvs[256];
    size_t  o = 0;
    /* TLV 0x11 legacy band pref (u64 LE) — low 64 bits of LTE. */
    uint8_t legacy[8]; put_u64_le(legacy, lte_low);
    o = lock_put_tlv(tlvs, o, 0x11, legacy, sizeof(legacy));
    /* TLV 0x12 mode pref: allow all RATs so modem picks from the masks. */
    uint8_t mode[2] = { 0xFF, 0x00 };
    o = lock_put_tlv(tlvs, o, 0x12, mode, sizeof(mode));
    /* TLV 0x1C lte_band_pref_ext (u64 low + u64 high = 128 bits). */
    uint8_t lte_ext[16]; put_u64_le(lte_ext, lte_low); put_u64_le(lte_ext + 8, lte_high);
    o = lock_put_tlv(tlvs, o, 0x1C, lte_ext, sizeof(lte_ext));
    /* TLV 0x24 nr5g_sa_band_pref (128 bits). */
    uint8_t nr[16]; put_u64_le(nr, nr_low); put_u64_le(nr + 8, nr_high);
    o = lock_put_tlv(tlvs, o, 0x24, nr, sizeof(nr));
    /* TLV 0x25 nr5g_nsa_band_pref (128 bits, same mask). */
    o = lock_put_tlv(tlvs, o, 0x25, nr, sizeof(nr));

    HOUT("LOCK req %zu bytes: ", o);
    for (size_t i = 0; i < o && i < 128; i++) HOUT("%02X ", tlvs[i]);
    HOUT("\n");

    /* Send 0x0033 SET_SYSTEM_SELECTION_PREFERENCE. */
    uint8_t resp[2048];
    unsigned int resp_len = 0;
    int srv = raw_fn(h, 0x0033, tlvs, (unsigned int)o,
                     resp, sizeof(resp), &resp_len, 5000);
    HOUT("LOCK send rc=%d resp_len=%u\n", srv, resp_len);

    uint16_t rcode = 0xFFFF, ecode = 0xFFFF;
    if (srv == 0 && resp_len >= 7) {
        parse_qmi_tlvs("LOCK", resp, resp_len);
        /* Mandatory RESULT TLV is at +0: type=0x02 len=4 rc:u16 ec:u16. */
        if (resp[0] == 0x02 && resp[1] == 0x04) {
            rcode = (uint16_t)(resp[3] | (resp[4] << 8));
            ecode = (uint16_t)(resp[5] | (resp[6] << 8));
        }
    }

    release_fn(h);
    HOUT("LOCK_RESULT rc=%d result_code=%u err_code=%u\n", srv, rcode, ecode);
    HOUT("=== qdiag_helper lock end ===\n");
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
    return (srv == 0 && rcode == 0) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        probe();
        return 0;
    }
    if (argc >= 6 && strcmp(argv[1], "lock") == 0) {
        uint64_t ll = strtoull(argv[2], NULL, 0);
        uint64_t lh = strtoull(argv[3], NULL, 0);
        uint64_t nl = strtoull(argv[4], NULL, 0);
        uint64_t nh = strtoull(argv[5], NULL, 0);
        return do_lock_bands(ll, lh, nl, nh);
    }
    fprintf(stderr, "usage: %s probe\n"
                    "       %s lock <lte_low_hex> <lte_high_hex> <nr_low_hex> <nr_high_hex>\n",
            argv[0], argv[0]);
    return 2;
}
