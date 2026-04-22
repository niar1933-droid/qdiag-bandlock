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

/* Full-range sweep: probe every msg_id in [lo, hi] on the given client,
 * log only accepted ids (rc == 0) and dump response bytes for the ones
 * that returned payload > 2 bytes (tiny 2-byte responses are usually
 * just the success result TLV with no data). */
static void sweep_msg_ids(void *handle, const char *tag,
                          qmi_client_send_msg_sync_fn send_fn,
                          unsigned int lo, unsigned int hi) {
    int accepted = 0, total = 0;
    for (unsigned int id = lo; id <= hi; id++) {
        unsigned char resp[512];
        memset(resp, 0, sizeof(resp));
        int rc = send_fn(handle, id, NULL, 0, resp, sizeof(resp), 800);
        total++;
        if (rc != 0) continue;
        accepted++;
        size_t nz = 0;
        for (size_t i = 0; i < sizeof(resp); i++) if (resp[i]) nz++;
        HOUT("SWEEP %-6s 0x%04X rc=0 nz=%zu\n", tag, id, nz);
        if (nz > 2) {
            hex_dump_line("   PAYLOAD", resp, nz < 128 ? nz : 128);
        }
    }
    HOUT("SWEEP %-6s total=%d accepted=%d range=0x%04X..0x%04X\n",
         tag, total, accepted, lo, hi);
}

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

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        probe();
        return 0;
    }
    fprintf(stderr, "usage: %s probe\n", argv[0]);
    return 2;
}
