#include "gltrace.hpp"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

#if defined(__aarch64__)
constexpr unsigned kRelGlobDat = 1025;   // R_AARCH64_GLOB_DAT
constexpr unsigned kRelJumpSlot = 1026;  // R_AARCH64_JUMP_SLOT
#elif defined(__x86_64__)
constexpr unsigned kRelGlobDat = 6;   // R_X86_64_GLOB_DAT (so para testes no host)
constexpr unsigned kRelJumpSlot = 7;  // R_X86_64_JUMP_SLOT
#else
#error "arquitetura nao suportada"
#endif

constexpr int kInfo = 4;   // ANDROID_LOG_INFO
constexpr int kError = 6;  // ANDROID_LOG_ERROR

void say(int prio, const char* fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rsLogRaw(prio, msg);
}

// ------------------------------------------------------------- GOT patcher ---
struct FindCtx {
    const char* mod;
    uintptr_t bias = 0;
    const ElfW(Phdr)* ph = nullptr;
    int n = 0;
    bool found = false;
};

int findCb(struct dl_phdr_info* info, size_t, void* data) {
    auto* c = static_cast<FindCtx*>(data);
    if (info->dlpi_name && std::strstr(info->dlpi_name, c->mod)) {
        c->bias = info->dlpi_addr;
        c->ph = info->dlpi_phdr;
        c->n = info->dlpi_phnum;
        c->found = true;
        return 1;
    }
    return 0;
}

bool writeSlot(void** slot, void* value, uintptr_t relroStart, uintptr_t relroEnd) {
    const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto addr = reinterpret_cast<uintptr_t>(slot);
    const auto pageStart = addr & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(pageStart), page, PROT_READ | PROT_WRITE) != 0) return false;
    *slot = value;
    __builtin___clear_cache(reinterpret_cast<char*>(slot), reinterpret_cast<char*>(slot) + sizeof(void*));
    const bool inRelro = relroEnd > relroStart && addr >= relroStart && addr < relroEnd;
    if (inRelro) mprotect(reinterpret_cast<void*>(pageStart), page, PROT_READ);
    return true;
}

}  // namespace

namespace gltrace {

int patchGot(const char* module, GotPatch* list, int count) {
    FindCtx ctx;
    ctx.mod = module;
    dl_iterate_phdr(findCb, &ctx);
    if (!ctx.found) {
        say(kError, "patchGot: modulo %s nao encontrado", module);
        return 0;
    }
    const ElfW(Dyn)* dyn = nullptr;
    uintptr_t relroStart = 0, relroEnd = 0;
    for (int i = 0; i < ctx.n; ++i) {
        const auto& p = ctx.ph[i];
        if (p.p_type == PT_DYNAMIC) dyn = reinterpret_cast<const ElfW(Dyn)*>(ctx.bias + p.p_vaddr);
        if (p.p_type == PT_GNU_RELRO) {
            relroStart = ctx.bias + p.p_vaddr;
            relroEnd = relroStart + p.p_memsz;
        }
    }
    if (!dyn) {
        say(kError, "patchGot: sem PT_DYNAMIC");
        return 0;
    }
    auto abs = [&](uintptr_t p) { return p >= ctx.bias ? p : ctx.bias + p; };
    uintptr_t symtab = 0, strtab = 0, jmprel = 0, rela = 0;
    size_t pltrelsz = 0, relasz = 0;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB: symtab = abs(d->d_un.d_ptr); break;
            case DT_STRTAB: strtab = abs(d->d_un.d_ptr); break;
            case DT_JMPREL: jmprel = abs(d->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
            case DT_RELA: rela = abs(d->d_un.d_ptr); break;
            case DT_RELASZ: relasz = d->d_un.d_val; break;
            default: break;
        }
    }
    if (!symtab || !strtab) {
        say(kError, "patchGot: sem symtab/strtab");
        return 0;
    }
    const auto* syms = reinterpret_cast<const ElfW(Sym)*>(symtab);
    const char* strs = reinterpret_cast<const char*>(strtab);

