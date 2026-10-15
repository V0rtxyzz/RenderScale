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
