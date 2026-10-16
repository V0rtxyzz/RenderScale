// RenderScale (port Bedrock) - mod nativo para LeviLauncher.
//
// Ideia: o renderer "dragon" do jogo ja possui um controlador de resolucao
// dinamica (mce::framebuilder::DynamicResolutionController) que, a cada frame,
// calcula a escala do 3D e a entrega ao passe TAAU. Hookamos o update desse
// controlador e, depois do original, gravamos a escala escolhida no slider.
//
// Layout do controlador (versao 26.52, ver README):
//   obj+0x00 (double): escala atual (1.0 = 100%)      [usado se obj+0x10 == 0]
//   obj+0x10 (u8)    : flag; se == 1 o jogo le a escala em obj+0x90
//
// So tem efeito com o upscaling ligado (Video > Upscaling) e pipeline deferred.

#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gltrace.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#define LOG_TAG "RenderScale"
#define RS_VERSION "0.3.0-redirect"

namespace {
void logLine(int prio, const char* fmt, ...);
}
#define LOGI(...) logLine(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGE(...) logLine(ANDROID_LOG_ERROR, __VA_ARGS__)

namespace {

std::filesystem::path g_logPath;  // renderscale.log, ao lado do config.json
std::mutex g_logMutex;

void logLine(int prio, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    __android_log_print(prio, LOG_TAG, "%s", msg);
    if (g_logPath.empty()) return;
    std::lock_guard lock(g_logMutex);
    std::ofstream out(g_logPath, std::ios::app);
    if (!out) return;
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    out << ts << (prio == ANDROID_LOG_ERROR ? " E " : " I ") << msg << '\n';
}

constexpr const char* kModuleId = "renderscale";
constexpr float kMinScale = 0.30f;
constexpr float kMaxScale = 1.00f;
constexpr float kDefaultScale = 0.75f;

// DynamicResolutionController::update(this, double, double)  -- 1.26.52
// Gerada com mksig.py (--addr 0x10fcdfec, 192 bytes); unica em .text.
constexpr const char* kSigDrcUpdate =
    "09 00 66 9E ?? ?? ?? ?? ?? ?? ?? ?? 3F 01 08 EB ?? ?? ?? ?? 18 20 60 1E"
    " ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 00 38 62 1E 60 08 60 1E"
    " 40 28 60 1E ?? ?? ?? ?? 29 00 66 9E ?? ?? ?? ?? 3F 01 08 EB ?? ?? ?? ??"
    " 38 20 60 1E ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 21 38 60 1E"
    " 41 08 61 1E 00 28 61 1E ?? ?? ?? ?? C0 03 5F D6 ?? ?? ?? ?? ?? ?? ?? ??"
    " ?? ?? ?? ?? 29 00 66 9E ?? ?? ?? ?? 3F 01 08 EB ?? ?? ?? ?? C0 03 5F D6"
    " ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? C0 03 5F D6 ?? ?? ?? ?? ?? ?? ?? ??"
    " ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 42 C0 22 1E 41 38 61 1E 21 18 62 1E";

std::atomic<bool> g_enabled{true};
std::atomic<float> g_scale{kDefaultScale};
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_menuRegistered{false};
std::atomic<bool> g_modEnabled{false};
std::atomic<uint64_t> g_frames{0};

std::filesystem::path g_configPath;
std::mutex g_installMutex;

void* g_drcTarget = nullptr;
void (*g_drcOriginal)(void*, double, double) = nullptr;

void* (*g_dlopenOrig)(const char*, int) = nullptr;
void* g_dlopenTarget = nullptr;
thread_local bool g_inInstall = false;

// ---------------------------------------------------------------- config ----
float clampScale(float v) {
    if (!(v == v)) return kDefaultScale;  // NaN
    return std::clamp(v, kMinScale, kMaxScale);
}

void loadConfig() {
    if (g_configPath.empty()) return;
    std::ifstream in(g_configPath);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const auto key = text.find("\"scale\"");
    if (key != std::string::npos) {
        const auto colon = text.find(':', key);
        if (colon != std::string::npos) {
            char* end = nullptr;
            const float v = std::strtof(text.c_str() + colon + 1, &end);
            if (end != text.c_str() + colon + 1) g_scale.store(clampScale(v));
        }
    }
    const auto en = text.find("\"enabled\"");
    if (en != std::string::npos) {
        auto colon = text.find(':', en);
        if (colon != std::string::npos) {
            ++colon;
            while (colon < text.size() && text[colon] == ' ') ++colon;
            g_enabled.store(text.compare(colon, 4, "true") == 0);
        }
    }
}

void saveConfig() {
    if (g_configPath.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(g_configPath.parent_path(), ec);
    std::ofstream out(g_configPath, std::ios::trunc);
    if (!out) return;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\n  \"enabled\": %s,\n  \"scale\": %.3f\n}\n",
                  g_enabled.load() ? "true" : "false", static_cast<double>(g_scale.load()));
    out << buf;
}

// ------------------------------------------------------------------ hook ----
// Estatisticas da janela atual (so a thread de render chama o detour).
double g_winMin = 1e9, g_winMax = -1e9;
bool g_warnedUnexpected = false;

void drcUpdateDetour(void* self, double a, double b) {
    g_drcOriginal(self, a, b);
    if (!self) return;
    const std::uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;

    auto* base = static_cast<std::uint8_t*>(self);
    const std::uint8_t flag = base[0x10];
    auto* slot = reinterpret_cast<double*>(flag == 1 ? base + 0x90 : base);
    const double game = *slot;  // escala que o proprio jogo acabou de calcular

    if (n == 1) {
        LOGI("hook ATIVO: update chamado pela 1a vez (obj=%p flag=%u valor_do_jogo=%.4f "
             "arg0=%.3f arg1=%.3f)",
             self, static_cast<unsigned>(flag), game, a, b);
    }

    const bool plausible = (game == 0.0) || (game >= 0.05 && game <= 8.0);
    const bool active = g_modEnabled.load(std::memory_order_relaxed) &&
                        g_enabled.load(std::memory_order_relaxed);
    const double mine = static_cast<double>(g_scale.load(std::memory_order_relaxed));

    if (active) {
        if (plausible) {
            *slot = mine;
        } else if (!g_warnedUnexpected) {
            g_warnedUnexpected = true;
            LOGE("campo inesperado (valor=%g): NAO vou escrever. Layout diferente nesta versao?",
                 game);
        }
    }

    if (plausible) {
        g_winMin = std::min(g_winMin, game);
        g_winMax = std::max(g_winMax, game);
    }
    if (n % 600 == 0) {
        LOGI("frames=%llu | jogo calculou min=%.3f max=%.3f | aplicado=%.3f | modulo=%s",
             static_cast<unsigned long long>(n), g_winMin, g_winMax, active ? mine : game,
             active ? "ligado" : "desligado");
        g_winMin = 1e9;
        g_winMax = -1e9;
    }
}

bool installHook() {
    std::lock_guard lock(g_installMutex);
    if (g_hooked.load()) return true;

    std::vector<std::string> patterns{kSigDrcUpdate};
    const auto resolved = pl::memory::resolveSignatures(patterns, "libminecraftpe.so");
    const auto it = resolved.find(patterns[0]);
    LOGI("procurando assinatura do DynamicResolutionController::update...");
    if (it == resolved.end() || it->second == 0) {
        LOGE("assinatura do DynamicResolutionController::update nao encontrada "
             "(versao do Minecraft diferente de 1.26.52?)");
        return false;
    }
    g_drcTarget = reinterpret_cast<void*>(it->second);
    {
        Dl_info info{};
        if (dladdr(g_drcTarget, &info) && info.dli_fbase) {
            LOGI("assinatura encontrada em %p (offset 0x%llx no libminecraftpe.so; esperado "
                 "0x10fcdfec na 1.26.52)",
                 g_drcTarget,
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_drcTarget) -
                                                 reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
        } else {
            LOGI("assinatura encontrada em %p", g_drcTarget);
        }
    }
    if (pl::memory::hook(g_drcTarget, reinterpret_cast<void*>(drcUpdateDetour),
                         reinterpret_cast<void**>(&g_drcOriginal)) != 0) {
        LOGE("falha ao instalar o hook em %p", g_drcTarget);
        g_drcTarget = nullptr;
        return false;
    }
    g_hooked.store(true);
    LOGI("hook instalado. Agora ligue o Upscaling em Configuracoes > Video e va para um mundo; "
         "deve aparecer 'hook ATIVO' aqui.");
    return true;
}

void installGlTrace();

// libminecraftpe.so ainda pode nao estar mapeada quando o mod carrega:
// observamos o dlopen e instalamos o hook assim que ela aparecer.
void* dlopenDetour(const char* filename, int flags) {
    void* handle = g_dlopenOrig ? g_dlopenOrig(filename, flags) : nullptr;
    if (handle && filename && std::strstr(filename, "libminecraftpe.so") && !g_inInstall &&
        g_modEnabled.load()) {
        g_inInstall = true;
        LOGI("libminecraftpe.so carregada; instalando hook");
        installHook();
        installGlTrace();
        g_inInstall = false;
    }
    return handle;
}

void watchDlopen() {
    if (g_dlopenTarget) return;
    void* libdl = dlopen("libdl.so", RTLD_NOW | RTLD_NOLOAD);
    if (!libdl) libdl = dlopen("libdl.so", RTLD_NOW);
    if (!libdl) return;
    void* sym = dlsym(libdl, "dlopen");
    dlclose(libdl);
    if (!sym) return;
    if (pl::memory::hook(sym, reinterpret_cast<void*>(dlopenDetour),
                         reinterpret_cast<void**>(&g_dlopenOrig)) == 0) {
        g_dlopenTarget = sym;
    }
}

void installGlTrace() {
    if (g_logPath.empty()) return;
    gltrace::install(g_logPath.parent_path());
}

bool minecraftMapped() {
    void* h = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    dlclose(h);
    return true;
}

// ------------------------------------------------------------------ menu ----
void onToggle(std::string_view /*moduleId*/, bool enabled) {
    g_enabled.store(enabled);
    gltrace::setEnabled(enabled);
    saveConfig();
    LOGI("modulo %s pelo menu", enabled ? "LIGADO" : "DESLIGADO");
}

void onConfigChanged(std::string_view /*moduleId*/, std::string_view key, std::string_view value) {
    if (key == "capture") {  // qualquer mudanca do interruptor arma uma captura
        gltrace::arm();
        return;
    }
    if (key != "scale") return;
    const std::string text(value);
    char* end = nullptr;
    const float v = std::strtof(text.c_str(), &end);
    if (end == text.c_str()) return;
    g_scale.store(clampScale(v));
    gltrace::setScale(g_scale.load());
    saveConfig();
    static std::atomic<long long> lastLogMs{0};
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    if (nowMs - lastLogMs.load() > 400) {  // o slider dispara dezenas de eventos por segundo
        lastLogMs.store(nowMs);
        LOGI("escala alterada para %.2f (update chamado %llu vezes ate agora)",
             static_cast<double>(g_scale.load()),
             static_cast<unsigned long long>(g_frames.load()));
    }
}

void registerMenu() {
    if (g_menuRegistered.exchange(true)) return;
    char def[32], mn[32], mx[32];
    std::snprintf(def, sizeof(def), "%.2f", static_cast<double>(g_scale.load()));
    std::snprintf(mn, sizeof(mn), "%.2f", static_cast<double>(kMinScale));
    std::snprintf(mx, sizeof(mx), "%.2f", static_cast<double>(kMaxScale));

    pl::modmenu::ModuleBuilder builder(kModuleId, "RenderScale");
    builder.description("Escala a resolucao so do 3D; o HUD fica nitido. Sem Vibrant Visuals usa um "
                        "framebuffer menor; com Vibrant Visuals usa o upscaling do jogo.")
        .defaultEnabled(g_enabled.load())
        .hideInHudEditor(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfigChanged)
        .config("scale", "Render Scale", pl::modmenu::ConfigType::SliderFloat, def, mn, mx, "")
        .config("capture", "Capturar frames GL (mude o interruptor)",
                pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    if (!builder.registerModule()) LOGE("falha ao registrar o modulo no Mod Menu");
}

}  // namespace

void rsLogRaw(int prio, const char* msg) { logLine(prio, "%s", msg); }

// ------------------------------------------------------------------- mod ----
class RenderScaleMod {
public:
    static RenderScaleMod& instance() {
        static RenderScaleMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) {
        g_configPath = context.configDir() / "config.json";
        g_logPath = context.configDir() / "renderscale.log";
        std::error_code ec;
        std::filesystem::create_directories(context.configDir(), ec);
        if (std::filesystem::exists(g_logPath, ec) &&
            std::filesystem::file_size(g_logPath, ec) > 512 * 1024) {
            std::filesystem::remove(g_logPath, ec);
        }
        loadConfig();
        gltrace::setScale(g_scale.load());
        gltrace::setEnabled(g_enabled.load());
        LOGI("==== RenderScale %s carregado | escala=%.2f | modulo=%s ====", RS_VERSION,
             static_cast<double>(g_scale.load()), g_enabled.load() ? "ligado" : "desligado");
        LOGI("config: %s", g_configPath.c_str());
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        g_modEnabled.store(true);
        gltrace::setEnabled(g_enabled.load());
        registerMenu();
        if (minecraftMapped()) {
            LOGI("enable: libminecraftpe.so ja mapeada");
            installHook();
            installGlTrace();
        } else {
            LOGI("enable: libminecraftpe.so ainda nao mapeada; aguardando dlopen");
            watchDlopen();
        }
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        g_modEnabled.store(false);  // o detour passa a ser transparente
        gltrace::setEnabled(false);
        saveConfig();
        LOGI("mod desativado");
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        g_modEnabled.store(false);
        gltrace::setEnabled(false);
        saveConfig();
        return true;
    }
};

PL_REGISTER_MOD(RenderScaleMod, RenderScaleMod::instance())