    auto scan = [&](uintptr_t table, size_t bytes) {
        if (!table || !bytes) return;
        const auto* r = reinterpret_cast<const ElfW(Rela)*>(table);
        const size_t n = bytes / sizeof(ElfW(Rela));
        for (size_t i = 0; i < n; ++i) {
            const unsigned type = ELF64_R_TYPE(r[i].r_info);
            if (type != kRelGlobDat && type != kRelJumpSlot) continue;
            const char* name = strs + syms[ELF64_R_SYM(r[i].r_info)].st_name;
            for (int k = 0; k < count; ++k) {
                if (std::strcmp(name, list[k].name) != 0) continue;
                auto** slot = reinterpret_cast<void**>(ctx.bias + r[i].r_offset);
                void* cur = *slot;
                if (cur == list[k].detour) continue;  // ja trocado
                if (list[k].original && !*list[k].original) *list[k].original = cur;
                if (writeSlot(slot, list[k].detour, relroStart, relroEnd)) ++list[k].patched;
            }
        }
    };
    scan(jmprel, pltrelsz);
    scan(rela, relasz);

    int total = 0;
    for (int k = 0; k < count; ++k) total += list[k].patched;
    return total;
}

}  // namespace gltrace

// ------------------------------------------------------------------ tracer ---
namespace {

using GLenum = unsigned;
using GLuint = unsigned;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned;

constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;

enum EvType : uint8_t {
    E_BIND, E_VIEWPORT, E_SCISSOR, E_CLEAR, E_ENABLE, E_DISABLE, E_FBTEX, E_FBRB,
    E_DRAWE, E_DRAWA, E_SWAP
};

struct Event {
    uint8_t type;
    int8_t depth, blend, tid;
    int32_t a, b, c, d;
    uint32_t fb;
    uint32_t n;
    uint64_t sum;
};

constexpr size_t kMaxEvents = 60000;

std::atomic<int> g_state{0};  // 0 ocioso, 1 armado, 2 capturando
std::mutex g_mu;
std::vector<Event> g_ev;
std::vector<int> g_tids;
bool g_overflow = false;
int g_framesLeft = 0;

std::atomic<unsigned> g_fb{0};
std::atomic<int> g_depth{-1}, g_blend{-1};
void* g_dpy = nullptr;
void* g_surf = nullptr;
std::filesystem::path g_out;
int g_seq = 0;
bool g_installed = false;

inline bool capturing() { return g_state.load(std::memory_order_relaxed) == 2; }

int tidIndexLocked() {
    const int t = static_cast<int>(syscall(__NR_gettid));
    for (size_t i = 0; i < g_tids.size(); ++i)
        if (g_tids[i] == t) return static_cast<int>(i);
    g_tids.push_back(t);
    return static_cast<int>(g_tids.size() - 1);
}

void rec(EvType type, int a = 0, int b = 0, int c = 0, int d = 0) {
    std::lock_guard lock(g_mu);
    if (g_ev.size() >= kMaxEvents) {
        g_overflow = true;
        return;
    }
    Event e{};
    e.type = type;
    e.depth = static_cast<int8_t>(g_depth.load());
    e.blend = static_cast<int8_t>(g_blend.load());
    e.tid = static_cast<int8_t>(tidIndexLocked());
    e.a = a; e.b = b; e.c = c; e.d = d;
    e.fb = g_fb.load();
    e.n = 1;
    g_ev.push_back(e);
}

void recDraw(EvType type, int mode, int count) {
    std::lock_guard lock(g_mu);
    const int tid = tidIndexLocked();
    const int8_t dep = static_cast<int8_t>(g_depth.load());
    const int8_t bl = static_cast<int8_t>(g_blend.load());
    const unsigned fb = g_fb.load();
    if (!g_ev.empty()) {
        Event& l = g_ev.back();
        if (l.type == type && l.a == mode && l.fb == fb && l.depth == dep && l.blend == bl &&
            l.tid == tid) {
            ++l.n;
            l.sum += static_cast<uint64_t>(std::max(count, 0));
            return;
        }
    }
    if (g_ev.size() >= kMaxEvents) {
        g_overflow = true;
        return;
    }
    Event e{};
    e.type = type; e.depth = dep; e.blend = bl; e.tid = static_cast<int8_t>(tid);
    e.a = mode; e.fb = fb; e.n = 1; e.sum = static_cast<uint64_t>(std::max(count, 0));
    g_ev.push_back(e);
}

// ---- originais
void (*o_bindFb)(GLenum, GLuint) = nullptr;
void (*o_viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*o_scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*o_clear)(GLbitfield) = nullptr;
void (*o_enable)(GLenum) = nullptr;
void (*o_disable)(GLenum) = nullptr;
void (*o_fbTex)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
void (*o_fbRb)(GLenum, GLenum, GLenum, GLuint) = nullptr;
void (*o_drawE)(GLenum, GLsizei, GLenum, const void*) = nullptr;
void (*o_drawA)(GLenum, GLint, GLsizei) = nullptr;
unsigned (*o_swap)(void*, void*) = nullptr;

// ---- detours
void d_bindFb(GLenum t, GLuint fb) {
    if (t == GL_FRAMEBUFFER) g_fb.store(fb, std::memory_order_relaxed);
    if (capturing()) rec(E_BIND, static_cast<int>(t), static_cast<int>(fb));
    o_bindFb(t, fb);
}
void d_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (capturing()) rec(E_VIEWPORT, x, y, w, h);
    o_viewport(x, y, w, h);
}
void d_scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (capturing()) rec(E_SCISSOR, x, y, w, h);
    o_scissor(x, y, w, h);
}
void d_clear(GLbitfield m) {
    if (capturing()) rec(E_CLEAR, static_cast<int>(m));
    o_clear(m);
}
void d_enable(GLenum c) {
    if (c == GL_DEPTH_TEST) g_depth.store(1, std::memory_order_relaxed);
    if (c == GL_BLEND) g_blend.store(1, std::memory_order_relaxed);
    if (capturing()) rec(E_ENABLE, static_cast<int>(c));
    o_enable(c);
}
void d_disable(GLenum c) {
    if (c == GL_DEPTH_TEST) g_depth.store(0, std::memory_order_relaxed);
    if (c == GL_BLEND) g_blend.store(0, std::memory_order_relaxed);
    if (capturing()) rec(E_DISABLE, static_cast<int>(c));
    o_disable(c);
}
void d_fbTex(GLenum t, GLenum att, GLenum tt, GLuint tex, GLint lvl) {
    if (capturing()) rec(E_FBTEX, static_cast<int>(att), static_cast<int>(tex), lvl);
    o_fbTex(t, att, tt, tex, lvl);
}
void d_fbRb(GLenum t, GLenum att, GLenum rt, GLuint rb) {
    if (capturing()) rec(E_FBRB, static_cast<int>(att), static_cast<int>(rb));
    o_fbRb(t, att, rt, rb);
}
void d_drawE(GLenum mode, GLsizei count, GLenum type, const void* idx) {
    if (capturing()) recDraw(E_DRAWE, static_cast<int>(mode), count);
    o_drawE(mode, count, type, idx);
}
void d_drawA(GLenum mode, GLint first, GLsizei count) {
    if (capturing()) recDraw(E_DRAWA, static_cast<int>(mode), count);
    o_drawA(mode, first, count);
}

