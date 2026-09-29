// Minimal dependency-lifetime diagnostic, without SkyAPO or EAPO code.
#include <pipewire/pipewire.h>
int main() {
  pw_init(nullptr, nullptr);
  auto *loop = pw_main_loop_new(nullptr);
  auto *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
  auto *core = pw_context_connect(context, nullptr, 0);
  if (core)
    pw_core_disconnect(core);
  pw_context_destroy(context);
  pw_main_loop_destroy(loop);
  pw_deinit();
  return core ? 0 : 1;
}
