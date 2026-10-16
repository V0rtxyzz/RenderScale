#pragma once
#include <filesystem>

// Diagnostico do renderer GLES do jogo: intercepta (via GOT do libminecraftpe.so) as chamadas
// GL principais e grava a linha do tempo de 2 frames em gltrace-N.txt.
namespace gltrace {
// Instala as interceptacoes (idempotente). outDir = pasta onde gravar gltrace-N.txt.
bool install(const std::filesystem::path& outDir);
// Arma a captura: no proximo eglSwapBuffers comeca a gravar 2 frames e depois grava o arquivo.
void arm();
bool installed();

// Escala so do 3D: o frame do jogo (antes do HUD) vai para um framebuffer menor e e esticado
// de volta; o HUD continua em resolucao total.
void setScale(float scale);   // 0.30 .. 1.00 (>= 0.995 desliga o redirecionamento)
void setEnabled(bool on);

// Substitui entradas da GOT de `module` (nome parcial, ex. "libminecraftpe.so").
struct GotPatch {
    const char* name;
    void* detour;
    void** original;
    int patched = 0;  // saida: quantos slots foram trocados
};
int patchGot(const char* module, GotPatch* list, int count);
}  // namespace gltrace

// Implementado em main.cpp (grava no log do mod).
void rsLogRaw(int prio, const char* msg);