const char* capName(int c) {
    switch (c) {
        case 0x0B71: return "DEPTH_TEST";
        case 0x0BE2: return "BLEND";
        case 0x0B44: return "CULL_FACE";
        case 0x0C11: return "SCISSOR_TEST";
        case 0x0B90: return "STENCIL_TEST";
        case 0x8037: return "POLYGON_OFFSET_FILL";
        default: return nullptr;
    }
}
const char* attName(int a) {
    switch (a) {
        case 0x8CE0: return "COLOR0";
        case 0x8D00: return "DEPTH";
        case 0x8D20: return "STENCIL";
        case 0x821A: return "DEPTH_STENCIL";
        default: return nullptr;
    }
}
const char* modeName(int m) {
    switch (m) {
        case 0: return "POINTS"; case 1: return "LINES"; case 3: return "LINE_STRIP";
        case 4: return "TRIANGLES"; case 5: return "TRI_STRIP"; case 6: return "TRI_FAN";
        default: return "?";
    }
}

void dumpLocked() {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/gltrace-%d.txt", g_out.c_str(), ++g_seq);
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        say(kError, "nao consegui gravar %s", path);
        return;
    }
    int ew = -1, eh = -1;
    if (g_dpy && g_surf) {
        void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (egl) {
            using QS = unsigned (*)(void*, void*, int, int*);
            auto q = reinterpret_cast<QS>(dlsym(egl, "eglQuerySurface"));
            if (q) {
                q(g_dpy, g_surf, 0x3057, &ew);
                q(g_dpy, g_surf, 0x3056, &eh);
            }
            dlclose(egl);
        }
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "# RenderScale gltrace | superficie EGL=%dx%d | eventos=%zu%s | threads=%zu\n",
                  ew, eh, g_ev.size(), g_overflow ? " (TRUNCADO)" : "", g_tids.size());
    f << line;
    f << "# DRAW xN = N chamadas seguidas com o mesmo estado; verts = soma dos vertices/indices.\n"
         "# d=depth_test b=blend (-1 desconhecido) fb=framebuffer ligado t=indice da thread\n";

    int frame = 0;
    int draws = 0, runs = 0, clears = 0, drawsDepthOn = 0, drawsDepthOff = 0, evInFrame = 0;
    std::set<unsigned> fbs;
    std::set<std::tuple<int, int, int, int>> vps;
    auto endFrame = [&]() {
        if (evInFrame == 0) return;
        evInFrame = 0;
        std::snprintf(line, sizeof(line),
                      "## RESUMO frame %d: draws=%d (runs=%d) clears=%d depth_on=%d depth_off=%d "
                      "fbs=%zu viewports_distintos=%zu\n",
                      frame, draws, runs, clears, drawsDepthOn, drawsDepthOff, fbs.size(),
                      vps.size());
        f << line;
        draws = runs = clears = drawsDepthOn = drawsDepthOff = 0;
        fbs.clear();
        vps.clear();
    };
    for (const Event& e : g_ev) {
        switch (e.type) {
            case E_SWAP:
                if (frame > 0) endFrame();
                ++frame;
                std::snprintf(line, sizeof(line), "==== SWAP -> inicio do frame %d ====\n", frame);
                f << line;
                continue;
            case E_BIND:
                fbs.insert(e.fb);
                std::snprintf(line, sizeof(line), "[t%d] BIND fb=%u\n", e.tid, e.b);
                break;
            case E_VIEWPORT:
                vps.insert({e.a, e.b, e.c, e.d});
                std::snprintf(line, sizeof(line), "[t%d] VIEWPORT %d %d %d %d (fb=%u)\n", e.tid,
                              e.a, e.b, e.c, e.d, e.fb);
                break;
            case E_SCISSOR:
                std::snprintf(line, sizeof(line), "[t%d] SCISSOR %d %d %d %d\n", e.tid, e.a, e.b,
                              e.c, e.d);
                break;
            case E_CLEAR:
                ++clears;
                std::snprintf(line, sizeof(line), "[t%d] CLEAR mask=0x%X%s%s%s (fb=%u)\n", e.tid,
                              e.a, (e.a & 0x4000) ? " COLOR" : "", (e.a & 0x100) ? " DEPTH" : "",
                              (e.a & 0x400) ? " STENCIL" : "", e.fb);
                break;
            case E_ENABLE:
            case E_DISABLE: {
                const char* n = capName(e.a);
                char nb[16];
                if (!n) { std::snprintf(nb, sizeof(nb), "0x%X", e.a); n = nb; }
                std::snprintf(line, sizeof(line), "[t%d] %s %s\n", e.tid,
                              e.type == E_ENABLE ? "ENABLE" : "DISABLE", n);
                break;
            }
            case E_FBTEX:
            case E_FBRB: {
                const char* n = attName(e.a);
                char nb[16];
                if (!n) { std::snprintf(nb, sizeof(nb), "0x%X", e.a); n = nb; }
                std::snprintf(line, sizeof(line), "[t%d] FB_%s fb=%u attach=%s id=%d\n", e.tid,
                              e.type == E_FBTEX ? "TEXTURE" : "RENDERBUFFER", e.fb, n, e.b);
                break;
            }
            case E_DRAWE:
            case E_DRAWA:
                draws += static_cast<int>(e.n);
                ++runs;
                (e.depth == 1 ? drawsDepthOn : drawsDepthOff) += static_cast<int>(e.n);
                std::snprintf(line, sizeof(line),
                              "[t%d] DRAW%s x%u %s verts=%llu (fb=%u d=%d b=%d)\n", e.tid,
                              e.type == E_DRAWE ? "_ELEMENTS" : "_ARRAYS", e.n, modeName(e.a),
                              static_cast<unsigned long long>(e.sum), e.fb, e.depth, e.blend);
                break;
            default:
                continue;
        }
        ++evInFrame;
        f << line;
    }
    if (frame > 0) endFrame();
    say(kInfo, "gltrace gravado em %s (%zu eventos)", path, g_ev.size());
}

