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