unsigned d_swap(void* dpy, void* surf) {
    g_dpy = dpy;
    g_surf = surf;
    const int st = g_state.load(std::memory_order_relaxed);
    if (st == 1) {
        {
            std::lock_guard lock(g_mu);
            g_ev.clear();
            g_ev.reserve(8192);
            g_tids.clear();
            g_overflow = false;
        }
        g_framesLeft = 2;
        g_state.store(2);
        rec(E_SWAP);
    } else if (st == 2) {
        rec(E_SWAP);
        if (--g_framesLeft <= 0) {
            g_state.store(0);
            std::lock_guard lock(g_mu);
            dumpLocked();
        }
    }
    return o_swap(dpy, surf);
}

}  // namespace

namespace gltrace {

bool installed() { return g_installed; }

void arm() {
    if (!g_installed) {
        say(kError, "captura pedida, mas as interceptacoes GL nao estao instaladas");
        return;
    }
    g_state.store(1);
    say(kInfo, "captura ARMADA: vou gravar 2 frames no proximo swap");
}

bool install(const std::filesystem::path& outDir) {
    if (g_installed) return true;
    g_out = outDir;
    GotPatch p[] = {
        {"glBindFramebuffer", reinterpret_cast<void*>(d_bindFb), reinterpret_cast<void**>(&o_bindFb)},
        {"glViewport", reinterpret_cast<void*>(d_viewport), reinterpret_cast<void**>(&o_viewport)},
        {"glScissor", reinterpret_cast<void*>(d_scissor), reinterpret_cast<void**>(&o_scissor)},
        {"glClear", reinterpret_cast<void*>(d_clear), reinterpret_cast<void**>(&o_clear)},
        {"glEnable", reinterpret_cast<void*>(d_enable), reinterpret_cast<void**>(&o_enable)},
        {"glDisable", reinterpret_cast<void*>(d_disable), reinterpret_cast<void**>(&o_disable)},
        {"glFramebufferTexture2D", reinterpret_cast<void*>(d_fbTex), reinterpret_cast<void**>(&o_fbTex)},
        {"glFramebufferRenderbuffer", reinterpret_cast<void*>(d_fbRb), reinterpret_cast<void**>(&o_fbRb)},
        {"glDrawElements", reinterpret_cast<void*>(d_drawE), reinterpret_cast<void**>(&o_drawE)},
        {"glDrawArrays", reinterpret_cast<void*>(d_drawA), reinterpret_cast<void**>(&o_drawA)},
        {"eglSwapBuffers", reinterpret_cast<void*>(d_swap), reinterpret_cast<void**>(&o_swap)},
    };
    const int n = static_cast<int>(sizeof(p) / sizeof(p[0]));
    const int total = patchGot("libminecraftpe.so", p, n);
    int missing = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i].patched == 0) {
            ++missing;
            say(kError, "GOT: %s NAO interceptada", p[i].name);
        }
    }
    say(kInfo, "GOT: %d slots trocados (%d funcoes sem slot)", total, missing);
    // Sem o swap e sem os originais nao da pra operar com seguranca.
    if (!o_swap || !o_bindFb || !o_viewport || !o_scissor || !o_clear || !o_enable || !o_disable ||
        !o_fbTex || !o_fbRb || !o_drawE || !o_drawA) {
        say(kError, "interceptacoes GL incompletas; captura indisponivel");
        return false;
    }
    g_installed = true;
    return true;
}

}  // namespace gltrace
